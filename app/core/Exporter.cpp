// Exporter — see header. Markdown emitter v1.

#include "Exporter.h"
#include "BlockModel.h"
#include "MediaStore.h"

#include "../notes/annotation_io.h"
#include "../notes/annotation_note.h"
#include "../notes/annotation_serializer.h"
#include "../notes/annotation_thumbnail.h"

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
