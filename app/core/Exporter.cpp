// Exporter — see header. Markdown emitter v1.

#include "Exporter.h"
#include "BlockModel.h"
#include "MediaStore.h"

#include "../notes/annotation_io.h"
#include "../notes/annotation_note.h"
#include "../notes/annotation_serializer.h"
#include "../notes/annotation_thumbnail.h"
#include "../notes/doc_ink.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTime>
#include <QUrl>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace {

// ---------- small helpers ----------

QString localPathOf(const QString& urlOrPath) {
    const QUrl u(urlOrPath);
    return u.isLocalFile() ? u.toLocalFile() : urlOrPath;
}

// Escape markdown punctuation in plain prose. NOT applied inside code spans
// or code blocks. Line-start-only hazards ('#'/'>' openers) are left alone:
// the autoformat triggers already convert such content to real blocks, so
// prose starting with them is rare enough to accept in v1.
QString escapeMd(const QString& in) {
    QString out;
    out.reserve(in.size() + 8);
    for (const QChar ch : in) {
        switch (ch.unicode()) {
        case '\\': case '`': case '*': case '_':
        case '[': case ']': case '~': case '<':
            out += QLatin1Char('\\');
            break;
        default:
            break;
        }
        out += ch;
    }
    return out;
}

// Link/image destination: angle-bracket when the target carries characters
// that break the bare () form (spaces, parens — production paths do).
QString mdDest(const QString& target) {
    if (target.contains(QLatin1Char(' ')) || target.contains(QLatin1Char('('))
        || target.contains(QLatin1Char(')')))
        return QStringLiteral("<") + target + QStringLiteral(">");
    return target;
}

QString humanSize(qint64 bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return u == 0 ? QStringLiteral("%1 B").arg(bytes)
                  : QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1String(units[u]));
}

QString humanDuration(qreal ms) {
    const int totalS = static_cast<int>(ms / 1000.0 + 0.5);
    const int h = totalS / 3600, m = (totalS % 3600) / 60, s = totalS % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h)
                       .arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

// ---------- inline span emission ----------

struct StyleRun {
    int s = 0, e = 0;
    uint8_t kind = 0;
    QString u;       // link target
    QString delim;   // code spans: chosen backtick delimiter
    bool pad = false; // code spans: space-pad inside the delimiters
    bool operator==(const StyleRun& o) const {
        return kind == o.kind && u == o.u && s == o.s && e == o.e;
    }
};

// Nesting preference, outermost first. Code is innermost on purpose:
// markdown inside backticks is literal, so **`x`** works and `**x**` doesn't.
int rankOf(uint8_t k) {
    switch (k) {
    case BlockModel::SpanLink:      return 0;
    case BlockModel::SpanBold:      return 1;
    case BlockModel::SpanItalic:    return 2;
    case BlockModel::SpanStrike:    return 3;
    case BlockModel::SpanUnderline: return 4;
    case BlockModel::SpanCode:      return 5;
    }
    return 9;
}

QString openMarker(const StyleRun& r) {
    switch (r.kind) {
    case BlockModel::SpanLink:      return QStringLiteral("[");
    case BlockModel::SpanBold:      return QStringLiteral("**");
    case BlockModel::SpanItalic:    return QStringLiteral("*");
    case BlockModel::SpanStrike:    return QStringLiteral("~~");
    case BlockModel::SpanUnderline: return QStringLiteral("<u>");
    case BlockModel::SpanCode:      return r.delim + (r.pad ? QStringLiteral(" ") : QString());
    }
    return {};
}

QString closeMarker(const StyleRun& r) {
    switch (r.kind) {
    case BlockModel::SpanLink:
        return QStringLiteral("](") + mdDest(r.u) + QStringLiteral(")");
    case BlockModel::SpanBold:      return QStringLiteral("**");
    case BlockModel::SpanItalic:    return QStringLiteral("*");
    case BlockModel::SpanStrike:    return QStringLiteral("~~");
    case BlockModel::SpanUnderline: return QStringLiteral("</u>");
    case BlockModel::SpanCode:      return (r.pad ? QStringLiteral(" ") : QString()) + r.delim;
    }
    return {};
}

int maxBacktickRun(const QString& s) {
    int best = 0, run = 0;
    for (const QChar c : s) {
        run = (c == QLatin1Char('`')) ? run + 1 : 0;
        best = std::max(best, run);
    }
    return best;
}

// Footnote bookkeeping across the whole document.
struct FootnoteCtx {
    QStringList threadIds;                 // in first-marker order
    int numberFor(const QString& id) {
        int i = threadIds.indexOf(id);
        if (i < 0) { threadIds.append(id); i = threadIds.size() - 1; }
        return i + 1;
    }
};

// One block's text + spans → markdown with markers, footnote refs recorded.
QString emitInline(const BlockModel* m, int row, FootnoteCtx& fn) {
    const QString text = m->contentForRow(row);
    const int len = text.size();

    std::vector<StyleRun> styles;
    std::map<int, QList<int>> notesAt;   // char pos → footnote numbers

    const QVariantList spans = m->spansForRow(row);
    for (const QVariant& v : spans) {
        const QVariantMap sp = v.toMap();
        const uint8_t k = static_cast<uint8_t>(sp.value(QStringLiteral("k")).toInt());
        int s = std::clamp(sp.value(QStringLiteral("s")).toInt(), 0, len);
        int e = std::clamp(sp.value(QStringLiteral("e")).toInt(), 0, len);
        if (k == BlockModel::SpanComment) {
            if (e >= s) notesAt[e].append(fn.numberFor(sp.value(QStringLiteral("u")).toString()));
            continue;
        }
        if (k == BlockModel::SpanFgColor || k == BlockModel::SpanHighlight)
            continue;   // dropped in markdown (ruling: HTML carries fidelity)
        if (s >= e) continue;
        StyleRun r{s, e, k, sp.value(QStringLiteral("u")).toString(), {}, false};
        if (k == BlockModel::SpanCode) {
            const QString body = text.mid(s, e - s);
            const int run = maxBacktickRun(body);
            r.delim = QString(std::max(1, run + 1), QLatin1Char('`'));
            r.pad = body.startsWith(QLatin1Char('`')) || body.endsWith(QLatin1Char('`'));
        } else {
            // Emphasis can't wrap leading/trailing whitespace — shrink the
            // span; the whitespace still emits, just outside the markers.
            while (r.s < r.e && text.at(r.s).isSpace()) ++r.s;
            while (r.e > r.s && text.at(r.e - 1).isSpace()) --r.e;
            if (r.s >= r.e) continue;
        }
        styles.push_back(r);
    }

    std::set<int> bounds{0, len};
    for (const StyleRun& r : styles) { bounds.insert(r.s); bounds.insert(r.e); }
    for (const auto& kv : notesAt) bounds.insert(std::clamp(kv.first, 0, len));

    QString out;
    std::vector<StyleRun> stack;
    auto desiredAt = [&](int a, int b) {
        std::vector<StyleRun> d;
        for (const StyleRun& r : styles)
            if (r.s <= a && r.e >= b) d.push_back(r);
        std::sort(d.begin(), d.end(), [](const StyleRun& x, const StyleRun& y) {
            if (rankOf(x.kind) != rankOf(y.kind)) return rankOf(x.kind) < rankOf(y.kind);
            if (x.s != y.s) return x.s < y.s;
            return x.u < y.u;
        });
        return d;
    };

    // Emphasis markers can't sit against whitespace (CommonMark flanking
    // rules), so each segment's edge whitespace is emitted OUTSIDE any
    // markers that open/close at its boundaries: leading ws goes out before
    // this segment's opens; trailing ws is DEFERRED and lands after the next
    // boundary's closes (a plain append when nothing closes — same position).
    QString deferredWs;
    auto it = bounds.begin();
    int prev = *it;
    for (++it; it != bounds.end(); ++it) {
        const int a = prev, b = *it;
        prev = *it;
        if (a >= b) continue;
        const std::vector<StyleRun> desired = desiredAt(a, b);
        // Close down to the common prefix, then open the rest.
        size_t common = 0;
        while (common < stack.size() && common < desired.size()
               && stack[common] == desired[common]) ++common;
        while (stack.size() > common) { out += closeMarker(stack.back()); stack.pop_back(); }
        out += deferredWs;
        deferredWs.clear();
        if (const auto nf = notesAt.find(a); nf != notesAt.end())
            for (int n : nf->second) out += QStringLiteral("[^%1]").arg(n);

        QString seg = text.mid(a, b - a);
        const bool inCode = std::any_of(desired.begin(), desired.end(), [](const StyleRun& r) {
            return r.kind == BlockModel::SpanCode; });
        if (!inCode) {
            int lead = 0;
            while (lead < seg.size() && seg.at(lead).isSpace()) ++lead;
            out += seg.left(lead);
            seg = seg.mid(lead);
            int trail = 0;
            while (trail < seg.size() && seg.at(seg.size() - 1 - trail).isSpace()) ++trail;
            if (trail > 0) { deferredWs = seg.right(trail); seg.chop(trail); }
        }
        for (size_t i = common; i < desired.size(); ++i) {
            out += openMarker(desired[i]);
            stack.push_back(desired[i]);
        }
        out += inCode ? seg : escapeMd(seg);
    }
    while (!stack.empty()) { out += closeMarker(stack.back()); stack.pop_back(); }
    out += deferredWs;
    if (const auto nf = notesAt.find(len); nf != notesAt.end())
        for (int n : nf->second) out += QStringLiteral("[^%1]").arg(n);
    return out;
}

// ---------- table ----------

QString cellMd(const BlockModel* m, int row, int r, int c, Exporter::AssetSink& sink) {
    QString out;
    const int kind = m->tableColumnKind(row, c);
    if (kind == 2) {
        switch (m->tableCellCheck(row, r, c)) {
        case 1:  out = QStringLiteral("[/]"); break;
        case 2:  out = QStringLiteral("[x]"); break;
        default: out = QStringLiteral("[ ]"); break;
        }
    } else if (kind == 1) {
        out = escapeMd(m->tableCellChoiceLabel(row, r, c));
    } else {
        if (!m->tableCellMedia(row, r, c).isEmpty()) {
            const QString p = localPathOf(m->tableCellMediaUrl(row, r, c));
            const QString rel = sink.addFile(p, QFileInfo(p).completeBaseName());
            if (!rel.isEmpty())
                out += QStringLiteral("![](%1) ").arg(mdDest(rel));
        }
        out += escapeMd(m->tableCell(row, r, c));
    }
    out.replace(QStringLiteral("|"), QStringLiteral("\\|"));
    out.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    return out;
}

QString emitTable(const BlockModel* m, int row, Exporter::AssetSink& sink) {
    const int rows = m->tableRows(row), cols = m->tableColumns(row);
    const int hdr = m->tableHeaderRows(row);
    if (rows <= 0 || cols <= 0) return {};
    QString out;
    // Pipe tables require a header row: use the table's first header row, or
    // synthesize an empty one. Extra header rows fall through to the body
    // (documented lossiness).
    out += QStringLiteral("|");
    for (int c = 0; c < cols; ++c)
        out += QStringLiteral(" %1 |").arg(hdr > 0 ? cellMd(m, row, 0, c, sink) : QString());
    out += QStringLiteral("\n|");
    for (int c = 0; c < cols; ++c) {
        switch (m->tableColAlign(row, c)) {
        case 1:  out += QStringLiteral(" :---: |"); break;
        case 2:  out += QStringLiteral(" ---: |"); break;
        default: out += QStringLiteral(" --- |"); break;
        }
    }
    for (int r = (hdr > 0 ? 1 : 0); r < rows; ++r) {
        out += QStringLiteral("\n|");
        for (int c = 0; c < cols; ++c)
            out += QStringLiteral(" %1 |").arg(cellMd(m, row, r, c, sink));
    }
    return out;
}

// ---------- media ----------

QString sanitizedBase(const QString& path) {
    return qcv::annotation_io::sanitizeMediaName(QFileInfo(path).fileName());
}

// Margin ink on a MEDIA block is frame-normalized (it scales with the
// media), so it exports EXACTLY: strokes rendered to a transparent PNG the
// size of the exported image/poster, stacked as a z-layer wired to the
// page's Annotations toggle. (Text-block ink is px-anchored to the app's
// layout and stays out — layering can't fix anchoring.)
QImage renderMediaInk(const BlockModel* m, int row, const QSize& target) {
    mn::DocInkAnchor a;
    if (!mn::docInkFromJson(m->inkForRow(row), a) || a.strokes.empty()
        || a.space != mn::DocInkAnchor::Frame || target.isEmpty())
        return {};
    QImage img(target, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    // strokeWidth is stored in media-INTRINSIC px (the doc_ink convention).
    const double ws = double(target.width()) / std::max(1, m->mediaW(row));
    for (const qcv::ActiveStroke& s : a.strokes)
        qcv::paintStroke(p, s, target.width(), target.height(), ws);
    p.end();
    return img;
}

// Page ink on a TEXT-ish block (px space: X from page center, Y from block
// top). Rendered at 2x into a bbox-cropped transparent PNG and absolutely
// positioned inside the block element: X is EXACT (the export column is the
// same fixed 760 the ink was drawn against); Y is exact to the block top,
// approximate within multi-line blocks when the browser wraps differently.
// pointer-events:none — never interactive. The size guard drops corrupt
// blobs (pre-oval-fix radii) instead of exploding the canvas.
struct TextInk { QImage img; QRectF box; };
TextInk renderTextInk(const BlockModel* m, int row) {
    TextInk out;
    mn::DocInkAnchor a;
    if (!mn::docInkFromJson(m->inkForRow(row), a) || a.strokes.empty()
        || a.space != mn::DocInkAnchor::Px)
        return out;
    QRectF box;
    for (const qcv::ActiveStroke& st : a.strokes) {
        QRectF b = qcv::strokeBoundsNorm(st);
        const double pad = std::max<double>(2.0, st.strokeWidth);
        b.adjust(-pad, -pad, pad, pad);
        box = box.isNull() ? b : box.united(b);
    }
    const int W = int(std::ceil(box.width() * 2.0));
    const int H = int(std::ceil(box.height() * 2.0));
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192) return out;
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(2.0, 2.0);
    p.translate(-box.left(), -box.top());
    for (const qcv::ActiveStroke& st : a.strokes)
        qcv::paintStroke(p, st, 1.0, 1.0, 1.0);
    p.end();
    out.img = img;
    out.box = box;
    return out;
}

// Rasterize a sketch block (strokes + placed images) to a 2x transparent PNG.
QImage renderSketch(const BlockModel* m, int row) {
    const QString json = m->sketchResolvedJson(row);
    const QJsonObject obj = QJsonDocument::fromJson(json.toUtf8()).object();
    const int w = obj.value(QStringLiteral("w")).toInt(480);
    const int h = obj.value(QStringLiteral("h")).toInt(480);
    if (w <= 0 || h <= 0) return {};
    const int W = w * 2, H = h * 2;
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const QJsonValue& v : obj.value(QStringLiteral("images")).toArray()) {
        const QJsonObject im = v.toObject();
        const QImage src(localPathOf(im.value(QStringLiteral("src")).toString()));
        if (src.isNull()) continue;
        p.drawImage(QRectF(im.value(QStringLiteral("x")).toDouble() * W,
                           im.value(QStringLiteral("y")).toDouble() * H,
                           im.value(QStringLiteral("w")).toDouble() * W,
                           im.value(QStringLiteral("h")).toDouble() * H),
                    src);
    }
    // strokeWidth is stored in SOURCE px (the w-wide canvas) — scale to 2x.
    for (const qcv::ActiveStroke& s : qcv::AnnotationSerializer::jsonStringToStrokes(json))
        qcv::paintStroke(p, s, W, H, double(W) / double(w));
    p.end();
    return img;
}

QString emitVideoNotes(const QString& mediaPath, Exporter::AssetSink& sink) {
    std::vector<qcv::AnnotationNote> notes;
    if (!qcv::annotation_io::loadNotes(notes, mediaPath) || notes.empty())
        return {};
    const QString imagesDir = qcv::annotation_io::getImagesFolder(mediaPath);
    const QString base = sanitizedBase(mediaPath);
    QString out = QStringLiteral("**Notes (%1)**").arg(notes.size());
    for (const qcv::AnnotationNote& n : notes) {
        // Thumb: prefer the annotated sibling; if strokes exist but the
        // annotated PNG is stale/missing, recomposite from the clean frame so
        // inked notes never export ink-less.
        const QString cleanName = qcv::annotation_io::generateImageFilename(n.timecode);
        QString annName = cleanName;
        annName.replace(QStringLiteral(".png"), QStringLiteral("_annotated.png"));
        const QString cleanPath = imagesDir + QLatin1Char('/') + cleanName;
        const QString annPath   = imagesDir + QLatin1Char('/') + annName;
        const bool hasStrokes = qcv::AnnotationSerializer::hasStrokes(n.annotation_data);

        QString rel;
        const QString assetBase = base + QStringLiteral("_") + QFileInfo(cleanName).completeBaseName();
        if (QFileInfo::exists(annPath)) {
            rel = sink.addFile(annPath, assetBase);
        } else if (QFileInfo::exists(cleanPath)) {
            if (hasStrokes) {
                const QImage clean(cleanPath);
                const QImage composed = qcv::renderNoteThumbnail(
                    clean, qcv::AnnotationSerializer::jsonStringToStrokes(n.annotation_data), 1, 1);
                rel = composed.isNull() ? sink.addFile(cleanPath, assetBase)
                                        : sink.addImage(composed, assetBase);
            } else {
                rel = sink.addFile(cleanPath, assetBase);
            }
        }

        out += QStringLiteral("\n\n");
        if (!rel.isEmpty())
            out += QStringLiteral("![%1](%2)\n").arg(n.timecode, mdDest(rel));
        out += QStringLiteral("`%1`").arg(n.timecode);
        if (!n.text.isEmpty()) {
            QString t = escapeMd(n.text);
            t.replace(QStringLiteral("\n"), QStringLiteral("  \n"));
            out += QStringLiteral(" — ") + t;
        }
        if (n.addressed) out += QStringLiteral(" — ✓ addressed");
    }
    return out;
}

QString emitMedia(const BlockModel* m, int row, const Exporter::Options& opt,
                  Exporter::AssetSink& sink) {
    const QString kind = m->mediaKind(row);
    const QString path = m->mediaLocalPath(row);
    const QString name = QFileInfo(path).fileName();

    if (kind == QLatin1String("sketch")) {
        const QImage img = renderSketch(m, row);
        if (img.isNull()) return QStringLiteral("<!-- sketch could not be rendered -->");
        const QString rel = sink.addImage(img, QStringLiteral("sketch"));
        return rel.isEmpty() ? QStringLiteral("<!-- sketch could not be exported -->")
                             : QStringLiteral("![Sketch](%1)").arg(mdDest(rel));
    }
    if (kind == QLatin1String("image")) {
        // Collect EVERY image (pasted AND referenced) into the assets folder:
        // sandboxed markdown viewers (VS Code, GitHub) won't render absolute
        // paths outside their workspace, so in-place links broke previews
        // (user-verified 2026-07-11). Everything the .md needs to RENDER is
        // collected; video/pdf/file stay pointers by design. Fallback to the
        // absolute path only when the source file is unreachable.
        const QString rel = sink.addFile(path, QFileInfo(path).completeBaseName());
        return QStringLiteral("![%1](%2)").arg(escapeMd(name),
                                               mdDest(rel.isEmpty() ? path : rel));
    }

    // video / pdf / file → poster (when we can make one) + a kind-labeled
    // fenced reference block: filename, resolved absolute path, metadata.
    QString out;
    QString meta;
    if (kind == QLatin1String("video")) {
        const QImage poster = MediaStore::extractFrame(path, 0, 1280);
        if (!poster.isNull()) {
            const QString rel = sink.addImage(poster, sanitizedBase(path) + QStringLiteral("_poster"));
            if (!rel.isEmpty())
                out += QStringLiteral("![%1](%2)\n").arg(escapeMd(name), mdDest(rel));
        }
        meta = QStringLiteral("%1×%2 · %3 fps · %4 · %5 frames")
                   .arg(m->mediaW(row)).arg(m->mediaH(row))
                   .arg(m->mediaFps(row), 0, 'g', 5)
                   .arg(humanDuration(m->mediaDurationMs(row)))
                   .arg(m->mediaFrames(row));
    } else if (kind == QLatin1String("pdf")) {
        const QImage poster = MediaStore::renderPdfPage(path, 0, 1280);
        if (!poster.isNull()) {
            const QString rel = sink.addImage(poster, sanitizedBase(path) + QStringLiteral("_page1"));
            if (!rel.isEmpty())
                out += QStringLiteral("![%1](%2)\n").arg(escapeMd(name), mdDest(rel));
        }
        meta = QStringLiteral("%1 pages").arg(m->mediaPdfPages(row));
    } else {   // "file" and anything unknown → a plain reference
        const QFileInfo fi(path);
        meta = fi.exists() ? humanSize(fi.size()) : QStringLiteral("(unavailable)");
    }
    out += QStringLiteral("```%1\n%2\n%3\n%4\n```").arg(kind, name, path, meta);

    if (kind == QLatin1String("video") && opt.includeVideoNotes) {
        const QString notes = emitVideoNotes(path, sink);
        if (!notes.isEmpty()) out += QStringLiteral("\n\n") + notes;
    }
    return out;
}

// ---------- file sink ----------

class FileSink : public Exporter::AssetSink {
public:
    FileSink(const QString& parentDir, const QString& dirName)
        : parent_(parentDir), dirName_(dirName) {}

    QString addFile(const QString& srcPath, const QString& baseName) override {
        if (!QFileInfo::exists(srcPath) || !ensureDir()) return {};
        const QString name = unique(baseName, QFileInfo(srcPath).suffix());
        const QString dst = parent_ + QLatin1Char('/') + dirName_ + QLatin1Char('/') + name;
        QFile::remove(dst);
        if (!QFile::copy(srcPath, dst)) return {};
        return dirName_ + QLatin1Char('/') + name;
    }
    QString addImage(const QImage& img, const QString& baseName) override {
        if (img.isNull() || !ensureDir()) return {};
        const QString name = unique(baseName, QStringLiteral("png"));
        const QString dst = parent_ + QLatin1Char('/') + dirName_ + QLatin1Char('/') + name;
        if (!img.save(dst, "PNG")) return {};
        return dirName_ + QLatin1Char('/') + name;
    }

private:
    bool ensureDir() {
        if (made_) return true;
        made_ = QDir(parent_).mkpath(dirName_);
        return made_;
    }
    QString unique(const QString& base, const QString& suffix) {
        QString clean = base;
        clean.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
        const QString ext = suffix.isEmpty() ? QStringLiteral("png") : suffix;
        QString name = clean + QLatin1Char('.') + ext;
        int n = 2;
        while (used_.contains(name))
            name = clean + QStringLiteral("-%1.").arg(n++) + ext;
        used_.insert(name);
        return name;
    }
    QString parent_, dirName_;
    QSet<QString> used_;
    bool made_ = false;
};

} // namespace

// ---------- Exporter ----------

Exporter::Exporter(QObject* parent) : QObject(parent) {}

QString Exporter::toMarkdown(const Options& opt, AssetSink& sink) const {
    if (!model_) return {};
    const BlockModel* m = model_;
    FootnoteCtx fn;
    QString doc;
    bool prevWasList = false;

    const int count = m->rowCountQml();
    for (int row = 0; row < count; ++row) {
        const int type = m->typeForRow(row);
        const bool isList = BlockModel::isListType(static_cast<uint8_t>(type));
        QString block;

        switch (type) {
        case BlockModel::Heading:
            block = QString(std::clamp(m->levelForRow(row), 1, 6), QLatin1Char('#'))
                    + QLatin1Char(' ') + emitInline(m, row, fn);
            break;
        case BlockModel::Quote: {
            QString inl = emitInline(m, row, fn);
            block = QStringLiteral("> ") + inl.replace(QStringLiteral("\n"), QStringLiteral("\n> "));
            break;
        }
        case BlockModel::Code: {
            const QString body = m->contentForRow(row);
            const QString fence(std::max(3, maxBacktickRun(body) + 1), QLatin1Char('`'));
            block = fence + m->languageForRow(row) + QLatin1Char('\n')
                    + body + QLatin1Char('\n') + fence;
            break;
        }
        case BlockModel::ListItem:
        case BlockModel::TaskListItem:
        case BlockModel::OrderedListItem: {
            const QString indent(2 * std::clamp(m->depthForRow(row), 0, BlockModel::kMaxListDepth),
                                 QLatin1Char(' '));
            QString bullet;
            if (type == BlockModel::OrderedListItem) {
                bullet = QStringLiteral("%1. ").arg(m->orderedNumberForRow(row));
            } else if (type == BlockModel::TaskListItem) {
                switch (m->taskStateForRow(row)) {
                case BlockModel::TaskDoing: bullet = QStringLiteral("- [/] "); break;
                case BlockModel::TaskDone:  bullet = QStringLiteral("- [x] "); break;
                default:                    bullet = QStringLiteral("- [ ] "); break;
                }
            } else {
                bullet = QStringLiteral("- ");
            }
            QString inl = emitInline(m, row, fn);
            inl.replace(QStringLiteral("\n"), QStringLiteral("  \n") + indent + QStringLiteral("  "));
            block = indent + bullet + inl;
            break;
        }
        case BlockModel::Divider:
            block = QStringLiteral("---");
            break;
        case BlockModel::Table:
            block = emitTable(m, row, sink);
            break;
        case BlockModel::Media:
            block = emitMedia(m, row, opt, sink);
            break;
        case BlockModel::Paragraph:
        default: {
            QString inl = emitInline(m, row, fn);
            block = inl.replace(QStringLiteral("\n"), QStringLiteral("  \n"));
            break;
        }
        }

        // Margin ink isn't exported in markdown v1 — leave an honest,
        // renderer-invisible note where it was.
        if (!m->inkForRow(row).isEmpty())
            block += QStringLiteral("\n<!-- margin ink on this block was not exported -->");

        if (block.isEmpty() && type == BlockModel::Paragraph
            && m->contentForRow(row).isEmpty())
            block = QString();   // keep empty paragraphs as blank separation

        if (!doc.isEmpty())
            doc += (prevWasList && isList) ? QStringLiteral("\n") : QStringLiteral("\n\n");
        doc += block;
        prevWasList = isList;
    }

    // Footnote bodies (comment threads), in first-reference order.
    if (!fn.threadIds.isEmpty()) {
        doc += QStringLiteral("\n");
        const QVariantList threads = m->commentThreads();
        for (int i = 0; i < fn.threadIds.size(); ++i) {
            const QString id = fn.threadIds.at(i);
            bool resolved = false;
            for (const QVariant& tv : threads) {
                const QVariantMap t = tv.toMap();
                if (t.value(QStringLiteral("id")).toString() == id) {
                    resolved = t.value(QStringLiteral("resolved")).toBool();
                    break;
                }
            }
            QString body;
            for (const QVariant& mv : m->commentMessages(id)) {
                const QVariantMap msg = mv.toMap();
                QString b = escapeMd(msg.value(QStringLiteral("body")).toString());
                b.replace(QStringLiteral("\n"), QStringLiteral("\n    "));
                const qint64 created = msg.value(QStringLiteral("created")).toLongLong();
                const QString stamp = created > 0
                    ? QDateTime::fromMSecsSinceEpoch(created).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                    : QString();
                if (!body.isEmpty()) body += QStringLiteral("\n    ");
                body += b;
                if (!stamp.isEmpty()) body += QStringLiteral(" _(%1)_").arg(stamp);
            }
            if (body.isEmpty()) body = QStringLiteral("_(no messages)_");
            doc += QStringLiteral("\n[^%1]: %2%3").arg(i + 1)
                       .arg(resolved ? QStringLiteral("(resolved) ") : QString())
                       .arg(body);
        }
    }

    if (!doc.endsWith(QLatin1Char('\n'))) doc += QLatin1Char('\n');
    return doc;
}

QVariantMap Exporter::scan() const {
    QVariantMap out;
    int videos = 0, videosWithNotes = 0, videoNotes = 0, sketches = 0;
    if (model_) {
        const int count = model_->rowCountQml();
        for (int row = 0; row < count; ++row) {
            if (model_->typeForRow(row) != BlockModel::Media) continue;
            const QString kind = model_->mediaKind(row);
            if (kind == QLatin1String("sketch")) { ++sketches; continue; }
            if (kind != QLatin1String("video")) continue;
            ++videos;
            std::vector<qcv::AnnotationNote> notes;
            if (qcv::annotation_io::loadNotes(notes, model_->mediaLocalPath(row))
                && !notes.empty()) {
                ++videosWithNotes;
                videoNotes += static_cast<int>(notes.size());
            }
        }
    }
    out.insert(QStringLiteral("videos"), videos);
    out.insert(QStringLiteral("videosWithNotes"), videosWithNotes);
    out.insert(QStringLiteral("videoNotes"), videoNotes);
    out.insert(QStringLiteral("inkBlocks"),
               model_ ? model_->inkBlockIds().size() : 0);
    out.insert(QStringLiteral("sketches"), sketches);
    return out;
}

bool Exporter::exportMarkdown(const QString& fileUrlOrPath, bool includeVideoNotes) {
    if (!model_) return false;
    const QString path = localPathOf(fileUrlOrPath);
    if (path.isEmpty()) return false;
    const QFileInfo fi(path);

    Options opt;
    opt.includeVideoNotes = includeVideoNotes;
    FileSink sink(fi.absolutePath(), fi.completeBaseName() + QStringLiteral(".assets"));
    const QString md = toMarkdown(opt, sink);

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(md.toUtf8());
    return f.commit();
}

// ============================ HTML emitter =============================

namespace {

QString htmlEscape(const QString& s) {
    QString out;
    out.reserve(s.size() + 16);
    for (const QChar c : s) {
        switch (c.unicode()) {
        case '&':  out += QStringLiteral("&amp;");  break;
        case '<':  out += QStringLiteral("&lt;");   break;
        case '>':  out += QStringLiteral("&gt;");   break;
        case '"':  out += QStringLiteral("&quot;"); break;
        default:   out += c; break;
        }
    }
    return out;
}

// Highlight text needs contrast against the user's swatch — the same
// auto-contrast idea the in-app highlighter uses, approximated by luma.
QString contrastOn(const QString& hex) {
    const QColor c(hex);
    if (!c.isValid()) return QStringLiteral("#f0f0f0");
    const double luma = 0.299 * c.red() + 0.587 * c.green() + 0.114 * c.blue();
    return luma > 140.0 ? QStringLiteral("#111111") : QStringLiteral("#f0f0f0");
}

// Everything the HTML walk styles, comment ranges included (they render as a
// tinted span closed by a superscript reference into the comments section).
int htmlRankOf(uint8_t k) {
    switch (k) {
    case BlockModel::SpanComment:   return 0;   // outermost
    case BlockModel::SpanLink:      return 1;
    case BlockModel::SpanFgColor:   return 2;
    case BlockModel::SpanHighlight: return 3;
    case BlockModel::SpanBold:      return 4;
    case BlockModel::SpanItalic:    return 5;
    case BlockModel::SpanStrike:    return 6;
    case BlockModel::SpanUnderline: return 7;
    case BlockModel::SpanCode:      return 8;
    }
    return 9;
}

QString emitInlineHtml(const BlockModel* m, int row, FootnoteCtx& fn) {
    const QString text = m->contentForRow(row);
    const int len = text.size();

    struct Run { int s, e; uint8_t kind; QString u; int note = 0;
                 bool operator==(const Run& o) const {
                     return kind == o.kind && u == o.u && s == o.s && e == o.e; } };
    std::vector<Run> runs;
    for (const QVariant& v : m->spansForRow(row)) {
        const QVariantMap sp = v.toMap();
        const uint8_t k = static_cast<uint8_t>(sp.value(QStringLiteral("k")).toInt());
        const int s = std::clamp(sp.value(QStringLiteral("s")).toInt(), 0, len);
        const int e = std::clamp(sp.value(QStringLiteral("e")).toInt(), 0, len);
        if (s >= e) continue;
        Run r{s, e, k, sp.value(QStringLiteral("u")).toString(), 0};
        if (k == BlockModel::SpanComment)
            r.note = fn.numberFor(r.u);
        runs.push_back(r);
    }

    std::set<int> bounds{0, len};
    for (const Run& r : runs) { bounds.insert(r.s); bounds.insert(r.e); }

    auto openTag = [](const Run& r) -> QString {
        switch (r.kind) {
        case BlockModel::SpanComment:   return QStringLiteral("<span class=\"cmt\">");
        case BlockModel::SpanLink:
            return QStringLiteral("<a href=\"%1\">").arg(htmlEscape(r.u));
        case BlockModel::SpanFgColor:
            return QStringLiteral("<span style=\"color:%1\">").arg(htmlEscape(r.u));
        case BlockModel::SpanHighlight:
            return QStringLiteral("<span style=\"background:%1;color:%2;padding:1px 2px\">")
                .arg(htmlEscape(r.u), contrastOn(r.u));
        case BlockModel::SpanBold:      return QStringLiteral("<strong>");
        case BlockModel::SpanItalic:    return QStringLiteral("<em>");
        case BlockModel::SpanStrike:    return QStringLiteral("<s>");
        case BlockModel::SpanUnderline: return QStringLiteral("<u>");
        case BlockModel::SpanCode:      return QStringLiteral("<code>");
        }
        return {};
    };
    auto closeTag = [m](const Run& r) -> QString {
        switch (r.kind) {
        case BlockModel::SpanComment: {
            // The thread rides INSIDE the tinted range as a hover card (the
            // app's margin card, exported); the section at the end remains
            // for print and the superscript link.
            QString card = QStringLiteral("<span class=\"cmtcard\">");
            for (const QVariant& mv : m->commentMessages(r.u)) {
                const QVariantMap msg = mv.toMap();
                QString b = htmlEscape(msg.value(QStringLiteral("body")).toString());
                b.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
                const qint64 created = msg.value(QStringLiteral("created")).toLongLong();
                card += QStringLiteral("<span class=\"cmsg\">%1%2</span>")
                    .arg(b, created > 0
                            ? QStringLiteral("<span class=\"stamp\">%1</span>")
                                  .arg(QDateTime::fromMSecsSinceEpoch(created)
                                           .toString(QStringLiteral("yyyy-MM-dd hh:mm")))
                            : QString());
            }
            card += QStringLiteral("</span>");
            return card + QStringLiteral("</span><sup class=\"cref\"><a href=\"#c%1\">%1</a></sup>")
                .arg(r.note);
        }
        case BlockModel::SpanLink:      return QStringLiteral("</a>");
        case BlockModel::SpanFgColor:
        case BlockModel::SpanHighlight: return QStringLiteral("</span>");
        case BlockModel::SpanBold:      return QStringLiteral("</strong>");
        case BlockModel::SpanItalic:    return QStringLiteral("</em>");
        case BlockModel::SpanStrike:    return QStringLiteral("</s>");
        case BlockModel::SpanUnderline: return QStringLiteral("</u>");
        case BlockModel::SpanCode:      return QStringLiteral("</code>");
        }
        return {};
    };

    QString out;
    std::vector<Run> stack;
    auto it = bounds.begin();
    int prev = *it;
    for (++it; it != bounds.end(); ++it) {
        const int a = prev, b = *it;
        prev = *it;
        if (a >= b) continue;
        std::vector<Run> desired;
        for (const Run& r : runs)
            if (r.s <= a && r.e >= b) desired.push_back(r);
        std::sort(desired.begin(), desired.end(), [](const Run& x, const Run& y) {
            if (htmlRankOf(x.kind) != htmlRankOf(y.kind))
                return htmlRankOf(x.kind) < htmlRankOf(y.kind);
            if (x.s != y.s) return x.s < y.s;
            return x.u < y.u;
        });
        size_t common = 0;
        while (common < stack.size() && common < desired.size()
               && stack[common] == desired[common]) ++common;
        while (stack.size() > common) { out += closeTag(stack.back()); stack.pop_back(); }
        for (size_t i = common; i < desired.size(); ++i) {
            out += openTag(desired[i]);
            stack.push_back(desired[i]);
        }
        out += htmlEscape(text.mid(a, b - a));
    }
    while (!stack.empty()) { out += closeTag(stack.back()); stack.pop_back(); }
    out.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    return out;
}

// Mirrors the app's tri-state checkbox exactly (Editor.qml task item):
// todo = muted 1.5px border; doing = accent border + centered accent dash;
// done = borderless accent fill + white check. Pure CSS (.cb in kHtmlCss).
QString taskGlyphHtml(int state) {
    switch (state) {
    case BlockModel::TaskDoing: return QStringLiteral("<span class=\"cb doing\"></span>");
    case BlockModel::TaskDone:  return QStringLiteral("<span class=\"cb done\"></span>");
    default:                    return QStringLiteral("<span class=\"cb\"></span>");
    }
}

QString cellHtml(const BlockModel* m, int row, int r, int c, Exporter::AssetSink& sink) {
    const int kind = m->tableColumnKind(row, c);
    if (kind == 2) return taskGlyphHtml(m->tableCellCheck(row, r, c));
    if (kind == 1) {
        const QString label = m->tableCellChoiceLabel(row, r, c);
        if (label.isEmpty()) return {};
        const QString col = m->tableCellChoiceColor(row, r, c);
        const QColor cc(col);
        const QString bg = cc.isValid()
            ? QStringLiteral("rgba(%1,%2,%3,0.28)").arg(cc.red()).arg(cc.green()).arg(cc.blue())
            : QStringLiteral("#333333");
        return QStringLiteral("<span class=\"chip\" style=\"background:%1\">%2</span>")
            .arg(bg, htmlEscape(label));
    }
    QString out;
    if (!m->tableCellMedia(row, r, c).isEmpty()) {
        const QString p = localPathOf(m->tableCellMediaUrl(row, r, c));
        const QString src = sink.addFile(p, QFileInfo(p).completeBaseName());
        if (!src.isEmpty())
            out += QStringLiteral("<img src=\"%1\" alt=\"\"><br>").arg(src);
    }
    QString t = htmlEscape(m->tableCell(row, r, c));
    t.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    return out + t;
}

QString emitTableHtml(const BlockModel* m, int row, Exporter::AssetSink& sink) {
    const int rows = m->tableRows(row), cols = m->tableColumns(row);
    const int hdr = m->tableHeaderRows(row);
    if (rows <= 0 || cols <= 0) return {};
    auto cellStyle = [&](int r, int c) {
        QString st;
        const QString bg = m->tableCellBg(row, r, c);
        const QString fg = m->tableCellFg(row, r, c);
        if (!bg.isEmpty()) st += QStringLiteral("background:%1;").arg(htmlEscape(bg));
        if (!fg.isEmpty()) st += QStringLiteral("color:%1;").arg(htmlEscape(fg));
        switch (m->tableColAlign(row, c)) {
        case 1: st += QStringLiteral("text-align:center;"); break;
        case 2: st += QStringLiteral("text-align:right;"); break;
        default: break;
        }
        return st.isEmpty() ? QString() : QStringLiteral(" style=\"%1\"").arg(st);
    };
    // Column widths authored in the app (drag-resized; 0 = auto) carry into
    // the export as a colgroup: set columns hold their width, auto columns
    // keep flexing. When EVERY column is authored the table goes
    // table-layout:fixed so the widths are exact (content wraps inside,
    // matching the app), not just minimums.
    QString colTags;
    int authored = 0;
    for (int c = 0; c < cols; ++c) {
        const int w = m->tableColWidth(row, c);
        if (w > 0) {
            colTags += QStringLiteral("<col style=\"width:%1px\">").arg(w);
            ++authored;
        } else {
            colTags += QStringLiteral("<col>");
        }
    }
    QString out = (authored == cols && authored > 0)
        ? QStringLiteral("<table style=\"table-layout:fixed\">")
        : QStringLiteral("<table>");
    if (authored > 0)
        out += QStringLiteral("<colgroup>%1</colgroup>").arg(colTags);
    for (int r = 0; r < rows; ++r) {
        const bool isHdr = r < hdr;
        out += QStringLiteral("<tr>");
        for (int c = 0; c < cols; ++c)
            out += QStringLiteral("<%1%2>%3</%1>")
                       .arg(isHdr ? QStringLiteral("th") : QStringLiteral("td"),
                            cellStyle(r, c), cellHtml(m, row, r, c, sink));
        out += QStringLiteral("</tr>");
    }
    out += QStringLiteral("</table>");
    return out;
}

// Note thumbs export as LAYERS when possible: the clean frame at the base
// and the ink on a transparent PNG stacked above it (z), so the page-level
// Annotations toggle can flip between marked-up and clean — the in-app eye
// switch, carried into the deliverable. The ink layer reuses
// renderNoteThumbnail's exact geometry by passing a transparent base.
// Falls back to the flattened annotated/composited image when the clean
// frame is missing or a render fails. `inkLayers` counts emitted layers so
// the toggle only appears on pages that have something to toggle.
QString emitVideoNotesHtml(const QString& mediaPath, Exporter::AssetSink& sink,
                           int& inkLayers) {
    std::vector<qcv::AnnotationNote> notes;
    if (!qcv::annotation_io::loadNotes(notes, mediaPath) || notes.empty())
        return {};
    const QString imagesDir = qcv::annotation_io::getImagesFolder(mediaPath);
    const QString base = sanitizedBase(mediaPath);
    QString out = QStringLiteral("<section class=\"vnotes\"><h4>Notes (%1)</h4>")
                      .arg(notes.size());
    for (const qcv::AnnotationNote& n : notes) {
        const QString cleanName = qcv::annotation_io::generateImageFilename(n.timecode);
        QString annName = cleanName;
        annName.replace(QStringLiteral(".png"), QStringLiteral("_annotated.png"));
        const QString cleanPath = imagesDir + QLatin1Char('/') + cleanName;
        const QString annPath   = imagesDir + QLatin1Char('/') + annName;
        const QString assetBase = base + QStringLiteral("_") + QFileInfo(cleanName).completeBaseName();
        const bool hasStrokes = qcv::AnnotationSerializer::hasStrokes(n.annotation_data);

        QString baseSrc, inkSrc;
        if (QFileInfo::exists(cleanPath)) {
            if (hasStrokes) {
                const QImage clean(cleanPath);
                QImage blank(clean.size(), QImage::Format_ARGB32_Premultiplied);
                blank.fill(Qt::transparent);
                const QImage ink = qcv::renderNoteThumbnail(
                    blank, qcv::AnnotationSerializer::jsonStringToStrokes(n.annotation_data),
                    1, 1);
                if (!ink.isNull()) {
                    baseSrc = sink.addFile(cleanPath, assetBase + QStringLiteral("_clean"));
                    inkSrc  = sink.addImage(ink, assetBase + QStringLiteral("_ink"));
                }
                if (baseSrc.isEmpty() || inkSrc.isEmpty()) {   // fall back to flattened
                    baseSrc.clear(); inkSrc.clear();
                    const QImage composed = qcv::renderNoteThumbnail(
                        clean, qcv::AnnotationSerializer::jsonStringToStrokes(n.annotation_data),
                        1, 1);
                    baseSrc = composed.isNull() ? sink.addFile(cleanPath, assetBase)
                                                : sink.addImage(composed, assetBase);
                }
            } else {
                baseSrc = sink.addFile(cleanPath, assetBase);
            }
        } else if (QFileInfo::exists(annPath)) {
            baseSrc = sink.addFile(annPath, assetBase);   // flattened — no clean to layer over
        }

        out += QStringLiteral("<div class=\"notecard%1\">")
                   .arg(n.addressed ? QStringLiteral(" addressed") : QString());
        if (!inkSrc.isEmpty()) {
            ++inkLayers;
            out += QStringLiteral("<div class=\"thumb\"><img src=\"%1\" alt=\"%2\">"
                                  "<img class=\"ink\" src=\"%3\" alt=\"\"></div>")
                       .arg(baseSrc, htmlEscape(n.timecode), inkSrc);
        } else if (!baseSrc.isEmpty()) {
            out += QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(baseSrc, htmlEscape(n.timecode));
        }
        out += QStringLiteral("<div><span class=\"tc\">%1</span>%2")
                   .arg(htmlEscape(n.timecode),
                        n.addressed ? QStringLiteral(" <span class=\"ok\">✓ addressed</span>")
                                    : QString());
        if (!n.text.isEmpty()) {
            QString t = htmlEscape(n.text);
            t.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
            out += QStringLiteral("<p>%1</p>").arg(t);
        }
        out += QStringLiteral("</div></div>");
    }
    out += QStringLiteral("</section>");
    return out;
}

QString emitMediaHtml(const BlockModel* m, int row, const Exporter::Options& opt,
                      Exporter::AssetSink& sink, int& inkLayers) {
    const QString kind = m->mediaKind(row);
    const QString path = m->mediaLocalPath(row);
    const QString name = QFileInfo(path).fileName();

    if (kind == QLatin1String("sketch")) {
        const QImage img = renderSketch(m, row);
        if (img.isNull()) return {};
        const QString src = sink.addImage(img, QStringLiteral("sketch"));
        return src.isEmpty() ? QString()
            : QStringLiteral("<figure><img class=\"sketch\" src=\"%1\" alt=\"Sketch\"></figure>").arg(src);
    }
    if (kind == QLatin1String("image")) {
        QString src = sink.addFile(path, QFileInfo(path).completeBaseName());
        if (src.isEmpty()) src = QStringLiteral("file://") + path;   // unreachable source
        const QImage ink = renderMediaInk(m, row, QSize(m->mediaW(row), m->mediaH(row)));
        if (!ink.isNull()) {
            const QString inkSrc = sink.addImage(ink, QFileInfo(path).completeBaseName()
                                                          + QStringLiteral("_ink"));
            if (!inkSrc.isEmpty()) {
                ++inkLayers;
                return QStringLiteral("<figure><div class=\"inkwrap\">"
                                      "<img src=\"%1\" alt=\"%2\">"
                                      "<img class=\"ink\" src=\"%3\" alt=\"\"></div></figure>")
                    .arg(htmlEscape(src), htmlEscape(name), inkSrc);
            }
        }
        return QStringLiteral("<figure><img src=\"%1\" alt=\"%2\"></figure>")
            .arg(htmlEscape(src), htmlEscape(name));
    }

    QString poster, posterInk, meta;
    QSize posterSize;
    if (kind == QLatin1String("video")) {
        const QImage p = MediaStore::extractFrame(path, 0, 1280);
        if (!p.isNull()) {
            poster = sink.addImage(p, sanitizedBase(path) + QStringLiteral("_poster"));
            posterSize = p.size();
        }
        meta = QStringLiteral("%1×%2 · %3 fps · %4 · %5 frames")
                   .arg(m->mediaW(row)).arg(m->mediaH(row))
                   .arg(m->mediaFps(row), 0, 'g', 5)
                   .arg(humanDuration(m->mediaDurationMs(row)))
                   .arg(m->mediaFrames(row));
    } else if (kind == QLatin1String("pdf")) {
        const QImage p = MediaStore::renderPdfPage(path, 0, 1280);
        if (!p.isNull()) {
            poster = sink.addImage(p, sanitizedBase(path) + QStringLiteral("_page1"));
            posterSize = p.size();
        }
        meta = QStringLiteral("%1 pages").arg(m->mediaPdfPages(row));
    } else {
        const QFileInfo fi(path);
        meta = fi.exists() ? humanSize(fi.size()) : QStringLiteral("(unavailable)");
    }
    if (!poster.isEmpty()) {
        const QImage ink = renderMediaInk(m, row, posterSize);
        if (!ink.isNull())
            posterInk = sink.addImage(ink, sanitizedBase(path) + QStringLiteral("_ink"));
    }

    QString out = QStringLiteral("<figure class=\"ref\">");
    if (!poster.isEmpty() && !posterInk.isEmpty()) {
        ++inkLayers;
        out += QStringLiteral("<div class=\"inkwrap\"><img src=\"%1\" alt=\"%2\">"
                              "<img class=\"ink\" src=\"%3\" alt=\"\"></div>")
                   .arg(poster, htmlEscape(name), posterInk);
    } else if (!poster.isEmpty()) {
        out += QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(poster, htmlEscape(name));
    }
    out += QStringLiteral("<figcaption><div class=\"fname\">%1</div>"
                          "<div class=\"fpath\">%2</div>"
                          "<div class=\"fmeta\">%3 · %4</div></figcaption></figure>")
               .arg(htmlEscape(name), htmlEscape(path), htmlEscape(kind), htmlEscape(meta));

    if (kind == QLatin1String("video") && opt.includeVideoNotes)
        out += emitVideoNotesHtml(path, sink, inkLayers);
    return out;
}

// Self-contained sink: every asset becomes a data URI inline in the page.
class DataUriSink : public Exporter::AssetSink {
public:
    QString addFile(const QString& srcPath, const QString&) override {
        QFile f(srcPath);
        if (!f.open(QIODevice::ReadOnly)) return {};
        const QString ext = QFileInfo(srcPath).suffix().toLower();
        QString mime = QStringLiteral("image/png");
        if (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg"))
            mime = QStringLiteral("image/jpeg");
        else if (ext == QLatin1String("gif"))  mime = QStringLiteral("image/gif");
        else if (ext == QLatin1String("webp")) mime = QStringLiteral("image/webp");
        else if (ext == QLatin1String("svg"))  mime = QStringLiteral("image/svg+xml");
        return QStringLiteral("data:%1;base64,%2")
            .arg(mime, QString::fromLatin1(f.readAll().toBase64()));
    }
    QString addImage(const QImage& img, const QString&) override {
        if (img.isNull()) return {};
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        return QStringLiteral("data:image/png;base64,%1")
            .arg(QString::fromLatin1(png.toBase64()));
    }
};

// The exported page's whole design system — the minNotes look in a file:
// the dark page, the 760px measure, squared corners, rationed accent, the
// QCView-note violet. System font stacks (no bundled fonts in v1).
const char* kHtmlCss = R"CSS(
:root{--bg:#181817;--text:#e4e3e2;--bright:#f0f0f0;--muted:#8a8a8a;--subtle:#5e5e5e;
--border:#2a2a2a;--divider:#333333;--accent:#0189f1;--recess:#0e0e0e;
--chipbg:#1d2733;--chiptext:#4aa8ff;--codetext:#d4d4e8;--violet:#b48ef0;--sel:#2a568c;--quote:#3a5e86}
/* One-tone field (#181817 — the app's ruled worksheet), left-anchored. */
*{box-sizing:border-box}
body{background:var(--bg);color:var(--text);margin:0;
font:15px/1.65 -apple-system,BlinkMacSystemFont,"Segoe UI",Inter,Roboto,sans-serif}
/* Left-anchored page (user ruling): the prose measure hugs the left edge
   rather than centering, so wide tables growing rightward read as one
   left-aligned system instead of breaking a centered frame. */
main{max-width:808px;margin:0;padding:48px 24px 96px}  /* content = the app's true 760 measure */
/* The BLOCK RULER: every block's number in the right margin (the app's
   rail). Elements host the span; all numbers align on one ledger line. */
main p,main h1,main h2,main h3,main h4,main h5,main h6,main blockquote,
main li,main figure,main .tablewrap,main .blkw{position:relative}
.bnum{position:absolute;left:772px;top:4px;width:calc(100vw - 836px);
min-width:48px;border-top:1px solid var(--border);padding-top:3px;
text-align:right;font-family:ui-monospace,Menlo,Consolas,monospace;
font-size:11px;color:var(--subtle);user-select:none;pointer-events:none}
/* Media-ink z-stack: frame-normalized margin ink rides its media exactly;
   the Annotations toggle governs it like the note-thumb layers. */
img.ink{pointer-events:none}
.inkwrap{position:relative}
.inkwrap img{display:block;max-width:100%}
.inkwrap .ink{position:absolute;inset:0;width:100%;z-index:1;background:transparent}
::selection{background:var(--sel);color:var(--bright)}
h1,h2,h3,h4,h5,h6{color:var(--bright);line-height:1.25}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
hr{border:0;border-top:1px solid var(--divider);margin:24px 0}
blockquote{font-family:Georgia,'Times New Roman',serif;border-left:3px solid var(--quote);
margin:0;padding:2px 16px;color:var(--muted)}
pre{background:var(--recess);border:1px solid var(--border);padding:12px 14px;overflow-x:auto;color:var(--codetext)}
code{font-family:ui-monospace,'JetBrains Mono',Menlo,Consolas,monospace;font-size:.9em}
:not(pre)>code{background:var(--chipbg);color:var(--chiptext);padding:1px 5px}
img{max-width:100%}
figure{margin:16px 0}
ul,ol{padding-left:24px}li{margin:2px 0}
/* Tri-state checkboxes, the app's exact recipe (squared, accent = task-state). */
.cb{display:inline-block;width:14px;height:14px;box-sizing:border-box;position:relative;
border:1.5px solid var(--muted);vertical-align:-2px;margin-right:6px}
.cb.doing{border-color:var(--accent)}
.cb.doing::after{content:"";position:absolute;left:2px;top:4.5px;width:7px;height:2px;background:var(--accent)}
.cb.done{background:var(--accent);border:0}
.cb.done::after{content:"";position:absolute;left:4.5px;top:1.5px;width:3.5px;height:7.5px;
border:solid var(--bright);border-width:0 2px 2px 0;transform:rotate(45deg)}
/* Tables size to content and escape the prose measure when wide: same left
   edge as everything else, growing rightward UNCAPPED (user ruling — a
   table wider than the viewport widens the page, one honest page-level
   scrollbar instead of a nested one). Narrow tables still fill the column.
   32px vertical margins, matching the app's user-tuned table pocket. */
.tablewrap{margin:32px 0;width:fit-content;min-width:100%}
.tablewrap>.bnum{top:-28px}
table{border-collapse:collapse;width:max-content;min-width:100%;font-size:14px;
background:var(--bg)}
td,th{border:1px solid var(--border);padding:6px 10px;text-align:left;vertical-align:top;overflow-wrap:break-word}
th{background:#252525;color:var(--bright)}
.chip{padding:1px 8px;font-size:13px;color:var(--bright)}
.cmt{background:rgba(1,137,241,.13);position:relative}
/* Hover thread card — the app's margin card, exported. Spans styled as
   blocks (a real <p> inside a span would trip the parser). */
.cmt .cmtcard{display:none;position:absolute;left:0;top:1.6em;z-index:30;
width:300px;background:#202020;border:1px solid var(--border);
padding:10px 12px;font-size:13px;font-style:normal;font-weight:400;
line-height:1.5;color:var(--text)}
.cmt:hover .cmtcard{display:block}
.cmtcard .cmsg{display:block;margin-bottom:6px}
.cmtcard .stamp{color:var(--subtle);font-size:11px;margin-left:8px}
@media print{.cmt .cmtcard{display:none !important}}
.cref a{font-size:.72em;color:var(--accent)}
.ref{background:transparent;border:1px solid var(--border)}
.ref img{display:block;width:100%}
.ref figcaption{padding:10px 14px}
.fname{color:var(--bright)}
.fpath{font-family:ui-monospace,Menlo,monospace;font-size:12px;color:var(--muted);word-break:break-all}
.fmeta{font-family:ui-monospace,Menlo,monospace;font-size:12px;color:var(--subtle);margin-top:2px}
.vnotes h4{color:var(--violet);margin:14px 0 8px}
.notecard{display:flex;gap:12px;background:transparent;border:1px solid var(--border);
padding:10px;margin-bottom:10px;align-items:flex-start}
.notecard>img{width:220px;flex:none;background:var(--recess)}
/* Layered thumbs: clean frame at the base, ink on a transparent PNG above
   (z-stack) — the page-level Annotations toggle flips the ink layer. */
.notecard .thumb{position:relative;width:220px;flex:none}
.notecard .thumb img{display:block;width:100%;background:var(--recess)}
.notecard .thumb .ink{position:absolute;inset:0;z-index:1;background:transparent}
#mn-ink{display:none}
label[for=mn-ink]{position:fixed;top:14px;right:16px;z-index:9;cursor:pointer;
user-select:none;font-size:12px;padding:5px 12px;color:var(--muted);
background:var(--recess);border:1px solid var(--border)}
#mn-ink:checked~label[for=mn-ink]{color:var(--bright);background:var(--divider);
border-color:var(--divider)}
#mn-ink:not(:checked)~main .ink{visibility:hidden}
@media print{label[for=mn-ink]{display:none}}
.notecard .tc{font-family:ui-monospace,Menlo,monospace;font-size:13px;color:var(--bright)}
.notecard p{margin:6px 0 0;font-size:14px}
.notecard.addressed{opacity:.55}
.notecard .ok{color:var(--muted);font-size:12px;margin-left:8px}
.sketch{background:transparent}
.comments{margin-top:48px;border-top:1px solid var(--divider);padding-top:12px}
.comments h2{font-size:18px}
.comments li{margin-bottom:12px;font-size:14px}
.comments .stamp{color:var(--subtle);font-size:12px;margin-left:8px}
.comments li.resolved{opacity:.55}
)CSS";

} // namespace

QString Exporter::toHtml(const Options& opt, AssetSink& sink) const {
    if (!model_) return {};
    const BlockModel* m = model_;
    FootnoteCtx fn;
    QString body;
    int inkLayers = 0;                // stacked ink layers emitted (gates the toggle)
    std::vector<QString> listStack;   // open list tags, one per depth level

    auto closeListsTo = [&](int n) {
        while (static_cast<int>(listStack.size()) > n) {
            body += QStringLiteral("</%1>\n").arg(listStack.back());
            listStack.pop_back();
        }
    };

    // The BLOCK RULER, exported: every block carries its number in the right
    // margin (the app's rail language — a shared address for reviews).
    // Injected as the element's first child (elements are position:relative);
    // pre/hr can't host children so they get a .blkw wrapper. List items get
    // an inline offset compensating their nesting indent so all numbers
    // align on one ledger line.
    auto bnum = [](int row) {
        return QStringLiteral("<span class=\"bnum\">%1</span>").arg(row + 1);
    };
    auto bnumLi = [](int row, int depth) {
        return QStringLiteral("<span class=\"bnum\" style=\"left:%1px\">%2</span>")
            .arg(772 - 24 * (depth + 1)).arg(row + 1);
    };
    // Page ink rides INSIDE its block element (the positioned ancestor):
    // injected before the block's final closing tag. X anchors to the page
    // center (380 in the 760 frame), minus the element's own indent.
    auto injectInk = [&](QString blk, int row, double indent) -> QString {
        const TextInk ti = renderTextInk(m, row);
        if (ti.img.isNull()) return blk;
        const QString src = sink.addImage(ti.img, QStringLiteral("pageink"));
        if (src.isEmpty()) return blk;
        ++inkLayers;
        const QString tag = QStringLiteral(
            "<img class=\"ink\" style=\"position:absolute;left:%1px;top:%2px;"
            "width:%3px;height:%4px;z-index:2\" src=\"%5\" alt=\"\">")
            .arg(380.0 + ti.box.left() - indent)
            .arg(ti.box.top())
            .arg(ti.box.width())
            .arg(ti.box.height())
            .arg(src);
        const int at = blk.lastIndexOf(QStringLiteral("</"));
        if (at < 0) return blk;
        blk.insert(at, tag);
        return blk;
    };

    const int count = m->rowCountQml();
    for (int row = 0; row < count; ++row) {
        const int type = m->typeForRow(row);
        if (!BlockModel::isListType(static_cast<uint8_t>(type)))
            closeListsTo(0);

        switch (type) {
        case BlockModel::Heading: {
            const int lvl = std::clamp(m->levelForRow(row), 1, 6);
            body += injectInk(QStringLiteral("<h%1>%2%3</h%1>\n")
                        .arg(lvl).arg(bnum(row), emitInlineHtml(m, row, fn)), row, 0);
            break;
        }
        case BlockModel::Quote:
            body += injectInk(QStringLiteral("<blockquote>%1<p>%2</p></blockquote>\n")
                        .arg(bnum(row), emitInlineHtml(m, row, fn)), row, 0);
            break;
        case BlockModel::Code:
            body += injectInk(QStringLiteral("<div class=\"blkw\">%1<pre><code class=\"language-%2\">%3</code></pre></div>\n")
                        .arg(bnum(row), htmlEscape(m->languageForRow(row)),
                             htmlEscape(m->contentForRow(row))), row, 0);
            break;
        case BlockModel::ListItem:
        case BlockModel::TaskListItem:
        case BlockModel::OrderedListItem: {
            const int depth = std::clamp(m->depthForRow(row), 0, BlockModel::kMaxListDepth);
            const int want = depth + 1;
            const QString tag = (type == BlockModel::OrderedListItem)
                                    ? QStringLiteral("ol") : QStringLiteral("ul");
            closeListsTo(want);
            if (static_cast<int>(listStack.size()) == want && listStack.back() != tag)
                closeListsTo(want - 1);
            while (static_cast<int>(listStack.size()) < want) {
                QString open = tag;
                if (tag == QLatin1String("ol")) {
                    const int start = m->orderedNumberForRow(row);
                    if (start > 1) open += QStringLiteral(" start=\"%1\"").arg(start);
                }
                body += QStringLiteral("<%1>\n").arg(open);
                listStack.push_back(tag);
            }
            QString li = emitInlineHtml(m, row, fn);
            if (type == BlockModel::TaskListItem)
                li = taskGlyphHtml(m->taskStateForRow(row)) + li;
            body += injectInk(QStringLiteral("<li>%1%2</li>\n").arg(bnumLi(row, depth), li),
                              row, 24.0 * (depth + 1));
            break;
        }
        case BlockModel::Divider:
            body += injectInk(QStringLiteral("<div class=\"blkw\">%1<hr></div>\n").arg(bnum(row)), row, 0);
            break;
        case BlockModel::Table:
            // Wrapped so wide tables can BREAK OUT of the prose measure
            // while small ones still fill the column — see .tablewrap.
            body += injectInk(QStringLiteral("<div class=\"tablewrap\">%1%2</div>\n")
                        .arg(bnum(row), emitTableHtml(m, row, sink)), row, 0);
            break;
        case BlockModel::Media: {
            QString media = emitMediaHtml(m, row, opt, sink, inkLayers);
            // Number the figure (first tag) — the notes section below it
            // belongs to the same block.
            const int gt = media.indexOf(QLatin1Char('>'));
            if (gt >= 0) media.insert(gt + 1, bnum(row));
            body += media + QLatin1Char('\n');
            break;
        }
        case BlockModel::Paragraph:
        default: {
            const QString inl = emitInlineHtml(m, row, fn);
            body += injectInk(QStringLiteral("<p>%1%2</p>\n")
                        .arg(bnum(row), inl.isEmpty() ? QStringLiteral("&nbsp;") : inl), row, 0);
            break;
        }
        }
    }
    closeListsTo(0);

    // Comments section (linked from the superscript refs).
    if (!fn.threadIds.isEmpty()) {
        body += QStringLiteral("<section class=\"comments\"><h2>Comments</h2><ol>\n");
        const QVariantList threads = m->commentThreads();
        for (int i = 0; i < fn.threadIds.size(); ++i) {
            const QString id = fn.threadIds.at(i);
            bool resolved = false;
            for (const QVariant& tv : threads) {
                const QVariantMap t = tv.toMap();
                if (t.value(QStringLiteral("id")).toString() == id) {
                    resolved = t.value(QStringLiteral("resolved")).toBool();
                    break;
                }
            }
            body += QStringLiteral("<li id=\"c%1\"%2>")
                        .arg(i + 1)
                        .arg(resolved ? QStringLiteral(" class=\"resolved\"") : QString());
            if (resolved) body += QStringLiteral("<em>(resolved)</em> ");
            for (const QVariant& mv : m->commentMessages(id)) {
                const QVariantMap msg = mv.toMap();
                QString b = htmlEscape(msg.value(QStringLiteral("body")).toString());
                b.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
                const qint64 created = msg.value(QStringLiteral("created")).toLongLong();
                body += QStringLiteral("<p>%1%2</p>")
                            .arg(b, created > 0
                                     ? QStringLiteral("<span class=\"stamp\">%1</span>")
                                           .arg(QDateTime::fromMSecsSinceEpoch(created)
                                                    .toString(QStringLiteral("yyyy-MM-dd hh:mm")))
                                     : QString());
            }
            body += QStringLiteral("</li>\n");
        }
        body += QStringLiteral("</ol></section>\n");
    }

    // The Annotations toggle (the app's eye switch, CSS-only): present only
    // when ink layers were actually emitted. Checked = ink shown, matching
    // how the app opens; unchecking hides every .ink layer via the sibling
    // selector. Works without JavaScript and disappears in print.
    const QString toggle = inkLayers > 0
        ? QStringLiteral("<input type=\"checkbox\" id=\"mn-ink\" checked>"
                         "<label for=\"mn-ink\">Annotations</label>\n")
        : QString();

    return QStringLiteral(
               "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
               "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
               "<meta name=\"generator\" content=\"minNotes\">\n"
               "<title>%1</title>\n<style>%2</style>\n</head>\n<body>\n%3<main>\n%4</main>\n"
               "</body>\n</html>\n")
        .arg(htmlEscape(m->documentName()), QLatin1String(kHtmlCss), toggle, body);
}

bool Exporter::exportHtml(const QString& fileUrlOrPath, bool includeVideoNotes) {
    if (!model_) return false;
    const QString path = localPathOf(fileUrlOrPath);
    if (path.isEmpty()) return false;

    Options opt;
    opt.includeVideoNotes = includeVideoNotes;
    DataUriSink sink;
    const QString html = toHtml(opt, sink);

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(html.toUtf8());
    return f.commit();
}

// ============================ DOCX emitter =============================
//
// Hand-rolled OOXML via Qt's private QZipWriter — the QCView recipe,
// grown from a flat note list to the full block walker. Comments become
// NATIVE Word review comments (commentRangeStart/End + comments.xml).
// White-paper document: content-faithful, not dark-theme-faithful.

#include <QXmlStreamWriter>
#include <private/qzipwriter_p.h>

namespace {

constexpr double kEmuPerPx = 9525.0;           // 96 dpi
constexpr int    kDocxImgMaxPx = 624;          // 6.5in content width @96

struct DocxCtx {
    const BlockModel* m = nullptr;
    Exporter::Options opt;
    QList<QPair<QByteArray, QString>> images;  // bytes + extension, rIdImg<n>
    QStringList links;                          // hyperlink targets, rIdLnk<n>
    QStringList commentIds;                     // thread ids, w:id = index
    int picId = 1;
    int inkBaked = 0;
};

QByteArray docxXml(const std::function<void(QXmlStreamWriter&)>& fn) {
    QByteArray ba;
    QXmlStreamWriter w(&ba);
    w.setAutoFormatting(false);
    w.writeStartDocument(QStringLiteral("1.0"), true);
    fn(w);
    w.writeEndDocument();
    return ba;
}

int docxAddImage(DocxCtx& c, const QByteArray& bytes, const QString& ext) {
    c.images.append({bytes, ext});
    return c.images.size();                     // 1-based rel index
}
int docxAddImage(DocxCtx& c, const QImage& img) {
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return docxAddImage(c, png, QStringLiteral("png"));
}

// Transplanted from QCView: the full inline-drawing XML for one image.
void docxDrawing(QXmlStreamWriter& w, const QString& relId,
                 int widthEmu, int heightEmu, int picId) {
    w.writeStartElement(QStringLiteral("w:drawing"));
    w.writeStartElement(QStringLiteral("wp:inline"));
    for (const char* a : {"distT", "distB", "distL", "distR"})
        w.writeAttribute(QLatin1String(a), QStringLiteral("0"));
    w.writeStartElement(QStringLiteral("wp:extent"));
    w.writeAttribute(QStringLiteral("cx"), QString::number(widthEmu));
    w.writeAttribute(QStringLiteral("cy"), QString::number(heightEmu));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("wp:docPr"));
    w.writeAttribute(QStringLiteral("id"), QString::number(picId));
    w.writeAttribute(QStringLiteral("name"), QStringLiteral("Picture %1").arg(picId));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:graphic"));
    w.writeAttribute(QStringLiteral("xmlns:a"),
        QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/main"));
    w.writeStartElement(QStringLiteral("a:graphicData"));
    w.writeAttribute(QStringLiteral("uri"),
        QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/picture"));
    w.writeStartElement(QStringLiteral("pic:pic"));
    w.writeAttribute(QStringLiteral("xmlns:pic"),
        QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/picture"));
    w.writeStartElement(QStringLiteral("pic:nvPicPr"));
    w.writeStartElement(QStringLiteral("pic:cNvPr"));
    w.writeAttribute(QStringLiteral("id"), QString::number(picId));
    w.writeAttribute(QStringLiteral("name"), QStringLiteral("image%1").arg(picId));
    w.writeEndElement();
    w.writeEmptyElement(QStringLiteral("pic:cNvPicPr"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("pic:blipFill"));
    w.writeStartElement(QStringLiteral("a:blip"));
    w.writeAttribute(QStringLiteral("xmlns:r"),
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"));
    w.writeAttribute(QStringLiteral("r:embed"), relId);
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:stretch"));
    w.writeEmptyElement(QStringLiteral("a:fillRect"));
    w.writeEndElement();
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("pic:spPr"));
    w.writeStartElement(QStringLiteral("a:xfrm"));
    w.writeStartElement(QStringLiteral("a:off"));
    w.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:ext"));
    w.writeAttribute(QStringLiteral("cx"), QString::number(widthEmu));
    w.writeAttribute(QStringLiteral("cy"), QString::number(heightEmu));
    w.writeEndElement();
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:prstGeom"));
    w.writeAttribute(QStringLiteral("prst"), QStringLiteral("rect"));
    w.writeEmptyElement(QStringLiteral("a:avLst"));
    w.writeEndElement();
    w.writeEndElement();
    w.writeEndElement();   // pic:pic
    w.writeEndElement();   // a:graphicData
    w.writeEndElement();   // a:graphic
    w.writeEndElement();   // wp:inline
    w.writeEndElement();   // w:drawing
}

// One paragraph holding one image, display width capped to the page.
void docxImagePara(DocxCtx& c, QXmlStreamWriter& w, int relIdx,
                   int pxW, int pxH) {
    if (pxW <= 0 || pxH <= 0) return;
    double dispW = std::min(pxW, kDocxImgMaxPx);
    const double dispH = dispW * pxH / pxW;
    w.writeStartElement(QStringLiteral("w:p"));
    w.writeStartElement(QStringLiteral("w:r"));
    docxDrawing(w, QStringLiteral("rIdImg%1").arg(relIdx),
                int(dispW * kEmuPerPx), int(dispH * kEmuPerPx), c.picId++);
    w.writeEndElement();
    w.writeEndElement();
}

struct DocxRunProps {
    bool b = false, i = false, strike = false, u = false, code = false;
    QString color, highlight;                   // "#RRGGBB" or ""
    int halfPtSize = 0;                          // 0 = default
};

void docxRunProps(QXmlStreamWriter& w, const DocxRunProps& rp) {
    if (!rp.b && !rp.i && !rp.strike && !rp.u && !rp.code
        && rp.color.isEmpty() && rp.highlight.isEmpty() && rp.halfPtSize == 0)
        return;
    w.writeStartElement(QStringLiteral("w:rPr"));
    if (rp.code) {
        w.writeStartElement(QStringLiteral("w:rFonts"));
        w.writeAttribute(QStringLiteral("w:ascii"), QStringLiteral("Courier New"));
        w.writeAttribute(QStringLiteral("w:hAnsi"), QStringLiteral("Courier New"));
        w.writeEndElement();
    }
    if (rp.b) w.writeEmptyElement(QStringLiteral("w:b"));
    if (rp.i) w.writeEmptyElement(QStringLiteral("w:i"));
    if (rp.strike) w.writeEmptyElement(QStringLiteral("w:strike"));
    if (rp.u) {
        w.writeStartElement(QStringLiteral("w:u"));
        w.writeAttribute(QStringLiteral("w:val"), QStringLiteral("single"));
        w.writeEndElement();
    }
    if (!rp.color.isEmpty()) {
        w.writeStartElement(QStringLiteral("w:color"));
        w.writeAttribute(QStringLiteral("w:val"), QString(rp.color).remove(QLatin1Char('#')).toUpper());
        w.writeEndElement();
    }
    if (!rp.highlight.isEmpty() || rp.code) {
        w.writeStartElement(QStringLiteral("w:shd"));
        w.writeAttribute(QStringLiteral("w:val"), QStringLiteral("clear"));
        w.writeAttribute(QStringLiteral("w:fill"),
            rp.highlight.isEmpty() ? QStringLiteral("EFEFEF")
                                   : QString(rp.highlight).remove(QLatin1Char('#')).toUpper());
        w.writeEndElement();
    }
    if (rp.halfPtSize > 0) {
        w.writeStartElement(QStringLiteral("w:sz"));
        w.writeAttribute(QStringLiteral("w:val"), QString::number(rp.halfPtSize));
        w.writeEndElement();
    }
    w.writeEndElement();
}

void docxTextRun(QXmlStreamWriter& w, const QString& text, const DocxRunProps& rp) {
    if (text.isEmpty()) return;
    w.writeStartElement(QStringLiteral("w:r"));
    docxRunProps(w, rp);
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        if (i > 0) w.writeEmptyElement(QStringLiteral("w:br"));
        w.writeStartElement(QStringLiteral("w:t"));
        w.writeAttribute(QStringLiteral("xml:space"), QStringLiteral("preserve"));
        w.writeCharacters(lines.at(i));
        w.writeEndElement();
    }
    w.writeEndElement();
}

// One block's text + spans → OOXML runs. Every run carries its FULL
// properties, so overlapping spans need no nesting stack — only comment
// range markers and hyperlink wrappers have element structure.
void docxRuns(DocxCtx& c, QXmlStreamWriter& w, int row,
              const DocxRunProps& base) {
    const BlockModel* m = c.m;
    const QString text = m->contentForRow(row);
    const int len = text.size();

    struct Run { int s, e; uint8_t kind; QString u; };
    std::vector<Run> runs;
    std::map<int, QStringList> cmtStart, cmtEnd;   // pos → thread ids
    for (const QVariant& v : m->spansForRow(row)) {
        const QVariantMap sp = v.toMap();
        const uint8_t k = static_cast<uint8_t>(sp.value(QStringLiteral("k")).toInt());
        const int s = std::clamp(sp.value(QStringLiteral("s")).toInt(), 0, len);
        const int e = std::clamp(sp.value(QStringLiteral("e")).toInt(), 0, len);
        const QString u = sp.value(QStringLiteral("u")).toString();
        if (k == BlockModel::SpanComment) {
            if (e >= s) { cmtStart[s].append(u); cmtEnd[e].append(u); }
            continue;
        }
        if (s >= e) continue;
        runs.push_back({s, e, k, u});
    }

    std::set<int> bounds{0, len};
    for (const Run& r : runs) { bounds.insert(r.s); bounds.insert(r.e); }
    for (const auto& kv : cmtStart) bounds.insert(kv.first);
    for (const auto& kv : cmtEnd) bounds.insert(kv.first);

    auto cmtNum = [&](const QString& id) {
        int i = c.commentIds.indexOf(id);
        if (i < 0) { c.commentIds.append(id); i = c.commentIds.size() - 1; }
        return i;
    };
    auto markers = [&](int pos) {
        if (const auto it = cmtEnd.find(pos); it != cmtEnd.end())
            for (const QString& id : it->second) {
                const int n = cmtNum(id);
                w.writeStartElement(QStringLiteral("w:commentRangeEnd"));
                w.writeAttribute(QStringLiteral("w:id"), QString::number(n));
                w.writeEndElement();
                w.writeStartElement(QStringLiteral("w:r"));
                w.writeStartElement(QStringLiteral("w:commentReference"));
                w.writeAttribute(QStringLiteral("w:id"), QString::number(n));
                w.writeEndElement();
                w.writeEndElement();
            }
        if (const auto it = cmtStart.find(pos); it != cmtStart.end())
            for (const QString& id : it->second) {
                w.writeStartElement(QStringLiteral("w:commentRangeStart"));
                w.writeAttribute(QStringLiteral("w:id"), QString::number(cmtNum(id)));
                w.writeEndElement();
            }
    };

    QString openLink;   // active hyperlink target ("" = none)
    auto setLink = [&](const QString& target) {
        if (target == openLink) return;
        if (!openLink.isEmpty()) w.writeEndElement();   // </w:hyperlink>
        openLink = target;
        if (!openLink.isEmpty()) {
            int i = c.links.indexOf(openLink);
            if (i < 0) { c.links.append(openLink); i = c.links.size() - 1; }
            w.writeStartElement(QStringLiteral("w:hyperlink"));
            w.writeAttribute(QStringLiteral("r:id"), QStringLiteral("rIdLnk%1").arg(i + 1));
        }
    };

    auto it = bounds.begin();
    int prev = *it;
    for (++it; it != bounds.end(); ++it) {
        const int a = prev, b = *it;
        prev = *it;
        if (a >= b) continue;
        markers(a);
        DocxRunProps rp = base;
        QString link;
        for (const Run& r : runs) {
            if (!(r.s <= a && r.e >= b)) continue;
            switch (r.kind) {
            case BlockModel::SpanBold:      rp.b = true; break;
            case BlockModel::SpanItalic:    rp.i = true; break;
            case BlockModel::SpanCode:      rp.code = true; break;
            case BlockModel::SpanStrike:    rp.strike = true; break;
            case BlockModel::SpanUnderline: rp.u = true; break;
            case BlockModel::SpanLink:      link = r.u; rp.color = QStringLiteral("#0563C1"); rp.u = true; break;
            case BlockModel::SpanFgColor:   rp.color = r.u; break;
            case BlockModel::SpanHighlight: rp.highlight = r.u; break;
            default: break;
            }
        }
        setLink(link);
        docxTextRun(w, text.mid(a, b - a), rp);
    }
    setLink(QString());
    markers(len);
}

void docxPara(DocxCtx& c, QXmlStreamWriter& w, int row, const DocxRunProps& base,
              const std::function<void(QXmlStreamWriter&)>& pPr = nullptr) {
    w.writeStartElement(QStringLiteral("w:p"));
    if (pPr) { w.writeStartElement(QStringLiteral("w:pPr")); pPr(w); w.writeEndElement(); }
    docxRuns(c, w, row, base);
    w.writeEndElement();
}

void docxPlainPara(QXmlStreamWriter& w, const QString& text, const DocxRunProps& rp,
                   const std::function<void(QXmlStreamWriter&)>& pPr = nullptr) {
    w.writeStartElement(QStringLiteral("w:p"));
    if (pPr) { w.writeStartElement(QStringLiteral("w:pPr")); pPr(w); w.writeEndElement(); }
    docxTextRun(w, text, rp);
    w.writeEndElement();
}

} // namespace

namespace {

// ---- Whole-document writers ----

void docxHeading(DocxCtx& c, QXmlStreamWriter& w, int row, int level) {
    static const int half[7] = {0, 36, 32, 28, 26, 24, 22};
    DocxRunProps rp;
    rp.b = true;
    rp.halfPtSize = half[std::clamp(level, 1, 6)];
    docxPara(c, w, row, rp);
}

void docxTable(DocxCtx& c, QXmlStreamWriter& w, int row) {
    const BlockModel* m = c.m;
    const int rows = m->tableRows(row), cols = m->tableColumns(row);
    if (rows <= 0 || cols <= 0) return;
    w.writeStartElement(QStringLiteral("w:tbl"));
    w.writeStartElement(QStringLiteral("w:tblPr"));
    w.writeStartElement(QStringLiteral("w:tblBorders"));
    for (const char* side : {"top", "left", "bottom", "right", "insideH", "insideV"}) {
        w.writeStartElement(QStringLiteral("w:") + QLatin1String(side));
        w.writeAttribute(QStringLiteral("w:val"), QStringLiteral("single"));
        w.writeAttribute(QStringLiteral("w:sz"), QStringLiteral("4"));
        w.writeAttribute(QStringLiteral("w:color"), QStringLiteral("999999"));
        w.writeEndElement();
    }
    w.writeEndElement();
    w.writeEndElement();
    // Authored column widths carry through (px → dxa ≈ ×15); auto = 2000.
    w.writeStartElement(QStringLiteral("w:tblGrid"));
    for (int cix = 0; cix < cols; ++cix) {
        const int px = m->tableColWidth(row, cix);
        w.writeStartElement(QStringLiteral("w:gridCol"));
        w.writeAttribute(QStringLiteral("w:w"), QString::number(px > 0 ? px * 15 : 2000));
        w.writeEndElement();
    }
    w.writeEndElement();
    const int hdr = m->tableHeaderRows(row);
    for (int r = 0; r < rows; ++r) {
        w.writeStartElement(QStringLiteral("w:tr"));
        for (int cix = 0; cix < cols; ++cix) {
            w.writeStartElement(QStringLiteral("w:tc"));
            w.writeStartElement(QStringLiteral("w:tcPr"));
            const QString bg = m->tableCellBg(row, r, cix);
            if (!bg.isEmpty() || r < hdr) {
                w.writeStartElement(QStringLiteral("w:shd"));
                w.writeAttribute(QStringLiteral("w:val"), QStringLiteral("clear"));
                w.writeAttribute(QStringLiteral("w:fill"),
                    bg.isEmpty() ? QStringLiteral("EDEDED")
                                 : QString(bg).remove(QLatin1Char('#')).toUpper());
                w.writeEndElement();
            }
            w.writeEndElement();
            const int kind = m->tableColumnKind(row, cix);
            QString cell;
            if (kind == 2) {
                switch (m->tableCellCheck(row, r, cix)) {
                case 1:  cell = QStringLiteral("◐"); break;
                case 2:  cell = QStringLiteral("☑"); break;
                default: cell = QStringLiteral("☐"); break;
                }
            } else if (kind == 1) {
                cell = m->tableCellChoiceLabel(row, r, cix);
            } else {
                cell = m->tableCell(row, r, cix);
            }
            DocxRunProps rp;
            rp.b = (r < hdr);
            const QString fg = m->tableCellFg(row, r, cix);
            if (!fg.isEmpty()) rp.color = fg;
            docxPlainPara(w, cell, rp);
            w.writeEndElement();   // w:tc
        }
        w.writeEndElement();       // w:tr
    }
    w.writeEndElement();           // w:tbl
    docxPlainPara(w, QString(), {});   // spacer after the table
}

void docxMedia(DocxCtx& c, QXmlStreamWriter& w, int row) {
    const BlockModel* m = c.m;
    const QString kind = m->mediaKind(row);
    const QString path = m->mediaLocalPath(row);
    const QString name = QFileInfo(path).fileName();
    DocxRunProps mono;
    mono.code = true;
    mono.halfPtSize = 18;   // 9pt paths

    auto bakeInk = [&](QImage img) {
        const QImage ink = renderMediaInk(m, row, img.size());
        if (!ink.isNull()) {
            QPainter p(&img);
            p.drawImage(0, 0, ink);
            p.end();
            ++c.inkBaked;
        }
        return img;
    };

    if (kind == QLatin1String("sketch")) {
        const QImage img = renderSketch(m, row);
        if (!img.isNull())
            docxImagePara(c, w, docxAddImage(c, img), img.width(), img.height());
        return;
    }
    if (kind == QLatin1String("image")) {
        QImage img(path);
        if (!img.isNull()) {
            const bool hasInk = mn::docInkHasStrokes(m->inkForRow(row));
            if (hasInk) {
                img = bakeInk(img);
                docxImagePara(c, w, docxAddImage(c, img), img.width(), img.height());
            } else {
                // Untouched images ship their original bytes (no PNG bloat).
                QFile f(path);
                const QString ext = QFileInfo(path).suffix().toLower();
                if (f.open(QIODevice::ReadOnly)
                    && (ext == QLatin1String("png") || ext == QLatin1String("jpg")
                        || ext == QLatin1String("jpeg"))) {
                    docxImagePara(c, w,
                        docxAddImage(c, f.readAll(),
                                     ext == QLatin1String("png") ? QStringLiteral("png")
                                                                 : QStringLiteral("jpeg")),
                        img.width(), img.height());
                } else {
                    docxImagePara(c, w, docxAddImage(c, img), img.width(), img.height());
                }
            }
        }
        return;
    }

    // video / pdf / file → poster (ink baked) + reference paragraphs.
    QImage poster;
    QString meta;
    if (kind == QLatin1String("video")) {
        poster = MediaStore::extractFrame(path, 0, 1280);
        meta = QStringLiteral("%1×%2 · %3 fps · %4 · %5 frames")
                   .arg(m->mediaW(row)).arg(m->mediaH(row))
                   .arg(m->mediaFps(row), 0, 'g', 5)
                   .arg(humanDuration(m->mediaDurationMs(row)))
                   .arg(m->mediaFrames(row));
    } else if (kind == QLatin1String("pdf")) {
        poster = MediaStore::renderPdfPage(path, 0, 1280);
        meta = QStringLiteral("%1 pages").arg(m->mediaPdfPages(row));
    } else {
        const QFileInfo fi(path);
        meta = fi.exists() ? humanSize(fi.size()) : QStringLiteral("(unavailable)");
    }
    if (!poster.isNull()) {
        poster = bakeInk(poster);
        docxImagePara(c, w, docxAddImage(c, poster), poster.width(), poster.height());
    }
    DocxRunProps bold; bold.b = true;
    docxPlainPara(w, name, bold);
    docxPlainPara(w, path, mono);
    docxPlainPara(w, kind + QStringLiteral(" · ") + meta, mono);

    if (kind == QLatin1String("video") && c.opt.includeVideoNotes) {
        std::vector<qcv::AnnotationNote> notes;
        if (qcv::annotation_io::loadNotes(notes, path) && !notes.empty()) {
            DocxRunProps h; h.b = true;
            docxPlainPara(w, QStringLiteral("Notes (%1)").arg(notes.size()), h);
            const QString imagesDir = qcv::annotation_io::getImagesFolder(path);
            for (const qcv::AnnotationNote& n : notes) {
                const QString cleanName = qcv::annotation_io::generateImageFilename(n.timecode);
                QString annName = cleanName;
                annName.replace(QStringLiteral(".png"), QStringLiteral("_annotated.png"));
                QImage thumb;
                if (QFileInfo::exists(imagesDir + QLatin1Char('/') + annName))
                    thumb.load(imagesDir + QLatin1Char('/') + annName);
                else if (QFileInfo::exists(imagesDir + QLatin1Char('/') + cleanName)) {
                    thumb.load(imagesDir + QLatin1Char('/') + cleanName);
                    if (qcv::AnnotationSerializer::hasStrokes(n.annotation_data)) {
                        const QImage composed = qcv::renderNoteThumbnail(
                            thumb, qcv::AnnotationSerializer::jsonStringToStrokes(n.annotation_data), 1, 1);
                        if (!composed.isNull()) thumb = composed;
                    }
                }
                if (!thumb.isNull())
                    docxImagePara(c, w, docxAddImage(c, thumb), thumb.width(), thumb.height());
                QString line = n.timecode;
                if (!n.text.isEmpty()) line += QStringLiteral(" — ") + n.text;
                if (n.addressed) line += QStringLiteral(" — ✓ addressed");
                docxPlainPara(w, line, mono);
            }
        }
    }
}

QByteArray docxDocumentXml(DocxCtx& c) {
    return docxXml([&](QXmlStreamWriter& w) {
        const BlockModel* m = c.m;
        w.writeStartElement(QStringLiteral("w:document"));
        w.writeAttribute(QStringLiteral("xmlns:w"),
            QStringLiteral("http://schemas.openxmlformats.org/wordprocessingml/2006/main"));
        w.writeAttribute(QStringLiteral("xmlns:r"),
            QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"));
        w.writeAttribute(QStringLiteral("xmlns:wp"),
            QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"));
        w.writeStartElement(QStringLiteral("w:body"));

        const int count = m->rowCountQml();
        for (int row = 0; row < count; ++row) {
            const int type = m->typeForRow(row);
            switch (type) {
            case BlockModel::Heading:
                docxHeading(c, w, row, m->levelForRow(row));
                break;
            case BlockModel::Quote: {
                DocxRunProps rp; rp.i = true;
                docxPara(c, w, row, rp, [](QXmlStreamWriter& pw) {
                    pw.writeStartElement(QStringLiteral("w:ind"));
                    pw.writeAttribute(QStringLiteral("w:left"), QStringLiteral("720"));
                    pw.writeEndElement();
                });
                break;
            }
            case BlockModel::Code: {
                DocxRunProps rp; rp.code = true; rp.halfPtSize = 18;
                docxPlainPara(w, m->contentForRow(row), rp);
                break;
            }
            case BlockModel::ListItem:
            case BlockModel::TaskListItem:
            case BlockModel::OrderedListItem: {
                const int depth = std::clamp(m->depthForRow(row), 0, BlockModel::kMaxListDepth);
                const int numId = (type == BlockModel::OrderedListItem) ? 2 : 1;
                w.writeStartElement(QStringLiteral("w:p"));
                w.writeStartElement(QStringLiteral("w:pPr"));
                w.writeStartElement(QStringLiteral("w:numPr"));
                w.writeStartElement(QStringLiteral("w:ilvl"));
                w.writeAttribute(QStringLiteral("w:val"), QString::number(depth));
                w.writeEndElement();
                w.writeStartElement(QStringLiteral("w:numId"));
                w.writeAttribute(QStringLiteral("w:val"), QString::number(numId));
                w.writeEndElement();
                w.writeEndElement();
                w.writeEndElement();
                if (type == BlockModel::TaskListItem) {
                    QString g = QStringLiteral("☐ ");
                    if (m->taskStateForRow(row) == BlockModel::TaskDoing) g = QStringLiteral("◐ ");
                    else if (m->taskStateForRow(row) == BlockModel::TaskDone) g = QStringLiteral("☑ ");
                    docxTextRun(w, g, {});
                }
                docxRuns(c, w, row, {});
                w.writeEndElement();
                break;
            }
            case BlockModel::Divider:
                docxPlainPara(w, QString(), {}, [](QXmlStreamWriter& pw) {
                    pw.writeStartElement(QStringLiteral("w:pBdr"));
                    pw.writeStartElement(QStringLiteral("w:bottom"));
                    pw.writeAttribute(QStringLiteral("w:val"), QStringLiteral("single"));
                    pw.writeAttribute(QStringLiteral("w:sz"), QStringLiteral("6"));
                    pw.writeAttribute(QStringLiteral("w:color"), QStringLiteral("999999"));
                    pw.writeEndElement();
                    pw.writeEndElement();
                });
                break;
            case BlockModel::Table:
                docxTable(c, w, row);
                break;
            case BlockModel::Media:
                docxMedia(c, w, row);
                break;
            case BlockModel::Paragraph:
            default:
                docxPara(c, w, row, {});
                break;
            }
        }
        w.writeEndElement();   // w:body
        w.writeEndElement();   // w:document
    });
}

QByteArray docxCommentsXml(const DocxCtx& c) {
    return docxXml([&](QXmlStreamWriter& w) {
        w.writeStartElement(QStringLiteral("w:comments"));
        w.writeAttribute(QStringLiteral("xmlns:w"),
            QStringLiteral("http://schemas.openxmlformats.org/wordprocessingml/2006/main"));
        const QString author =
            qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", "minNotes"));
        const QVariantList threads = c.m->commentThreads();
        for (int i = 0; i < c.commentIds.size(); ++i) {
            const QString id = c.commentIds.at(i);
            bool resolved = false;
            for (const QVariant& tv : threads) {
                const QVariantMap t = tv.toMap();
                if (t.value(QStringLiteral("id")).toString() == id) {
                    resolved = t.value(QStringLiteral("resolved")).toBool();
                    break;
                }
            }
            w.writeStartElement(QStringLiteral("w:comment"));
            w.writeAttribute(QStringLiteral("w:id"), QString::number(i));
            w.writeAttribute(QStringLiteral("w:author"), author);
            const QVariantList msgs = c.m->commentMessages(id);
            if (!msgs.isEmpty()) {
                const qint64 created = msgs.first().toMap()
                                           .value(QStringLiteral("created")).toLongLong();
                if (created > 0)
                    w.writeAttribute(QStringLiteral("w:date"),
                        QDateTime::fromMSecsSinceEpoch(created).toString(Qt::ISODate));
            }
            if (resolved) docxPlainPara(w, QStringLiteral("(resolved)"), {});
            for (const QVariant& mv : msgs)
                docxPlainPara(w, mv.toMap().value(QStringLiteral("body")).toString(), {});
            w.writeEndElement();
        }
        w.writeEndElement();
    });
}

QByteArray docxNumberingXml() {
    return docxXml([](QXmlStreamWriter& w) {
        w.writeStartElement(QStringLiteral("w:numbering"));
        w.writeAttribute(QStringLiteral("xmlns:w"),
            QStringLiteral("http://schemas.openxmlformats.org/wordprocessingml/2006/main"));
        for (int abs = 0; abs < 2; ++abs) {
            w.writeStartElement(QStringLiteral("w:abstractNum"));
            w.writeAttribute(QStringLiteral("w:abstractNumId"), QString::number(abs));
            for (int lvl = 0; lvl < 9; ++lvl) {
                w.writeStartElement(QStringLiteral("w:lvl"));
                w.writeAttribute(QStringLiteral("w:ilvl"), QString::number(lvl));
                w.writeStartElement(QStringLiteral("w:numFmt"));
                w.writeAttribute(QStringLiteral("w:val"),
                    abs == 0 ? QStringLiteral("bullet") : QStringLiteral("decimal"));
                w.writeEndElement();
                w.writeStartElement(QStringLiteral("w:lvlText"));
                w.writeAttribute(QStringLiteral("w:val"),
                    abs == 0 ? QStringLiteral("•") : QStringLiteral("%%%1.").arg(lvl + 1));
                w.writeEndElement();
                w.writeStartElement(QStringLiteral("w:pPr"));
                w.writeStartElement(QStringLiteral("w:ind"));
                w.writeAttribute(QStringLiteral("w:left"), QString::number(720 * (lvl + 1)));
                w.writeAttribute(QStringLiteral("w:hanging"), QStringLiteral("360"));
                w.writeEndElement();
                w.writeEndElement();
                w.writeEndElement();
            }
            w.writeEndElement();
        }
        for (int n = 1; n <= 2; ++n) {
            w.writeStartElement(QStringLiteral("w:num"));
            w.writeAttribute(QStringLiteral("w:numId"), QString::number(n));
            w.writeStartElement(QStringLiteral("w:abstractNumId"));
            w.writeAttribute(QStringLiteral("w:val"), QString::number(n - 1));
            w.writeEndElement();
            w.writeEndElement();
        }
        w.writeEndElement();
    });
}

} // namespace

bool Exporter::exportDocx(const QString& fileUrlOrPath, bool includeVideoNotes) {
    if (!model_) return false;
    const QString outPath = localPathOf(fileUrlOrPath);
    if (outPath.isEmpty()) return false;

    DocxCtx c;
    c.m = model_;
    c.opt.includeVideoNotes = includeVideoNotes;
    const QByteArray documentXml = docxDocumentXml(c);

    const QByteArray contentTypes = docxXml([&](QXmlStreamWriter& w) {
        w.writeStartElement(QStringLiteral("Types"));
        w.writeAttribute(QStringLiteral("xmlns"),
            QStringLiteral("http://schemas.openxmlformats.org/package/2006/content-types"));
        auto def = [&](const char* ext, const char* type) {
            w.writeStartElement(QStringLiteral("Default"));
            w.writeAttribute(QStringLiteral("Extension"), QLatin1String(ext));
            w.writeAttribute(QStringLiteral("ContentType"), QLatin1String(type));
            w.writeEndElement();
        };
        def("rels", "application/vnd.openxmlformats-package.relationships+xml");
        def("xml", "application/xml");
        def("png", "image/png");
        def("jpeg", "image/jpeg");
        auto ovr = [&](const char* part, const char* type) {
            w.writeStartElement(QStringLiteral("Override"));
            w.writeAttribute(QStringLiteral("PartName"), QLatin1String(part));
            w.writeAttribute(QStringLiteral("ContentType"), QLatin1String(type));
            w.writeEndElement();
        };
        ovr("/word/document.xml",
            "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml");
        ovr("/word/numbering.xml",
            "application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml");
        if (!c.commentIds.isEmpty())
            ovr("/word/comments.xml",
                "application/vnd.openxmlformats-officedocument.wordprocessingml.comments+xml");
        w.writeEndElement();
    });

    const QByteArray pkgRels = docxXml([](QXmlStreamWriter& w) {
        w.writeStartElement(QStringLiteral("Relationships"));
        w.writeAttribute(QStringLiteral("xmlns"),
            QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships"));
        w.writeStartElement(QStringLiteral("Relationship"));
        w.writeAttribute(QStringLiteral("Id"), QStringLiteral("rId1"));
        w.writeAttribute(QStringLiteral("Type"),
            QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"));
        w.writeAttribute(QStringLiteral("Target"), QStringLiteral("word/document.xml"));
        w.writeEndElement();
        w.writeEndElement();
    });

    const QByteArray docRels = docxXml([&](QXmlStreamWriter& w) {
        w.writeStartElement(QStringLiteral("Relationships"));
        w.writeAttribute(QStringLiteral("xmlns"),
            QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships"));
        auto rel = [&](const QString& id, const char* type, const QString& target,
                       bool external = false) {
            w.writeStartElement(QStringLiteral("Relationship"));
            w.writeAttribute(QStringLiteral("Id"), id);
            w.writeAttribute(QStringLiteral("Type"),
                QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/")
                    + QLatin1String(type));
            w.writeAttribute(QStringLiteral("Target"), target);
            if (external)
                w.writeAttribute(QStringLiteral("TargetMode"), QStringLiteral("External"));
            w.writeEndElement();
        };
        rel(QStringLiteral("rIdNum"), "numbering", QStringLiteral("numbering.xml"));
        if (!c.commentIds.isEmpty())
            rel(QStringLiteral("rIdCmt"), "comments", QStringLiteral("comments.xml"));
        for (int i = 0; i < c.images.size(); ++i)
            rel(QStringLiteral("rIdImg%1").arg(i + 1), "image",
                QStringLiteral("media/image%1.%2").arg(i + 1).arg(c.images.at(i).second));
        for (int i = 0; i < c.links.size(); ++i)
            rel(QStringLiteral("rIdLnk%1").arg(i + 1), "hyperlink", c.links.at(i), true);
        w.writeEndElement();
    });

    QZipWriter zip(outPath);
    zip.setCompressionPolicy(QZipWriter::AlwaysCompress);
    if (zip.status() != QZipWriter::NoError) return false;
    zip.addFile(QStringLiteral("[Content_Types].xml"), contentTypes);
    zip.addFile(QStringLiteral("_rels/.rels"), pkgRels);
    zip.addFile(QStringLiteral("word/document.xml"), documentXml);
    zip.addFile(QStringLiteral("word/_rels/document.xml.rels"), docRels);
    zip.addFile(QStringLiteral("word/numbering.xml"), docxNumberingXml());
    if (!c.commentIds.isEmpty())
        zip.addFile(QStringLiteral("word/comments.xml"), docxCommentsXml(c));
    for (int i = 0; i < c.images.size(); ++i)
        zip.addFile(QStringLiteral("word/media/image%1.%2")
                        .arg(i + 1).arg(c.images.at(i).second),
                    c.images.at(i).first);
    zip.close();
    return zip.status() == QZipWriter::NoError;
}
