// Exporter — see header. Markdown emitter v1.

#include "Exporter.h"
#include "BlockModel.h"
#include "CodeSyntax.h"
#include "MediaStore.h"

#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>

#include "../notes/annotation_io.h"
#include "../notes/annotation_note.h"
#include "../notes/annotation_serializer.h"
#include "../notes/annotation_thumbnail.h"
#include "../notes/sketch_text.h"
#include "../notes/doc_ink.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QLocale>
#include <QScreen>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextTable>
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

// Effective column count for a table export: TRAILING fully-empty plain
// columns (no text, no media, in any row — header included) drop at export
// time. Sheet imports formatted to the sheet edge shouldn't ship 20 blank
// pipes per row; the document itself is untouched. Interior empty columns
// and typed (check/choice) columns are deliberate structure and stay.
int exportCols(const BlockModel* m, int row) {
    int cols = m->tableColumns(row);
    const int rows = m->tableRows(row);
    while (cols > 1) {
        const int c = cols - 1;
        if (m->tableColumnKind(row, c) != 0) break;
        bool empty = true;
        for (int r = 0; r < rows && empty; ++r)
            if (!m->tableCell(row, r, c).isEmpty()
                || !m->tableCellMedia(row, r, c).isEmpty())
                empty = false;
        if (!empty) break;
        --cols;
    }
    return cols;
}

// The app's column-width rule, mirrored for export (user ruling 2026-08-20:
// exports give copy the room the app does). Manual widths pass through; an
// auto column is CONTENT-MEASURED — the widest cell line plus padding —
// clamped [48, 360] exactly like BlockTable.recomputeAutoW. Falls back to
// the app's 160 default only for a column with nothing measurable.
int exportColWidth(const BlockModel* m, int row, int c) {
    const int manual = m->tableColWidth(row, c);
    if (manual > 0) return manual;
    static const QFontMetricsF fm = [] {
        QFont f(QStringLiteral("Aspekta"));
        f.setPixelSize(14);                    // the app's body size
        return QFontMetricsF(f);
    }();
    const int rows = m->tableRows(row);
    const int hdr = m->tableHeaderRows(row);
    const int kind = m->tableColumnKind(row, c);
    qreal mw = 0;
    // Header rows stay text in every column kind; body cells only carry
    // measurable text in a text column. Row 0 reserves the sort-glyph slot.
    const int textRows = (kind == 0) ? rows : hdr;
    for (int r = 0; r < textRows; ++r) {
        const qreal pad = (r == 0 && hdr > 0) ? 18 : 0;
        const QStringList lines = m->tableCell(row, r, c).split(QLatin1Char('\n'));
        for (const QString& ln : lines)
            mw = std::max(mw, fm.horizontalAdvance(ln) + pad);
    }
    if (kind == 1) {   // choice: fit the widest option chip
        for (const QVariant& ov : m->tableColumnOptions(row, c)) {
            const QString label = ov.toMap().value(QStringLiteral("label")).toString();
            mw = std::max(mw, fm.horizontalAdvance(label) + 10);
        }
    }
    if (mw <= 0) return 160;                   // nothing measurable: app default
    return int(std::lround(std::clamp(mw + 2 * 8 + 6, 48.0, 360.0)));
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

// Markdown destination for a LOCAL file: absolute paths become
// percent-encoded file:// URLs — "<{/Volumes/… with spaces}>" is legal
// CommonMark but the least portable destination form (user-caught
// 2026-08-20 on a NAS-referenced paste); encoded URLs render AND click in
// strictly more consumers, and need no angle brackets. Relative asset
// paths pass through untouched.
QString mdFileDest(const QString& pathOrRel) {
    if (QDir::isAbsolutePath(pathOrRel))
        return QString::fromUtf8(QUrl::fromLocalFile(pathOrRel).toEncoded());
    return mdDest(pathOrRel);
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
            if (!p.isEmpty()) {
                // The block-image rule (emitMedia): collect into .assets;
                // an unreachable source keeps its mapped absolute link
                // instead of silently dropping the image.
                const QString rel = sink.addFile(p, QFileInfo(p).completeBaseName());
                out += QStringLiteral("![](%1) ")
                           .arg(mdFileDest(rel.isEmpty() ? p : rel));
            }
        }
        out += escapeMd(m->tableCell(row, r, c));
    }
    out.replace(QStringLiteral("|"), QStringLiteral("\\|"));
    out.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    return out;
}

QString emitTable(const BlockModel* m, int row, Exporter::AssetSink& sink) {
    const int rows = m->tableRows(row), cols = exportCols(m, row);
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
// media). Ink may OVERSHOOT the frame (values outside [0,1] = margin
// overshoot, per doc_ink.h), so the raster covers frame ∪ ink bbox and
// `boxNorm` says where it sits in FRAME units — the HTML layer positions it
// with percent offsets (scales with the responsive image); a unit rect means
// exactly-the-frame (the legacy inset:0 case). (Text-block ink is
// px-anchored to the app's layout and rides inside its block instead.)
struct MediaInk { QImage img; QRectF boxNorm; };
MediaInk renderMediaInk(const BlockModel* m, int row, const QSize& target) {
    MediaInk out;
    mn::DocInkAnchor a;
    if (!mn::docInkFromJson(m->inkForRow(row), a)
        || (a.strokes.empty() && a.texts.empty())
        || a.space != mn::DocInkAnchor::Frame || target.isEmpty())
        return out;
    const double mw = std::max(1, m->mediaW(row));
    const double mh = std::max(1, m->mediaH(row));
    // strokeWidth is stored in media-INTRINSIC px (the doc_ink convention).
    const double ws = double(target.width()) / mw;
    for (mn::SketchTextSpec& t : a.texts) t.family = mn::sketchTextFamily();

    // Signed bbox in frame units: the frame itself ∪ every stroke (padded by
    // half its width) ∪ every chip's derived rect.
    QRectF box(0, 0, 1, 1);
    for (const qcv::ActiveStroke& s : a.strokes) {
        const double px = double(s.strokeWidth) / (2.0 * mw);
        const double py = double(s.strokeWidth) / (2.0 * mh);
        box = box.united(qcv::strokeBoundsNorm(s).adjusted(-px, -py, px, py));
    }
    for (const mn::SketchTextSpec& t : a.texts) {
        const QRectF r = mn::sketchTextRectSrc(t, mw, mh);   // intrinsic px
        box = box.united(QRectF(r.x() / mw, r.y() / mh,
                                r.width() / mw, r.height() / mh));
    }

    const int W = int(std::ceil(box.width() * target.width()));
    const int H = int(std::ceil(box.height() * target.height()));
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192) return out;
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(-box.left() * target.width(), -box.top() * target.height());
    // Text chips before the ink (z-order: strokes can circle labels).
    for (const mn::SketchTextSpec& t : a.texts)
        mn::paintSketchText(p, t, mw, mh, ws);
    for (const qcv::ActiveStroke& s : a.strokes)
        qcv::paintStroke(p, s, target.width(), target.height(), ws);
    p.end();
    out.img = img;
    out.boxNorm = box;
    return out;
}

// Inline geometry for an overshooting media-ink layer: percent-of-frame
// offsets override the stylesheet's inset:0;width:100% (right/bottom back to
// auto, and out of the global img max-width clamp). Unit rect → no style.
QString mediaInkStyle(const QRectF& b) {
    if (b == QRectF(0, 0, 1, 1)) return {};
    return QStringLiteral(" style=\"left:%1%;top:%2%;width:%3%;height:%4%;"
                          "right:auto;bottom:auto;max-width:none\"")
        .arg(b.left() * 100).arg(b.top() * 100)
        .arg(b.width() * 100).arg(b.height() * 100);
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
    if (!mn::docInkFromJson(m->inkForRow(row), a)
        || (a.strokes.empty() && a.texts.empty())
        || a.space != mn::DocInkAnchor::Px)
        return out;
    for (mn::SketchTextSpec& t : a.texts) t.family = mn::sketchTextFamily();
    QRectF box;
    for (const qcv::ActiveStroke& st : a.strokes) {
        QRectF b = qcv::strokeBoundsNorm(st);
        const double pad = std::max<double>(2.0, st.strokeWidth);
        b.adjust(-pad, -pad, pad, pad);
        box = box.isNull() ? b : box.united(b);
    }
    for (const mn::SketchTextSpec& t : a.texts) {
        // px space: local units ARE page px (x from page center — negative ok).
        const QRectF b = mn::sketchTextRectSrc(t, 1.0, 1.0).adjusted(-2, -2, 2, 2);
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
    for (const mn::SketchTextSpec& t : a.texts)   // chips under the ink
        mn::paintSketchText(p, t, 1.0, 1.0, 1.0);  // painter carries the 2×
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
    // 2× until the output long edge would hit 8192, then scale down — a
    // max-size canvas exports at 1× (the unconditional 2× would allocate a
    // 1 GiB image at the 8192 frame cap), and a pasted 4K screenshot keeps
    // native resolution.
    const double s = std::min(2.0, 8192.0 / double(std::max(w, h)));
    const int W = int(std::lround(w * s)), H = int(std::lround(h * s));
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
    // Text elements between images and ink (z-order: ink can circle labels).
    for (mn::SketchTextSpec t : mn::parseSketchTexts(obj)) {
        t.family = mn::sketchTextFamily();
        mn::paintSketchText(p, t, w, h, double(W) / double(w));
    }
    // strokeWidth is stored in SOURCE px (the w-wide canvas) — scale to 2x.
    for (const qcv::ActiveStroke& s : qcv::AnnotationSerializer::jsonStringToStrokes(json))
        qcv::paintStroke(p, s, W, H, double(W) / double(w));
    p.end();
    return img;
}

// --- Per-page PDF ink (strokes + text chips), 2026-08-20. The page envelope
// is the sketch schema (coordinate_system "normalized"; strokeWidth/size in
// SOURCE px = the descriptor's page-point width). The overlay variant is
// transparent (the HTML .ink stack / Annotations toggle); the annotated
// variant bakes onto the rendered page (flowed formats).
QImage renderPdfPageInkOverlay(const BlockModel* m, int row, int page, QSize pageSize) {
    const QString env = m->pdfPageInk(row, page);
    if (env.isEmpty() || pageSize.isEmpty()) return {};
    const int srcW = std::max(1, m->mediaW(row));
    const int srcH = std::max(1, m->mediaH(row));
    QImage img(pageSize, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QJsonObject obj = QJsonDocument::fromJson(env.toUtf8()).object();
    const double ws = double(pageSize.width()) / double(srcW);
    // Chips under strokes — the sketch z-order (ink can circle labels).
    for (mn::SketchTextSpec t : mn::parseSketchTexts(obj)) {
        t.family = mn::sketchTextFamily();
        mn::paintSketchText(p, t, srcW, srcH, ws);
    }
    for (const qcv::ActiveStroke& s : qcv::AnnotationSerializer::jsonStringToStrokes(env))
        qcv::paintStroke(p, s, pageSize.width(), pageSize.height(), ws);
    p.end();
    return img;
}
QImage renderPdfPageAnnotated(const BlockModel* m, int row, int page,
                              const QString& path, int maxW) {
    QImage pg = MediaStore::renderPdfPage(path, page, maxW);
    if (pg.isNull()) return pg;
    const QImage ov = renderPdfPageInkOverlay(m, row, page, pg.size());
    if (!ov.isNull()) { QPainter p(&pg); p.drawImage(0, 0, ov); p.end(); }
    return pg;
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
                                               mdFileDest(rel.isEmpty() ? path : rel));
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
        // Page ink/chips are CONTENT (the sketch precedent), so the poster
        // carries page 1's baked in; margin ink stays dropped as ever.
        const QImage poster = renderPdfPageAnnotated(m, row, 0, path, 1280);
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

    if (kind == QLatin1String("pdf")) {
        // Poster + annotated pages (ruling 2026-08-20): every OTHER page
        // carrying ink/chips joins the export as its own baked image.
        const int pages = m->mediaPdfPages(row);
        for (int pg = 1; pg < pages; ++pg) {
            if (m->pdfPageInk(row, pg).isEmpty()) continue;
            const QImage img = renderPdfPageAnnotated(m, row, pg, path, 1280);
            if (img.isNull()) continue;
            const QString rel = sink.addImage(img,
                sanitizedBase(path) + QStringLiteral("_page%1").arg(pg + 1));
            if (!rel.isEmpty())
                out += QStringLiteral("\n\n![%1 — page %2](%3)")
                           .arg(escapeMd(name)).arg(pg + 1).arg(mdDest(rel));
        }
    }

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
        // One source, one copy: the same image in ten table cells (or a cell
        // AND a block) must not land ten -2/-3 duplicates in .assets.
        const QString key = QFileInfo(srcPath).canonicalFilePath();
        const auto it = bySrc_.constFind(key);
        if (it != bySrc_.constEnd()) return it.value();
        const QString name = unique(baseName, QFileInfo(srcPath).suffix());
        const QString dst = parent_ + QLatin1Char('/') + dirName_ + QLatin1Char('/') + name;
        QFile::remove(dst);
        if (!QFile::copy(srcPath, dst)) return {};
        const QString rel = dirName_ + QLatin1Char('/') + name;
        bySrc_.insert(key, rel);
        return rel;
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
    QHash<QString, QString> bySrc_;   // canonical source path → emitted rel
    QString parent_, dirName_;
    QSet<QString> used_;
    bool made_ = false;
};

} // namespace

// ---------- Exporter ----------

Exporter::Exporter(QObject* parent) : QObject(parent) {}

QString Exporter::toMarkdown(const Options& opt, AssetSink& sink) const {
    if (!model_) return {};
    return markdownRange(0, model_->rowCountQml() - 1, opt, sink, /*withHeader=*/true);
}

// The markdown walker over rows [lo, hi] — toMarkdown is the whole document
// with the document-name header; Copy as Markdown reuses it for ranges
// (no header — a pasted fragment shouldn't restate the doc's name).
QString Exporter::markdownRange(int lo, int hi, const Options& opt,
                                AssetSink& sink, bool withHeader) const {
    const BlockModel* m = model_;
    FootnoteCtx fn;
    // Document header (ruling 2026-08-20): every export leads with the
    // document's name — the file you hand someone should say what it is.
    QString doc = withHeader
        ? QStringLiteral("# %1\n\n").arg(escapeMd(m->documentName()))
        : QString();
    bool prevWasList = false;

    for (int row = lo; row <= hi; ++row) {
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

QString Exporter::copyMarkdown(int loRow, int hiRow) const {
    if (!model_) return {};
    const int n = model_->rowCountQml();
    if (n <= 0) return {};
    // No asset folder exists for a clipboard payload: files/images reference
    // their absolute source paths (the emitMedia fallback); generated
    // rasters (sketches, posters) degrade to their honest fallbacks.
    class NullSink : public AssetSink {
    public:
        QString addFile(const QString&, const QString&) override { return {}; }
        QString addImage(const QImage&, const QString&) override { return {}; }
    };
    NullSink sink;
    const bool whole = loRow < 0;
    const int lo = whole ? 0 : std::clamp(loRow, 0, n - 1);
    const int hi = whole ? n - 1 : std::clamp(hiRow < 0 ? loRow : hiRow, lo, n - 1);
    return markdownRange(lo, hi, Options{}, sink, /*withHeader=*/whole);
}

QVariantMap Exporter::scan() const {
    QVariantMap out;
    int videos = 0, videosWithNotes = 0, videoNotes = 0, sketches = 0;
    int pdfInkPages = 0;
    if (model_) {
        const int count = model_->rowCountQml();
        for (int row = 0; row < count; ++row) {
            if (model_->typeForRow(row) != BlockModel::Media) continue;
            const QString kind = model_->mediaKind(row);
            if (kind == QLatin1String("sketch")) { ++sketches; continue; }
            if (kind == QLatin1String("pdf")) {
                const int pages = model_->mediaPdfPages(row);
                for (int pg = 0; pg < pages; ++pg)
                    if (!model_->pdfPageInk(row, pg).isEmpty()) ++pdfInkPages;
                continue;
            }
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
    out.insert(QStringLiteral("pdfInkPages"), pdfInkPages);   // annotated PDF pages
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
    const int rows = m->tableRows(row), cols = exportCols(m, row);
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
    //
    // Viewport cap (user rulings 2026-08-20): a table wider than the page
    // column keeps its FULL authored width — the in-app extend-past-the-page
    // behavior; HTML has a whole viewport to spend, and squeezing 26 columns
    // into the page measure leaves no room for copy. It only shrinks when
    // the BROWSER is narrower than the table: width:min(authored, viewport
    // minus the page's left offset), table-layout:fixed + percentage shares,
    // so text wraps inside (td overflow-wrap) and cell images clamp (the
    // global img max-width) instead of the page scrolling sideways.
    QString colTags;
    int authored = 0;
    double total = 0;
    for (int c = 0; c < cols; ++c) {
        const int w = m->tableColWidth(row, c);
        total += exportColWidth(m, row, c);   // app-measured, not a flat 160
        if (w > 0) {
            colTags += QStringLiteral("<col style=\"width:%1px\">").arg(w);
            ++authored;
        } else {
            colTags += QStringLiteral("<col>");
        }
    }
    QString out;
    const double pw = std::max<double>(400.0, m->pageWidth());
    if (total > pw) {
        colTags.clear();
        for (int c = 0; c < cols; ++c)
            colTags += QStringLiteral("<col style=\"width:%1%\">")
                           .arg(exportColWidth(m, row, c) / total * 100.0, 0, 'f', 2);
        // 152 = main's 120px left padding + 32px right breathing room.
        // Floor at the page measure (user ruling 2026-08-20): the viewport
        // cap never squeezes a table below the page column — geometry stays
        // put on narrow windows (scroll, don't reflow) so ink aligns.
        out = QStringLiteral(
            "<table style=\"table-layout:fixed;"
            "width:min(%1px,max(%2px,calc(100vw - 152px)))\">")
                  .arg(int(total)).arg(int(pw));
        out += QStringLiteral("<colgroup>%1</colgroup>").arg(colTags);
    } else {
        out = (authored == cols && authored > 0)
            ? QStringLiteral("<table style=\"table-layout:fixed\">")
            : QStringLiteral("<table>");
        if (authored > 0)
            out += QStringLiteral("<colgroup>%1</colgroup>").arg(colTags);
    }
    // Header-AGNOSTIC (user ruling 2026-08-20): the flag can't be trusted on
    // sheet imports (row 0 is often data), so styled exports emit every row
    // as a plain data row — authored cell colours still carry.
    for (int r = 0; r < rows; ++r) {
        out += QStringLiteral("<tr>");
        for (int c = 0; c < cols; ++c)
            out += QStringLiteral("<td%1>%2</td>")
                       .arg(cellStyle(r, c), cellHtml(m, row, r, c, sink));
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
        if (src.isEmpty()) return {};
        // Display at the canvas's SOURCE size (the raster may be 2×), or the
        // user's dw — the image branch's rules.
        const int dw = QJsonDocument::fromJson(m->contentForRow(row).toUtf8())
                           .object().value(QStringLiteral("dw")).toInt(0);
        const QString wstyle = dw > 0
            ? QStringLiteral(" style=\"width:%1px;max-width:none\"").arg(dw)
            : QStringLiteral(" style=\"width:%1px\"").arg(m->mediaW(row));
        return QStringLiteral("<figure><img class=\"sketch\" src=\"%1\" alt=\"Sketch\"%2></figure>")
            .arg(src, wstyle);
    }
    if (kind == QLatin1String("image")) {
        QString src = sink.addFile(path, QFileInfo(path).completeBaseName());
        if (src.isEmpty()) src = QStringLiteral("file://") + path;   // unreachable source
        // Honour a user-set display width (the app's resize handle), including
        // past the page measure — the tables' page-widening pattern. Untouched
        // images carry no dw and stay responsive (max-width:100%).
        const int dw = QJsonDocument::fromJson(m->contentForRow(row).toUtf8())
                           .object().value(QStringLiteral("dw")).toInt(0);
        const QString wstyle = dw > 0
            ? QStringLiteral(" style=\"width:%1px;max-width:none\"").arg(dw)
            : QString();
        const MediaInk ink = renderMediaInk(m, row, QSize(m->mediaW(row), m->mediaH(row)));
        if (!ink.img.isNull()) {
            const QString inkSrc = sink.addImage(ink.img, QFileInfo(path).completeBaseName()
                                                              + QStringLiteral("_ink"));
            if (!inkSrc.isEmpty()) {
                ++inkLayers;
                // Width rides the WRAPPER (the ink layer is frame-positioned,
                // so both images scale together); the base fills it when
                // sized. Overshooting ink carries percent offsets.
                return QStringLiteral("<figure><div class=\"inkwrap\"%1>"
                                      "<img src=\"%2\" alt=\"%3\"%4>"
                                      "<img class=\"ink\" src=\"%5\" alt=\"\"%6></div></figure>")
                    .arg(wstyle, htmlEscape(src), htmlEscape(name),
                         dw > 0 ? QStringLiteral(" style=\"width:100%\"") : QString(),
                         inkSrc, mediaInkStyle(ink.boxNorm));
            }
        }
        return QStringLiteral("<figure><img src=\"%1\" alt=\"%2\"%3></figure>")
            .arg(htmlEscape(src), htmlEscape(name), wstyle);
    }

    QString poster, posterInk, posterInkStyle, pageInk0, meta;
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
            // Page 1's PAGE ink (strokes + chips) rides the same .ink stack
            // as margin ink — toggle-aware, lightbox-aware (2026-08-20).
            const QImage ov = renderPdfPageInkOverlay(m, row, 0, p.size());
            if (!ov.isNull())
                pageInk0 = sink.addImage(ov, sanitizedBase(path) + QStringLiteral("_page1_ink"));
        }
        meta = QStringLiteral("%1 pages").arg(m->mediaPdfPages(row));
    } else {
        const QFileInfo fi(path);
        meta = fi.exists() ? humanSize(fi.size()) : QStringLiteral("(unavailable)");
    }
    if (!poster.isEmpty()) {
        const MediaInk ink = renderMediaInk(m, row, posterSize);
        if (!ink.img.isNull()) {
            posterInk = sink.addImage(ink.img, sanitizedBase(path) + QStringLiteral("_ink"));
            posterInkStyle = mediaInkStyle(ink.boxNorm);
        }
    }

    QString out = QStringLiteral("<figure class=\"ref\">");
    if (!poster.isEmpty() && (!posterInk.isEmpty() || !pageInk0.isEmpty())) {
        out += QStringLiteral("<div class=\"inkwrap\"><img src=\"%1\" alt=\"%2\">")
                   .arg(poster, htmlEscape(name));
        if (!pageInk0.isEmpty()) {   // page ink UNDER margin ink (content vs annotation)
            ++inkLayers;
            out += QStringLiteral("<img class=\"ink\" src=\"%1\" alt=\"\">").arg(pageInk0);
        }
        if (!posterInk.isEmpty()) {
            ++inkLayers;
            out += QStringLiteral("<img class=\"ink\" src=\"%1\" alt=\"\"%2>")
                       .arg(posterInk, posterInkStyle);
        }
        out += QStringLiteral("</div>");
    } else if (!poster.isEmpty()) {
        out += QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(poster, htmlEscape(name));
    }
    out += QStringLiteral("<figcaption><div class=\"fname\">%1</div>"
                          "<div class=\"fpath\">%2</div>"
                          "<div class=\"fmeta\">%3 · %4</div></figcaption></figure>")
               .arg(htmlEscape(name), htmlEscape(path), htmlEscape(kind), htmlEscape(meta));

    if (kind == QLatin1String("pdf")) {
        // Poster + annotated pages (ruling 2026-08-20): one figure per OTHER
        // page carrying ink/chips — clean page base + toggleable overlay.
        const int pages = m->mediaPdfPages(row);
        for (int pg = 1; pg < pages; ++pg) {
            if (m->pdfPageInk(row, pg).isEmpty()) continue;
            const QImage base = MediaStore::renderPdfPage(path, pg, 1280);
            if (base.isNull()) continue;
            const QString baseUrl = sink.addImage(base,
                sanitizedBase(path) + QStringLiteral("_page%1").arg(pg + 1));
            if (baseUrl.isEmpty()) continue;
            const QImage ov = renderPdfPageInkOverlay(m, row, pg, base.size());
            QString ovUrl;
            if (!ov.isNull())
                ovUrl = sink.addImage(ov,
                    sanitizedBase(path) + QStringLiteral("_page%1_ink").arg(pg + 1));
            out += QStringLiteral("<figure class=\"ref\">");
            if (!ovUrl.isEmpty()) {
                ++inkLayers;
                out += QStringLiteral("<div class=\"inkwrap\"><img src=\"%1\" alt=\"%2\">"
                                      "<img class=\"ink\" src=\"%3\" alt=\"\"></div>")
                           .arg(baseUrl, htmlEscape(name), ovUrl);
            } else {
                out += QStringLiteral("<img src=\"%1\" alt=\"%2\">")
                           .arg(baseUrl, htmlEscape(name));
            }
            out += QStringLiteral("<figcaption><div class=\"fmeta\">%1 · page %2 of %3</div>"
                                  "</figcaption></figure>")
                       .arg(htmlEscape(name)).arg(pg + 1).arg(pages);
        }
    }

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
// Syntax-coloured code for the HTML export: the SAME engine, theme, and
// lenient language resolver as the app's code blocks (CodeHighlighter), with
// token spans emitted as inline-styled <span>s — self-contained, theme-exact.
class HtmlCodeEmitter : public KSyntaxHighlighting::AbstractHighlighter {
public:
    HtmlCodeEmitter() {
        setTheme(codeHighlightRepo().defaultTheme(
            KSyntaxHighlighting::Repository::DarkTheme));
    }
    // The theme's editor background — the app fills code blocks with this
    // (Editor.qml matches the token colours the same way).
    QColor themeBackground() const {
        return QColor(theme().editorColor(KSyntaxHighlighting::Theme::BackgroundColor));
    }
    // Escaped, span-wrapped HTML for `code`; plain escape when the language
    // resolves to no definition (same silent fallback as the app).
    QString colorize(const QString& lang, const QString& code) {
        const KSyntaxHighlighting::Definition def = resolveCodeDefinition(lang);
        if (!def.isValid()) return htmlEscape(code);
        setDefinition(def);
        out_.clear();
        KSyntaxHighlighting::State st;
        const QStringList lines = code.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            line_ = lines.at(i);
            st = highlightLine(line_, st);
            if (i + 1 < lines.size()) out_ += QLatin1Char('\n');
        }
        return out_;
    }
protected:
    void applyFormat(int offset, int length,
                     const KSyntaxHighlighting::Format& f) override {
        const QString seg = htmlEscape(line_.mid(offset, length));
        QString style;
        if (f.hasTextColor(theme()))
            style += QStringLiteral("color:%1;").arg(f.textColor(theme()).name());
        if (f.isBold(theme()))      style += QStringLiteral("font-weight:bold;");
        if (f.isItalic(theme()))    style += QStringLiteral("font-style:italic;");
        if (f.isUnderline(theme())) style += QStringLiteral("text-decoration:underline;");
        if (style.isEmpty()) out_ += seg;
        else out_ += QStringLiteral("<span style=\"%1\">%2</span>").arg(style, seg);
    }
private:
    QString line_, out_;
};

// the dark page, the 760px measure, squared corners, rationed accent, the
// QCView-note violet. System font stacks (no bundled fonts in v1).
const char* kHtmlCss = R"CSS(
:root{--bg:#181817;--desk:#121211;--sheetw:1000px;
--text:#e4e3e2;--bright:#f0f0f0;--muted:#8a8a8a;--subtle:#5e5e5e;
--border:#2a2a2a;--divider:#333333;--accent:#0189f1;--recess:#0e0e0e;
--chipbg:#1d2733;--chiptext:#4aa8ff;--codetext:#d4d4e8;--violet:#b48ef0;--sel:#2a568c;--quote:#3a5e86}
/* The app's dual-tone ground (user ruling 2026-08-20): the SHEET — page +
   equal gutters, stretched to the widest table (--sheetw = the Editor's
   sheetSpan) — keeps the field tone; beyond it the ground drops to the
   window-shell desk tone, so the document reads as the same constrained
   shape as in the app. Hard-stop gradient on html = the one paint. */
*{box-sizing:border-box}
/* Explicit size + no-repeat: the root's background image is sized to its
   BOX (≈ viewport) and tiles across the scrollable canvas, so a table
   scrolling past the first viewport-width would repaint the boundary in
   the wrong place. One oversized image, desk colour beyond it. */
html{background-color:var(--desk);
background-image:linear-gradient(90deg,var(--bg) var(--sheetw),var(--desk) var(--sheetw));
background-size:calc(var(--sheetw) + 100vw) 100%;
background-repeat:no-repeat}
body{background:transparent;color:var(--text);margin:0;
font:15px/1.65 -apple-system,BlinkMacSystemFont,"Segoe UI",Inter,Roboto,sans-serif}
/* Left-anchored page (user ruling): the prose measure hugs the left edge
   rather than centering, so wide tables growing rightward read as one
   left-aligned system instead of breaking a centered frame. */
main{max-width:1000px;min-width:1000px;margin:0;padding:48px 120px 96px}
/* content = the app's true 760 measure, flanked by the app's 120px ink
   gutters so margin annotations render instead of cropping at the
   viewport's left origin. min-width == max-width (user ruling 2026-08-20):
   the page NEVER shrinks under the document width — narrow windows scroll
   instead of reflowing, so ink/annotation geometry always aligns. */
/* Document header: the export leads with the document's name (2026-08-20). */
header.doctitle{font-size:26px;font-weight:700;color:var(--bright);
padding-bottom:14px;margin-bottom:28px;border-bottom:1px solid var(--divider)}
/* The BLOCK RULER: every block's number in the right margin (the app's
   rail). Elements host the span; all numbers align on one ledger line.
   No rule line: the app made its rules focus-reactive, so a static export
   shows numbers only. */
main p,main h1,main h2,main h3,main h4,main h5,main h6,main blockquote,
main li,main figure,main .tablewrap,main .blkw{position:relative}
.bnum{position:absolute;left:772px;top:4px;width:calc(100vw - 932px);   /* 932 = the 836
   ledger constant + the 96px the wider left gutter shifted every block */
min-width:48px;padding-top:3px;
text-align:right;font-family:ui-monospace,Menlo,Consolas,monospace;
font-size:11px;color:var(--subtle);user-select:none;pointer-events:none}
/* (Block click-select was built then removed 2026-08-20 — user veto; the
   ledger stays a passive reference like the app's rail at rest.) */
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
/* Code escapes the measure like tables do (user ruling: capping code is
   purely aesthetic): natural width, page-level scroll, no inner scrollbar. */
pre{background:var(--recess);border:1px solid var(--border);padding:12px 14px;
width:max-content;min-width:100%;color:var(--codetext)}
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
/* Lightbox: click any figure image for full-screen; ‹ › buttons or arrow
   keys navigate document order, Esc / backdrop closes. Annotation overlays
   ride along (their percent offsets scale with the staged image) and honour
   the Annotations toggle. Squared, monochrome — the family chrome. */
main figure img:not(.ink),main .notecard img:not(.ink),main td img{cursor:zoom-in}
#mn-lb{position:fixed;inset:0;background:rgba(0,0,0,.92);z-index:99;
display:flex;align-items:center;justify-content:center;
opacity:0;visibility:hidden;pointer-events:none;
transition:opacity 160ms ease,visibility 0s linear 160ms}   /* fade out, THEN hide */
#mn-lb.open{opacity:1;visibility:visible;pointer-events:auto;
transition:opacity 160ms ease}
@keyframes mnlb-in{from{opacity:0}to{opacity:1}}
#mn-lb .stage>div{position:relative;display:inline-block;
animation:mnlb-in 160ms ease}   /* each staged photo reveals (rebuilt per show) */
#mn-lb .stage>div>img:first-child{max-width:86vw;max-height:92vh;display:block}
#mn-lb .nav{position:fixed;top:50%;transform:translateY(-50%);z-index:100;
width:44px;height:64px;background:rgba(0,0,0,.55);color:var(--text);
border:1px solid var(--border);border-radius:0;cursor:pointer;padding:0;
display:flex;align-items:center;justify-content:center}
/* Inline SVG glyphs: centered by geometry, not font metrics — identical on
   every platform (text glyphs sat visibly low on some). currentColor keeps
   the hover tint. */
#mn-lb .nav svg{display:block}
#mn-lb .nav:hover{background:var(--divider);color:var(--bright)}
#mn-lb .prev{left:16px}
#mn-lb .next{right:16px}
#mn-lb .close{position:fixed;top:14px;right:16px;transform:none;
width:44px;height:44px}
#mn-lb .cnt{position:fixed;bottom:14px;left:50%;transform:translateX(-50%);
color:var(--subtle);font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px}
@media print{#mn-lb{display:none!important}}
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
    // The document's page measure (v3; 760 for pre-v3 docs) — every 760-era
    // geometry constant below derives from it so exports match the app's
    // live layout at any width.
    const int pw = int(std::lround(m->pageWidth()));
    FootnoteCtx fn;
    QString body;
    int inkLayers = 0;                // stacked ink layers emitted (gates the toggle)
    HtmlCodeEmitter codeEmitter;      // syntax colours, the app's engine + theme
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
    auto bnumLi = [pw](int row, int depth) {
        return QStringLiteral("<span class=\"bnum\" style=\"left:%1px\">%2</span>")
            .arg((pw + 12) - 24 * (depth + 1)).arg(row + 1);
    };
    // Page ink rides INSIDE its block element (the positioned ancestor):
    // injected before the block's final closing tag. X anchors to the page
    // center (pw/2 — 380 in the classic 760 frame), minus the element's
    // own indent.
    auto injectInk = [&](QString blk, int row, double indent) -> QString {
        const TextInk ti = renderTextInk(m, row);
        if (ti.img.isNull()) return blk;
        const QString src = sink.addImage(ti.img, QStringLiteral("pageink"));
        if (src.isEmpty()) return blk;
        ++inkLayers;
        // max-width:none: the global img{max-width:100%} would clamp a
        // margin-spanning layer to the 760 block and squeeze its right side.
        const QString tag = QStringLiteral(
            "<img class=\"ink\" style=\"position:absolute;left:%1px;top:%2px;"
            "width:%3px;height:%4px;max-width:none;z-index:2\" src=\"%5\" alt=\"\">")
            .arg(pw / 2.0 + ti.box.left() - indent)
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
        case BlockModel::Code: {
            const QString lang = m->languageForRow(row);
            body += injectInk(QStringLiteral("<div class=\"blkw\">%1<pre><code class=\"language-%2\">%3</code></pre></div>\n")
                        .arg(bnum(row), htmlEscape(lang),
                             codeEmitter.colorize(lang, m->contentForRow(row))), row, 0);
            break;
        }
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

    // Code blocks fill with the syntax THEME's editor background (the app's
    // recipe, Editor.qml) so the token colours sit on the surface they were
    // designed for — patch it over the static recess tone.
    QString css = QLatin1String(kHtmlCss);
    const QColor codeBg = codeEmitter.themeBackground();
    if (codeBg.isValid())
        css.replace(QLatin1String("pre{background:var(--recess)"),
                    QStringLiteral("pre{background:%1").arg(codeBg.name()));

    // Per-document page width: patch the 760-era geometry — the column
    // (w+240 incl. the two 120px gutters) and the block-number ledger
    // (left = w+12; viewport reserve = w+172) — the codeBg pattern.
    if (pw != 760) {
        css.replace(QLatin1String("max-width:1000px"),
                    QStringLiteral("max-width:%1px").arg(pw + 240));
        css.replace(QLatin1String("min-width:1000px"),
                    QStringLiteral("min-width:%1px").arg(pw + 240));
        css.replace(QLatin1String("left:772px"),
                    QStringLiteral("left:%1px").arg(pw + 12));
        css.replace(QLatin1String("100vw - 932px"),
                    QStringLiteral("100vw - %1px").arg(pw + 172));
    }

    // The sheet band follows the widest table (the Editor's sheetSpan rule):
    // its right edge = the widest table's RENDERED width (same min/max
    // expression the table itself carries) + the trailing gutter.
    {
        qreal widest = 0;
        for (int r = 0; r < m->rowCountQml(); ++r) {
            if (m->typeForRow(r) != BlockModel::Table) continue;
            const int tc = exportCols(m, r);
            qreal t = 0;
            for (int c = 0; c < tc; ++c) t += exportColWidth(m, r, c);
            widest = std::max(widest, t);
        }
        if (widest > pw)
            css.replace(QLatin1String("--sheetw:1000px"),
                QStringLiteral("--sheetw:calc(min(%1px,max(%2px,100vw - 152px)) + 240px)")
                    .arg(int(widest)).arg(pw));
        else if (pw != 760)
            css.replace(QLatin1String("--sheetw:1000px"),
                        QStringLiteral("--sheetw:%1px").arg(pw + 240));
    }

    // Lightbox: the page's one script (everything else is CSS-only). Vanilla,
    // self-contained; removes itself when the page has no figure images. The
    // stage rebuilds base + (toggle-honouring) annotation overlay per show —
    // the overlay reuses the layer's own percent geometry, so overshoot ink
    // scales with the staged image exactly as it does in the page.
    static const char kLightbox[] = R"MNLB(<div id="mn-lb"><button class="nav prev" aria-label="Previous"><svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 5 8 12l7 7"/></svg></button><div class="stage"></div><button class="nav next" aria-label="Next"><svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 5l7 7-7 7"/></svg></button><button class="nav close" aria-label="Close"><svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M6 6l12 12M18 6 6 18"/></svg></button><span class="cnt"></span></div>
<script>
(function(){
var lb=document.getElementById('mn-lb');if(!lb)return;
var imgs=[].slice.call(document.querySelectorAll('main figure img,main .notecard img,main td img')).filter(function(im){return im.className.indexOf('ink')<0});
if(!imgs.length){lb.parentNode.removeChild(lb);return;}
var stage=lb.querySelector('.stage'),cnt=lb.querySelector('.cnt'),cur=-1;
function show(i){
cur=(i+imgs.length)%imgs.length;
var im=imgs[cur];
stage.innerHTML='';
var wrap=document.createElement('div');
var b=document.createElement('img');b.src=im.src;wrap.appendChild(b);
var tgl=document.getElementById('mn-ink');
var p=im.parentElement;
var inks=(p&&(p.className.indexOf('inkwrap')>=0||p.className.indexOf('thumb')>=0))
?p.querySelectorAll('img.ink'):[];
if(!tgl||tgl.checked)[].forEach.call(inks,function(ink){
var o=document.createElement('img');o.src=ink.src;
var st=ink.getAttribute('style');
o.setAttribute('style',st||'left:0;top:0;width:100%;max-width:none');
o.style.position='absolute';
wrap.appendChild(o);});
stage.appendChild(wrap);
cnt.textContent=(cur+1)+' / '+imgs.length;
lb.classList.add('open');}
imgs.forEach(function(im,i){im.addEventListener('click',function(){show(i)})});
function close(){lb.classList.remove('open');cur=-1}
lb.addEventListener('click',function(e){if(e.target===lb)close()});
lb.querySelector('.prev').addEventListener('click',function(){show(cur-1)});
lb.querySelector('.next').addEventListener('click',function(){show(cur+1)});
lb.querySelector('.close').addEventListener('click',close);
document.addEventListener('keydown',function(e){
if(cur<0)return;
if(e.key==='Escape')close();
else if(e.key==='ArrowLeft')show(cur-1);
else if(e.key==='ArrowRight')show(cur+1);
else return;
e.preventDefault();});
})();
</script>
)MNLB";

    return QStringLiteral(
               "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
               "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
               "<meta name=\"generator\" content=\"minNotes\">\n"
               "<title>%1</title>\n<style>%2</style>\n</head>\n<body>\n%3<main>\n"
               "<header class=\"doctitle\">%1</header>\n%4</main>\n"
               "%5</body>\n</html>\n")
        .arg(htmlEscape(m->documentName()), css, toggle, body,
             QLatin1String(kLightbox));
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

void docxPlainPara(QXmlStreamWriter& w, const QString& text, const DocxRunProps& rp,
                   const std::function<void(QXmlStreamWriter&)>& pPr);   // defined below

// Code blocks as COLORED runs (2026-08-20 — the last format without
// syntax colors): KSyntaxHighlighting LightTheme segments, each run
// keeping the Courier + EFEFEF shading so the DOCX importer's code sniff
// (font + shd) still coalesces the block on round-trip. No definition for
// the language → the old plain paragraph, verbatim.
class DocxCodeEmitter : public KSyntaxHighlighting::AbstractHighlighter {
public:
    DocxCodeEmitter() {
        setTheme(codeHighlightRepo().defaultTheme(
            KSyntaxHighlighting::Repository::LightTheme));
    }
    void emitPara(QXmlStreamWriter& w, const QString& lang, const QString& code) {
        DocxRunProps base;
        base.code = true;
        base.halfPtSize = 18;   // 9pt — the existing code-paragraph size
        const KSyntaxHighlighting::Definition def = resolveCodeDefinition(lang);
        if (!def.isValid()) { docxPlainPara(w, code, base, nullptr); return; }
        setDefinition(def);
        w_ = &w;
        base_ = base;
        w.writeStartElement(QStringLiteral("w:p"));
        KSyntaxHighlighting::State st;
        const QStringList lines = code.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (i > 0) {   // line-break run keeps the shading continuous
                w.writeStartElement(QStringLiteral("w:r"));
                docxRunProps(w, base_);
                w.writeEmptyElement(QStringLiteral("w:br"));
                w.writeEndElement();
            }
            line_ = lines.at(i);
            st = highlightLine(line_, st);
        }
        w.writeEndElement();
        w_ = nullptr;
    }
protected:
    void applyFormat(int offset, int length,
                     const KSyntaxHighlighting::Format& f) override {
        DocxRunProps rp = base_;
        if (f.hasTextColor(theme())) rp.color = f.textColor(theme()).name();
        if (f.isBold(theme()))       rp.b = true;
        if (f.isItalic(theme()))     rp.i = true;
        if (f.isUnderline(theme()))  rp.u = true;
        docxTextRun(*w_, line_.mid(offset, length), rp);
    }
private:
    QXmlStreamWriter* w_ = nullptr;
    DocxRunProps base_;
    QString line_;
};

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
    const int rows = m->tableRows(row), cols = exportCols(m, row);
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
    // Normalized to the printable page (≈9360 dxa, letter with 1" margins)
    // when they'd overflow — a 26-column sheet import was a 90cm table.
    std::vector<double> colDxa(size_t(cols), 0.0);
    {
        double total = 0;
        for (int cix = 0; cix < cols; ++cix) {
            colDxa[size_t(cix)] = exportColWidth(m, row, cix) * 15.0;   // app-measured
            total += colDxa[size_t(cix)];
        }
        constexpr double kPageDxa = 9360.0;
        if (total > kPageDxa)
            for (double& d : colDxa) d *= kPageDxa / total;
    }
    w.writeStartElement(QStringLiteral("w:tblGrid"));
    for (int cix = 0; cix < cols; ++cix) {
        w.writeStartElement(QStringLiteral("w:gridCol"));
        w.writeAttribute(QStringLiteral("w:w"),
                         QString::number(int(colDxa[size_t(cix)])));
        w.writeEndElement();
    }
    w.writeEndElement();
    for (int r = 0; r < rows; ++r) {
        w.writeStartElement(QStringLiteral("w:tr"));
        for (int cix = 0; cix < cols; ++cix) {
            w.writeStartElement(QStringLiteral("w:tc"));
            w.writeStartElement(QStringLiteral("w:tcPr"));
            // Header-AGNOSTIC (user ruling 2026-08-20): only AUTHORED cell
            // colours shade — the header flag can't be trusted on sheet
            // imports, so no row gets promoted styling.
            const QString bg = m->tableCellBg(row, r, cix);
            if (!bg.isEmpty()) {
                w.writeStartElement(QStringLiteral("w:shd"));
                w.writeAttribute(QStringLiteral("w:val"), QStringLiteral("clear"));
                w.writeAttribute(QStringLiteral("w:fill"),
                    QString(bg).remove(QLatin1Char('#')).toUpper());
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
            // Cell image (plain columns only, like the other emitters):
            // its own paragraph above the text, capped to the column width.
            if (kind == 0 && !m->tableCellMedia(row, r, cix).isEmpty()) {
                const QImage img(localPathOf(m->tableCellMediaUrl(row, r, cix)));
                if (!img.isNull() && img.width() > 0) {
                    const int dwOv = m->tableCellMediaDw(row, r, cix);
                    double dispW = dwOv > 0 ? dwOv : img.width();
                    // Cap to the (normalized) cell width, dxa → px.
                    dispW = std::min(dispW,
                                     std::max(8.0, colDxa[size_t(cix)] / 15.0 - 10.0));
                    docxImagePara(c, w, docxAddImage(c, img),
                                  int(dispW),
                                  int(dispW * img.height() / double(img.width())));
                }
            }
            DocxRunProps rp;
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
        const MediaInk ink = renderMediaInk(m, row, img.size());
        if (!ink.img.isNull()) {
            QPainter p(&img);
            // The raster covers frame ∪ overshoot; place it at its offset —
            // overshoot beyond the image clips in the bake (a composited
            // DOCX image has nowhere else to put it).
            p.drawImage(QPointF(ink.boxNorm.left() * img.width(),
                                ink.boxNorm.top() * img.height()), ink.img);
            p.end();
            ++c.inkBaked;
        }
        return img;
    };

    if (kind == QLatin1String("sketch")) {
        const QImage img = renderSketch(m, row);
        if (!img.isNull()) {
            // Size by the SOURCE canvas px, not the (possibly 2×) raster —
            // docxImagePara treats px as 96-dpi, like every other image here.
            const int srcW = m->mediaW(row), srcH = m->mediaH(row);
            docxImagePara(c, w, docxAddImage(c, img),
                          srcW > 0 ? srcW : img.width(),
                          srcH > 0 ? srcH : img.height());
        }
        return;
    }
    if (kind == QLatin1String("image")) {
        QImage img(path);
        if (!img.isNull()) {
            const bool hasInk = mn::docInkHasContent(m->inkForRow(row));   // strokes OR chips
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
        // Page 1's PAGE ink/chips bake into the poster (content, like sketch
        // strokes); margin ink bakes after, via the generic path below.
        poster = renderPdfPageAnnotated(m, row, 0, path, 1280);
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

    if (kind == QLatin1String("pdf")) {
        // Poster + annotated pages (ruling 2026-08-20): one baked image +
        // caption per OTHER page carrying ink/chips.
        const int pages = m->mediaPdfPages(row);
        for (int pg = 1; pg < pages; ++pg) {
            if (m->pdfPageInk(row, pg).isEmpty()) continue;
            const QImage img = renderPdfPageAnnotated(m, row, pg, path, 1280);
            if (img.isNull()) continue;
            docxImagePara(c, w, docxAddImage(c, img), img.width(), img.height());
            docxPlainPara(w, QStringLiteral("%1 · page %2 of %3")
                                 .arg(name).arg(pg + 1).arg(pages), mono);
        }
    }

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
        DocxCodeEmitter codeEmitter;   // one theme/definition resolver per export
        w.writeStartElement(QStringLiteral("w:document"));
        w.writeAttribute(QStringLiteral("xmlns:w"),
            QStringLiteral("http://schemas.openxmlformats.org/wordprocessingml/2006/main"));
        w.writeAttribute(QStringLiteral("xmlns:r"),
            QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships"));
        w.writeAttribute(QStringLiteral("xmlns:wp"),
            QStringLiteral("http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"));
        w.writeStartElement(QStringLiteral("w:body"));

        {   // Document header (ruling 2026-08-20): the name leads every export.
            DocxRunProps title;
            title.b = true;
            title.halfPtSize = 48;   // 24pt
            docxPlainPara(w, m->documentName(), title, [](QXmlStreamWriter& pw) {
                pw.writeStartElement(QStringLiteral("w:pBdr"));
                pw.writeStartElement(QStringLiteral("w:bottom"));
                pw.writeAttribute(QStringLiteral("w:val"), QStringLiteral("single"));
                pw.writeAttribute(QStringLiteral("w:sz"), QStringLiteral("6"));
                pw.writeAttribute(QStringLiteral("w:color"), QStringLiteral("BBBBBB"));
                pw.writeEndElement();
                pw.writeEndElement();
            });
            docxPlainPara(w, QString(), {});   // breathing room under the rule
        }

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
            case BlockModel::Code:
                codeEmitter.emitPara(w, m->languageForRow(row), m->contentForRow(row));
                break;
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

// ============================ PDF export ============================
// QTextDocument built programmatically via QTextCursor, printed to a
// QPdfWriter (design 2026-08-20; ruling: this pipeline over a hand-rolled
// painter). White-paper print styling (the DOCX ruling); 96-dpi layout
// units so numbers stay CSS-px-meaningful; ink pre-baked into images —
// QTextDocument has no overlay primitive. See Exporter.h for the v1
// print-lossiness list.

namespace {

const QColor kPdfText(0x20, 0x20, 0x20);
const QColor kPdfMuted(0x77, 0x77, 0x77);
const QColor kPdfAccent(0x01, 0x66, 0xc0);   // print-legible accent
const QColor kPdfBorder(0xC8, 0xC8, 0xC8);

QStringList pdfMonoFamilies() {
    return {QStringLiteral("JetBrains Mono"), QStringLiteral("Menlo"),
            QStringLiteral("Consolas"), QStringLiteral("Courier New")};
}

struct PdfCtx {
    const BlockModel* m = nullptr;
    Exporter::Options opt;
    QTextDocument* doc = nullptr;
    QTextCursor cur;
    FootnoteCtx fn;
    int imgSeq = 0;
    qreal contentW = 0, contentH = 0;   // page paint rect, 96-dpi units
    // QTextDocumentLayout scales INLINE OBJECT sizes (images — not text) by
    // deviceDpi / platform-default-dpi, and macOS's default is 72 — so a
    // format set to N units drew at N×1.33 (the "full-size images offpage"
    // bug, 2026-08-20). Every QTextImageFormat size is pre-multiplied by
    // this factor (defaultDpi/96) so the DRAWN size equals the intended
    // device units on every platform.
    qreal imgFmt = 1.0;
    bool first = true;                  // reuse the document's initial block once

    // Every structure appends; normalizing to the root frame's end keeps the
    // cursor sane after frames/tables (which capture it).
    void toEnd() { cur = doc->rootFrame()->lastCursorPosition(); }
    void newBlock(const QTextBlockFormat& bf, const QTextCharFormat& cf = {}) {
        if (first) { cur.setBlockFormat(bf); cur.setBlockCharFormat(cf); first = false; }
        else cur.insertBlock(bf, cf);
    }
};

QString pdfAddImage(PdfCtx& c, const QImage& img) {
    const QString name = QStringLiteral("mnimg:%1").arg(++c.imgSeq);
    c.doc->addResource(QTextDocument::ImageResource, QUrl(name), img);
    return name;
}

// Insert an image block, display size capped to the content rect (an image
// must fit ONE page — it is a single line to the layout).
void pdfInsertImage(PdfCtx& c, const QImage& img, qreal displayW) {
    if (img.isNull() || displayW <= 0 || img.width() <= 0) return;
    qreal w = std::min(displayW, c.contentW);
    qreal h = w * img.height() / double(img.width());
    // Fit ONE page with certainty (ruling 2026-08-20): leave room for the
    // block's own margins AND a caption line, so an image at the cap can
    // never tip its block past the page and clip.
    const qreal maxH = c.contentH - 48;
    if (maxH > 0 && h > maxH) { w *= maxH / h; h = maxH; }
    QTextBlockFormat bf; bf.setTopMargin(6); bf.setBottomMargin(6);
    c.newBlock(bf);
    QTextImageFormat f;
    f.setName(pdfAddImage(c, img));
    f.setWidth(w * c.imgFmt); f.setHeight(h * c.imgFmt);
    c.cur.insertImage(f);
}

// Tri-state checkbox as an IMAGE (the app's .cb recipe) — font fallback for
// ☐/◐/☑ is unreliable headless, painted pixels are not. 2× raster, shown
// at `display` units.
QImage taskGlyphImage(int state) {
    const int px = 26;
    QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r(2, 2, px - 4, px - 4);
    if (state == 2) {                        // done: accent fill + white check
        p.fillRect(r, kPdfAccent);
        QPen pen(Qt::white, 2.6); pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(7, 13.5), QPointF(11, 18));
        p.drawLine(QPointF(11, 18), QPointF(19, 8));
    } else {
        p.setPen(QPen(QColor(0x99, 0x99, 0x99), 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);
        if (state == 1)                      // doing: half fill
            p.fillRect(QRectF(r.left() + 1, r.top() + 1,
                              r.width() / 2 - 1, r.height() - 2), kPdfAccent);
    }
    p.end();
    return img;
}

void pdfInsertTaskGlyph(PdfCtx& c, int state) {
    QTextImageFormat f;
    f.setName(pdfAddImage(c, taskGlyphImage(state)));
    f.setWidth(11 * c.imgFmt); f.setHeight(11 * c.imgFmt);
    c.cur.insertImage(f);
    c.cur.insertText(QStringLiteral(" "));
}

// One block's text + spans as merged char formats — per segment, every
// covering span's attributes merge (no nesting order to manage, unlike the
// marker emitters). Comment spans record footnote numbers and drop a
// superscript [n] at the span end.
void pdfInline(PdfCtx& c, int row, const QTextCharFormat& base) {
    const QString text = c.m->contentForRow(row);
    const int len = text.size();
    struct Run { int s, e; uint8_t kind; QString u; };
    std::vector<Run> runs;
    std::set<int> bounds{0, len};
    std::vector<std::pair<int, int>> notes;   // (end pos, footnote number)
    for (const QVariant& v : c.m->spansForRow(row)) {
        const QVariantMap sp = v.toMap();
        const uint8_t k = static_cast<uint8_t>(sp.value(QStringLiteral("k")).toInt());
        const int s = std::clamp(sp.value(QStringLiteral("s")).toInt(), 0, len);
        const int e = std::clamp(sp.value(QStringLiteral("e")).toInt(), 0, len);
        if (s >= e) continue;
        const QString u = sp.value(QStringLiteral("u")).toString();
        if (k == BlockModel::SpanComment) notes.push_back({e, c.fn.numberFor(u)});
        runs.push_back({s, e, k, u});
        bounds.insert(s); bounds.insert(e);
    }
    std::sort(notes.begin(), notes.end());
    size_t ni = 0;
    auto emitNotesAt = [&](int pos) {
        for (; ni < notes.size() && notes[ni].first == pos; ++ni) {
            QTextCharFormat nf = base;
            nf.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
            nf.setForeground(kPdfAccent);
            c.cur.insertText(QStringLiteral("[%1]").arg(notes[ni].second), nf);
        }
    };
    emitNotesAt(0);
    for (auto it = bounds.begin(); std::next(it) != bounds.end(); ++it) {
        const int s = *it, e = *std::next(it);
        QTextCharFormat f = base;
        for (const Run& r : runs) {
            if (r.s > s || e > r.e) continue;
            switch (r.kind) {
            case BlockModel::SpanBold:      f.setFontWeight(QFont::Bold); break;
            case BlockModel::SpanItalic:    f.setFontItalic(true); break;
            case BlockModel::SpanUnderline: f.setFontUnderline(true); break;
            case BlockModel::SpanStrike:    f.setFontStrikeOut(true); break;
            case BlockModel::SpanCode:
                f.setFontFamilies(pdfMonoFamilies());
                f.setFontPointSize(std::max(6.0, base.fontPointSize() > 0
                                                     ? base.fontPointSize() - 1.5 : 9.0));
                f.setBackground(QColor(0xEF, 0xEF, 0xEF));
                break;
            case BlockModel::SpanLink:      // engine emits URI annotations for anchors
                f.setAnchor(true);
                f.setAnchorHref(r.u);
                f.setForeground(kPdfAccent);
                f.setFontUnderline(true);
                break;
            case BlockModel::SpanFgColor:   f.setForeground(QColor(r.u)); break;
            case BlockModel::SpanHighlight:
                f.setBackground(QColor(r.u));
                f.setForeground(QColor(contrastOn(r.u)));
                break;
            case BlockModel::SpanComment:   // light tint — the print take on .cmt
                f.setBackground(QColor(0xDC, 0xEA, 0xFB));
                break;
            }
        }
        c.cur.insertText(text.mid(s, e - s), f);
        emitNotesAt(e);
    }
}

// KSyntaxHighlighting → cursor runs, LightTheme (white paper).
class PdfCodeEmitter : public KSyntaxHighlighting::AbstractHighlighter {
public:
    PdfCodeEmitter() {
        setTheme(codeHighlightRepo().defaultTheme(
            KSyntaxHighlighting::Repository::LightTheme));
    }
    QColor themeBackground() const {
        return QColor(theme().editorColor(KSyntaxHighlighting::Theme::BackgroundColor));
    }
    void emitTo(QTextCursor& cur, const QTextCharFormat& base,
                const QString& lang, const QString& code) {
        cur_ = &cur; base_ = base;
        const KSyntaxHighlighting::Definition def = resolveCodeDefinition(lang);
        const QStringList lines = code.split(QLatin1Char('\n'));
        if (!def.isValid()) {
            for (int i = 0; i < lines.size(); ++i) {
                cur.insertText(lines.at(i), base);
                if (i + 1 < lines.size())
                    cur.insertText(QString(QChar::LineSeparator), base);
            }
            return;
        }
        setDefinition(def);
        KSyntaxHighlighting::State st;
        for (int i = 0; i < lines.size(); ++i) {
            line_ = lines.at(i);
            st = highlightLine(line_, st);
            if (i + 1 < lines.size())
                cur.insertText(QString(QChar::LineSeparator), base);
        }
    }
protected:
    void applyFormat(int offset, int length,
                     const KSyntaxHighlighting::Format& f) override {
        QTextCharFormat cf = base_;
        if (f.hasTextColor(theme())) cf.setForeground(f.textColor(theme()));
        if (f.isBold(theme()))       cf.setFontWeight(QFont::Bold);
        if (f.isItalic(theme()))     cf.setFontItalic(true);
        if (f.isUnderline(theme()))  cf.setFontUnderline(true);
        cur_->insertText(line_.mid(offset, length), cf);
    }
private:
    QTextCursor* cur_ = nullptr;
    QTextCharFormat base_;
    QString line_;
};

// Code block: themed frame (LightTheme editor background), mono runs,
// wrap-anywhere — a fixed page has no horizontal scrollbar.
void pdfCode(PdfCtx& c, int row, PdfCodeEmitter& ce) {
    QTextFrameFormat ff;
    ff.setBackground(ce.themeBackground());
    ff.setBorder(0.5);
    ff.setBorderBrush(kPdfBorder);
    ff.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    ff.setPadding(8);
    ff.setTopMargin(6); ff.setBottomMargin(6);
    QTextFrame* fr = c.cur.insertFrame(ff);
    c.first = false;
    QTextCursor ic = fr->firstCursorPosition();
    QTextBlockFormat bf; bf.setNonBreakableLines(false);
    ic.setBlockFormat(bf);
    QTextCharFormat mono;
    mono.setFontFamilies(pdfMonoFamilies());
    mono.setFontPointSize(8.5);
    mono.setForeground(kPdfText);
    ce.emitTo(ic, mono, c.m->languageForRow(row), c.m->contentForRow(row));
    c.toEnd();
}

void pdfTable(PdfCtx& c, int row) {
    const BlockModel* m = c.m;
    const int rows = m->tableRows(row), cols = exportCols(m, row);
    if (rows <= 0 || cols <= 0) return;
    QTextTableFormat tf;
    tf.setBorder(0.5);
    tf.setBorderBrush(kPdfBorder);
    tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    tf.setBorderCollapse(true);
    tf.setCellPadding(4);
    tf.setCellSpacing(0);
    // NO setHeaderRowCount (user ruling 2026-08-20): Qt re-renders the header
    // row — shading, bold and all — at the top of EVERY page a long table
    // spans, and a sheet-import's "header" is often just its first data row.
    // The header keeps its shading once, where it actually is.
    tf.setTopMargin(6); tf.setBottomMargin(6);
    // Real column constraints: authored widths (160 when auto), normalized
    // to the content width. Without them Qt distributes by content whim AND
    // an image wider than its actual cell gets width-clamped by the layout
    // while keeping the explicit height — squeezed aspect + paint bleeding
    // over the rows below (user PDF, 2026-08-20). colWpt below is the true
    // per-column budget the cell images cap against.
    std::vector<qreal> colWpt(size_t(cols), 0.0);
    {
        qreal total = 0;
        for (int cix = 0; cix < cols; ++cix) {
            colWpt[size_t(cix)] = exportColWidth(m, row, cix);   // app-measured
            total += colWpt[size_t(cix)];
        }
        const qreal scale = (total > c.contentW) ? c.contentW / total : 1.0;
        QList<QTextLength> cons;
        cons.reserve(cols);
        for (int cix = 0; cix < cols; ++cix) {
            colWpt[size_t(cix)] *= scale;
            // Layout units, NOT image-format units — only QTextImageFormat
            // sizes carry the imgFmt dpi pre-multiplier.
            cons.append(QTextLength(QTextLength::FixedLength, colWpt[size_t(cix)]));
        }
        tf.setColumnWidthConstraints(cons);
    }
    if (c.first) { c.first = false; }
    QTextTable* t = c.cur.insertTable(rows, cols, tf);
    // Header-AGNOSTIC (user ruling 2026-08-20): only AUTHORED cell colours
    // shade; no row gets promoted bold/fill — the header flag can't be
    // trusted on sheet imports.
    for (int r = 0; r < rows; ++r) {
        for (int cix = 0; cix < cols; ++cix) {
            QTextTableCell cell = t->cellAt(r, cix);
            const QString bg = m->tableCellBg(row, r, cix);
            if (!bg.isEmpty()) {
                QTextCharFormat cf = cell.format();
                cf.setBackground(QColor(bg));
                cell.setFormat(cf);
            }
            QTextCursor cc = cell.firstCursorPosition();
            const int kind = m->tableColumnKind(row, cix);
            QTextCharFormat rf;
            rf.setForeground(kPdfText);
            rf.setFontPointSize(9.5);
            const QString fg = m->tableCellFg(row, r, cix);
            if (!fg.isEmpty()) rf.setForeground(QColor(fg));
            if (kind == 2) {   // check column → painted glyph (font-fallback-proof)
                QTextImageFormat gf;
                QImage g = taskGlyphImage(m->tableCellCheck(row, r, cix));
                gf.setName(pdfAddImage(c, g));
                gf.setWidth(11 * c.imgFmt); gf.setHeight(11 * c.imgFmt);
                cc.insertImage(gf);
            } else if (kind == 1) {
                cc.insertText(m->tableCellChoiceLabel(row, r, cix), rf);
            } else {
                if (!m->tableCellMedia(row, r, cix).isEmpty()) {
                    const QImage img(localPathOf(m->tableCellMediaUrl(row, r, cix)));
                    if (!img.isNull() && img.width() > 0) {
                        const int dwOv = m->tableCellMediaDw(row, r, cix);
                        qreal dispW = dwOv > 0 ? dwOv : img.width();
                        // Cap to the cell's TRUE width (the constraint above,
                        // minus padding+border) — an image wider than its
                        // cell gets width-clamped by the layout while the
                        // explicit height stays → squeezed + overlap.
                        dispW = std::min(dispW,
                                         std::max(8.0, colWpt[size_t(cix)] - 10.0));
                        qreal dispH = dispW * img.height() / qreal(img.width());
                        // A cell is a single layout line — keep the image
                        // safely inside one page (the pdfInsertImage rule).
                        const qreal maxH = c.contentH - 80;
                        if (maxH > 0 && dispH > maxH) {
                            dispW *= maxH / dispH; dispH = maxH;
                        }
                        QTextImageFormat f;
                        f.setName(pdfAddImage(c, img));
                        f.setWidth(dispW * c.imgFmt);
                        f.setHeight(dispH * c.imgFmt);
                        cc.insertImage(f);
                        if (!m->tableCell(row, r, cix).isEmpty())
                            cc.insertBlock();   // text under the image
                    }
                }
                cc.insertText(m->tableCell(row, r, cix), rf);
            }
        }
    }
    c.toEnd();
}

// Reference lines under posters (the DOCX typography: bold name, mono path,
// mono kind·meta).
void pdfRefLines(PdfCtx& c, const QString& name, const QString& path,
                 const QString& kindMeta) {
    QTextBlockFormat bf; bf.setBottomMargin(0); bf.setTopMargin(2);
    QTextCharFormat b; b.setFontWeight(QFont::Bold); b.setForeground(kPdfText);
    c.newBlock(bf, b); c.cur.insertText(name, b);
    QTextCharFormat mono;
    mono.setFontFamilies(pdfMonoFamilies());
    mono.setFontPointSize(8.0);
    mono.setForeground(kPdfMuted);
    c.newBlock(bf, mono); c.cur.insertText(path, mono);
    c.newBlock(bf, mono); c.cur.insertText(kindMeta, mono);
}

void pdfVideoNotes(PdfCtx& c, const QString& mediaPath) {
    std::vector<qcv::AnnotationNote> notes;
    if (!qcv::annotation_io::loadNotes(notes, mediaPath) || notes.empty()) return;
    const QString imagesDir = qcv::annotation_io::getImagesFolder(mediaPath);
    QTextBlockFormat bf; bf.setTopMargin(8);
    QTextCharFormat h; h.setFontWeight(QFont::Bold); h.setForeground(kPdfText);
    c.newBlock(bf, h);
    c.cur.insertText(QStringLiteral("Notes (%1)").arg(notes.size()), h);
    for (const qcv::AnnotationNote& n : notes) {
        // The emitVideoNotes thumb rule: annotated sibling, else recomposite
        // from the clean frame when strokes exist (inked notes never export
        // ink-less).
        const QString cleanName = qcv::annotation_io::generateImageFilename(n.timecode);
        QString annName = cleanName;
        annName.replace(QStringLiteral(".png"), QStringLiteral("_annotated.png"));
        QImage thumb(imagesDir + QLatin1Char('/') + annName);
        if (thumb.isNull()) {
            const QImage clean(imagesDir + QLatin1Char('/') + cleanName);
            if (!clean.isNull() && qcv::AnnotationSerializer::hasStrokes(n.annotation_data))
                thumb = qcv::renderNoteThumbnail(
                    clean, qcv::AnnotationSerializer::jsonStringToStrokes(n.annotation_data), 1, 1);
            if (thumb.isNull()) thumb = clean;
        }
        if (!thumb.isNull()) pdfInsertImage(c, thumb, 240);
        QTextBlockFormat nb; nb.setBottomMargin(4);
        QTextCharFormat mono;
        mono.setFontFamilies(pdfMonoFamilies());
        mono.setFontPointSize(8.5);
        mono.setForeground(n.addressed ? kPdfMuted : kPdfText);
        c.newBlock(nb, mono);
        QString line = n.timecode;
        if (!n.text.isEmpty()) line += QStringLiteral(" — ") + n.text;
        if (n.addressed) line += QStringLiteral(" — ✓ addressed");
        c.cur.insertText(line, mono);
    }
}

void pdfMedia(PdfCtx& c, int row) {
    const BlockModel* m = c.m;
    const QString kind = m->mediaKind(row);
    const QString path = m->mediaLocalPath(row);
    const QString name = QFileInfo(path).fileName();

    auto bakeInk = [&](QImage img) {   // the DOCX bake — composited, clipped
        const MediaInk ink = renderMediaInk(m, row, img.size());
        if (!ink.img.isNull()) {
            QPainter p(&img);
            p.drawImage(QPointF(ink.boxNorm.left() * img.width(),
                                ink.boxNorm.top() * img.height()), ink.img);
            p.end();
        }
        return img;
    };

    if (kind == QLatin1String("sketch")) {
        const QImage img = renderSketch(m, row);
        if (!img.isNull()) {
            const int srcW = m->mediaW(row);   // size by SOURCE px, not the 2× raster
            pdfInsertImage(c, img, srcW > 0 ? srcW : img.width());
        }
        return;
    }
    if (kind == QLatin1String("image")) {
        QImage img(path);
        if (!img.isNull()) {
            if (mn::docInkHasContent(m->inkForRow(row))) img = bakeInk(img);
            pdfInsertImage(c, img, img.width() / (img.devicePixelRatio() > 0
                                                      ? img.devicePixelRatio() : 1.0));
        }
        return;
    }

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
        poster = renderPdfPageAnnotated(m, row, 0, path, 1280);   // page ink baked
        meta = QStringLiteral("%1 pages").arg(m->mediaPdfPages(row));
    } else {
        const QFileInfo fi(path);
        meta = fi.exists() ? humanSize(fi.size()) : QStringLiteral("(unavailable)");
    }
    if (!poster.isNull()) {
        poster = bakeInk(poster);
        pdfInsertImage(c, poster, 480);
    }
    pdfRefLines(c, name, path, kind + QStringLiteral(" · ") + meta);

    if (kind == QLatin1String("pdf")) {
        // Poster + annotated pages (ruling 2026-08-20).
        const int pages = m->mediaPdfPages(row);
        for (int pg = 1; pg < pages; ++pg) {
            if (m->pdfPageInk(row, pg).isEmpty()) continue;
            const QImage img = renderPdfPageAnnotated(m, row, pg, path, 1280);
            if (img.isNull()) continue;
            pdfInsertImage(c, img, 480);
            QTextBlockFormat bf; bf.setTopMargin(0);
            QTextCharFormat mono;
            mono.setFontFamilies(pdfMonoFamilies());
            mono.setFontPointSize(8.0);
            mono.setForeground(kPdfMuted);
            c.newBlock(bf, mono);
            c.cur.insertText(QStringLiteral("%1 · page %2 of %3")
                                 .arg(name).arg(pg + 1).arg(pages), mono);
        }
    }
    if (kind == QLatin1String("video") && c.opt.includeVideoNotes)
        pdfVideoNotes(c, path);
}

void pdfComments(PdfCtx& c) {
    if (c.fn.threadIds.isEmpty()) return;
    QTextBlockFormat hb; hb.setTopMargin(20);
    QTextCharFormat h; h.setFontWeight(QFont::Bold); h.setFontPointSize(14);
    h.setForeground(kPdfText);
    c.newBlock(hb, h);
    c.cur.insertText(QStringLiteral("Comments"), h);
    const QVariantList threads = c.m->commentThreads();
    for (int i = 0; i < c.fn.threadIds.size(); ++i) {
        const QString id = c.fn.threadIds.at(i);
        bool resolved = false;
        for (const QVariant& tv : threads) {
            const QVariantMap t = tv.toMap();
            if (t.value(QStringLiteral("id")).toString() == id) {
                resolved = t.value(QStringLiteral("resolved")).toBool();
                break;
            }
        }
        QTextBlockFormat bf; bf.setTopMargin(6);
        QTextCharFormat mk; mk.setForeground(kPdfAccent); mk.setFontWeight(QFont::Bold);
        c.newBlock(bf, mk);
        c.cur.insertText(QStringLiteral("[%1]").arg(i + 1), mk);
        if (resolved) {
            QTextCharFormat rz; rz.setForeground(kPdfMuted); rz.setFontItalic(true);
            c.cur.insertText(QStringLiteral(" (resolved)"), rz);
        }
        for (const QVariant& mv : c.m->commentMessages(id)) {
            const QVariantMap msg = mv.toMap();
            QTextBlockFormat mb; mb.setLeftMargin(14); mb.setBottomMargin(1);
            QTextCharFormat body; body.setForeground(resolved ? kPdfMuted : kPdfText);
            c.newBlock(mb, body);
            c.cur.insertText(msg.value(QStringLiteral("body")).toString(), body);
            const qint64 created = msg.value(QStringLiteral("created")).toLongLong();
            if (created > 0) {
                QTextCharFormat st; st.setForeground(kPdfMuted); st.setFontPointSize(8.0);
                c.cur.insertText(QStringLiteral("  ")
                    + QDateTime::fromMSecsSinceEpoch(created)
                          .toString(QStringLiteral("yyyy-MM-dd hh:mm")), st);
            }
        }
    }
}

void buildPdfDoc(PdfCtx& c) {
    const BlockModel* m = c.m;
    PdfCodeEmitter ce;
    QTextList* curList = nullptr;
    int curListDepth = -1;
    bool curListOrdered = false;
    auto endList = [&] { curList = nullptr; curListDepth = -1; };

    const int count = m->rowCountQml();
    for (int row = 0; row < count; ++row) {
        const int type = m->typeForRow(row);
        switch (type) {
        case BlockModel::Heading: {
            endList();
            const int lvl = std::clamp(m->levelForRow(row), 1, 6);
            static const qreal pts[6] = {20, 16, 13.5, 12, 11, 10.5};
            QTextBlockFormat bf;
            bf.setTopMargin(lvl <= 2 ? 16 : 12);
            bf.setBottomMargin(4);
            QTextCharFormat f;
            f.setFontWeight(QFont::Bold);
            f.setFontPointSize(pts[lvl - 1]);
            f.setForeground(QColor(0x10, 0x10, 0x10));
            c.newBlock(bf, f);
            pdfInline(c, row, f);
            break;
        }
        case BlockModel::Quote: {
            endList();
            QTextBlockFormat bf;
            bf.setLeftMargin(24);
            bf.setTopMargin(4); bf.setBottomMargin(4);
            QTextCharFormat f;
            f.setFontItalic(true);
            f.setForeground(QColor(0x55, 0x55, 0x55));
            c.newBlock(bf, f);
            pdfInline(c, row, f);
            break;
        }
        case BlockModel::Code:
            endList();
            pdfCode(c, row, ce);
            break;
        case BlockModel::ListItem:
        case BlockModel::TaskListItem:
        case BlockModel::OrderedListItem: {
            const int depth = std::clamp(m->depthForRow(row), 0, BlockModel::kMaxListDepth);
            const bool ordered = (type == BlockModel::OrderedListItem);
            QTextBlockFormat bf;
            bf.setTopMargin(1); bf.setBottomMargin(1);
            QTextCharFormat f; f.setForeground(kPdfText);
            c.newBlock(bf, f);
            if (curList && curListDepth == depth && curListOrdered == ordered) {
                curList->add(c.cur.block());
            } else {
                QTextListFormat lf;
                lf.setStyle(ordered ? QTextListFormat::ListDecimal
                                    : QTextListFormat::ListDisc);
                lf.setIndent(depth + 1);
                if (ordered) lf.setStart(m->orderedNumberForRow(row));
                curList = c.cur.createList(lf);
                curListDepth = depth;
                curListOrdered = ordered;
            }
            if (type == BlockModel::TaskListItem)
                pdfInsertTaskGlyph(c, m->taskStateForRow(row));
            pdfInline(c, row, f);
            break;
        }
        case BlockModel::Divider: {
            endList();
            QImage line(8, 1, QImage::Format_ARGB32_Premultiplied);
            line.fill(kPdfBorder);
            QTextBlockFormat bf; bf.setTopMargin(10); bf.setBottomMargin(10);
            c.newBlock(bf);
            QTextImageFormat f;
            f.setName(pdfAddImage(c, line));
            f.setWidth(c.contentW * c.imgFmt); f.setHeight(1 * c.imgFmt);
            c.cur.insertImage(f);
            break;
        }
        case BlockModel::Table:
            endList();
            pdfTable(c, row);
            break;
        case BlockModel::Media:
            endList();
            pdfMedia(c, row);
            break;
        case BlockModel::Paragraph:
        default: {
            endList();
            QTextBlockFormat bf;
            bf.setTopMargin(2); bf.setBottomMargin(2);
            QTextCharFormat f; f.setForeground(kPdfText);
            c.newBlock(bf, f);
            pdfInline(c, row, f);
            break;
        }
        }
    }
}

} // namespace

bool Exporter::toPdf(const Options& opt, QIODevice& out) const {
    if (!model_) return false;
    QPdfWriter writer(&out);
    // 96 dpi = layout units ≈ CSS px (A4 content ≈ 750 units — the numbers in
    // this codebase stay meaningful). Output is vector text + embedded images
    // regardless; resolution only sets the coordinate unit.
    writer.setResolution(96);
    const bool metric =
        QLocale::system().measurementSystem() == QLocale::MetricSystem;
    writer.setPageSize(QPageSize(metric ? QPageSize::A4 : QPageSize::Letter));
    writer.setPageMargins(QMarginsF(18, 15, 18, 18), QPageLayout::Millimeter);
    writer.setCreator(QStringLiteral("minNotes"));
    const QString docName = model_->documentName();
    writer.setTitle(docName.isEmpty() ? QStringLiteral("minNotes document") : docName);

    QTextDocument doc;
    doc.setDocumentMargin(0);   // margins live on the WRITER only (double-apply risk)
    QFont body(QStringLiteral("Aspekta"));
    body.setPointSizeF(10.5);   // the app's 14px body at 96 dpi
    doc.setDefaultFont(body);

    PdfCtx c;
    c.m = model_;
    c.opt = opt;
    c.doc = &doc;
    c.cur = QTextCursor(&doc);
    const QRectF paint = writer.pageLayout().paintRectPixels(96);
    c.contentW = paint.width();
    c.contentH = paint.height();
    // The inline-object dpi factor (see PdfCtx::imgFmt): the layout draws
    // image formats at deviceDpi/defaultDpi, where defaultDpi is the primary
    // screen's logical dpi (72 on macOS, 96 on Windows; 96 headless).
    const QScreen* screen = QGuiApplication::primaryScreen();
    const qreal defaultDpi = screen ? screen->logicalDotsPerInchY() : 96.0;
    c.imgFmt = defaultDpi / 96.0;
    {   // Document header (ruling 2026-08-20): the name leads every export.
        QTextBlockFormat bf; bf.setBottomMargin(6);
        QTextCharFormat f;
        f.setFontWeight(QFont::Bold);
        f.setFontPointSize(22);
        f.setForeground(QColor(0x10, 0x10, 0x10));
        c.newBlock(bf, f);
        c.cur.insertText(writer.title(), f);
        QImage line(8, 1, QImage::Format_ARGB32_Premultiplied);
        line.fill(kPdfBorder);
        QTextBlockFormat rb; rb.setBottomMargin(18);
        c.newBlock(rb);
        QTextImageFormat rf;
        rf.setName(pdfAddImage(c, line));
        rf.setWidth(c.contentW * c.imgFmt); rf.setHeight(1 * c.imgFmt);
        c.cur.insertImage(rf);
    }
    buildPdfDoc(c);
    pdfComments(c);

    // Manual page loop (2026-08-20): QTextDocument::print() force-adds its
    // own hardcoded 2cm root-frame margin to a non-paginated document — ON
    // TOP of the writer margins — silently narrowing the text column ~150
    // units below the contentW every image was capped against (full-width
    // images cropped at the right edge). Pre-paginate at the writer's paint
    // rect and drive the pages ourselves: every margin lives on the writer,
    // exactly once. (Also the future seam for keep-with-next.)
    const qint64 before = out.pos();
    doc.documentLayout()->setPaintDevice(&writer);
    doc.setPageSize(QSizeF(c.contentW, c.contentH));
    const int pages = std::max(1, doc.pageCount());
    QPainter p(&writer);
    if (!p.isActive()) return false;
    for (int pg = 0; pg < pages; ++pg) {
        if (pg > 0) writer.newPage();
        p.save();
        p.translate(0, -qreal(pg) * c.contentH);
        const QRectF view(0, qreal(pg) * c.contentH, c.contentW, c.contentH);
        p.setClipRect(view);
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.clip = view;
        doc.documentLayout()->draw(&p, ctx);
        p.restore();
    }
    p.end();
    return out.pos() > before;
}

bool Exporter::exportPdf(const QString& fileUrlOrPath, bool includeVideoNotes) {
    QString outPath = localPathOf(fileUrlOrPath);
    if (outPath.isEmpty() || !model_) return false;
    if (!outPath.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
        outPath += QStringLiteral(".pdf");
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    Options opt;
    opt.includeVideoNotes = includeVideoNotes;
    const bool ok = toPdf(opt, f);
    f.close();
    return ok && f.error() == QFile::NoError && f.size() > 0;
}
