#include "Converter.h"
#include "BlockModel.h"
#include "CanvasDocument.h"
#include "Document.h"
#include "../notes/sketch_text.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>

namespace mn {

QString ConversionReport::summary() const {
    if (!converted) return QString();
    QStringList parts;
    auto n = [](int k, const char* one, const char* many) {
        return k == 1 ? QStringLiteral("1 %1").arg(QLatin1String(one)) : QStringLiteral("%1 %2").arg(k).arg(QLatin1String(many));
    };
    if (textInkElements) parts << n(textInkElements, "ink mark", "ink marks");
    if (sketches) parts << n(sketches, "sketch", "sketches");
    if (textShapes) parts << n(textShapes, "label", "labels");
    if (orphanInk) parts << n(orphanInk, "orphan mark dropped", "orphan marks dropped");
    QString s = QStringLiteral("Converted from a 1.0 document");
    if (!parts.isEmpty()) s += QStringLiteral(" (") + parts.join(QStringLiteral(", ")) + QLatin1Char(')');
    return s;
}

namespace {

constexpr double kInset = CanvasDocument::kPageInset;
constexpr double kGap = CanvasDocument::kPageGap;

QJsonObject parse(const QString& json) { return QJsonDocument::fromJson(json.toUtf8()).object(); }
QString compact(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

// Map every point of every shape through f; returns the bbox of the mapped points (null if none).
QJsonArray mapShapes(const QJsonArray& shapes, const std::function<QPointF(QPointF)>& f, QRectF* bbox) {
    QJsonArray out;
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    bool any = false;
    for (const QJsonValue& sv : shapes) {
        QJsonObject s = sv.toObject();
        QJsonArray pts;
        const double halfW = s.value(QStringLiteral("stroke_width")).toDouble(2.0) / 2.0;
        for (const QJsonValue& pv : s.value(QStringLiteral("points")).toArray()) {
            const QJsonArray p = pv.toArray();
            if (p.size() < 2) continue;
            const QPointF q = f(QPointF(p.at(0).toDouble(), p.at(1).toDouble()));
            pts.append(QJsonArray{ q.x(), q.y() });
            x0 = std::min(x0, q.x() - halfW); y0 = std::min(y0, q.y() - halfW);
            x1 = std::max(x1, q.x() + halfW); y1 = std::max(y1, q.y() + halfW);
            any = true;
        }
        s.insert(QStringLiteral("points"), pts);
        out.append(s);
    }
    if (bbox) *bbox = any ? QRectF(QPointF(x0, y0), QPointF(x1, y1)) : QRectF();
    return out;
}

// Rebase world-px shapes onto an element whose origin is `origin`.
QJsonArray rebase(const QJsonArray& shapes, QPointF origin) {
    return mapShapes(shapes, [origin](QPointF p) { return p - origin; }, nullptr);
}

QString inkJson(const QJsonArray& localShapes) {
    QJsonObject o;
    o.insert(QStringLiteral("version"), QStringLiteral("2.0"));
    o.insert(QStringLiteral("coordinate_system"), QStringLiteral("element-local"));
    o.insert(QStringLiteral("shapes"), localShapes);
    return compact(o);
}

Document::Element element(const QString& kind, const QRectF& r, int z, const QString& json) {
    Document::Element e;
    e.id = makeUlid();
    e.kind = kind;
    e.x = r.x(); e.y = r.y(); e.w = r.width(); e.h = r.height();
    e.z = z;
    e.json = json;
    return e;
}

// A text chip → a text shape element. Height is derived the way the sketch painted it (the
// shared sketch_text layout), in the chip's own source px.
Document::Element textShape(const SketchTextSpec& t, double srcW, QPointF origin, QSizeF scale, int z) {
    const double x = origin.x() + t.x * scale.width();
    const double y = origin.y() + t.y * scale.height();
    const double w = std::max(8.0, t.w * scale.width());
    SketchTextSpec norm = t;
    norm.w = srcW > 0 ? w / srcW : 1.0;   // the layout helper wants a frame-normalized width
    const double h = std::max(t.size * 1.4, sketchTextHeightSrc(norm, srcW));
    QJsonObject o;
    o.insert(QStringLiteral("shape"), QStringLiteral("text"));
    o.insert(QStringLiteral("text"), t.text);
    o.insert(QStringLiteral("fontSize"), t.size);
    o.insert(QStringLiteral("fill"), t.color.isValid() ? t.color.name(QColor::HexArgb) : QStringLiteral("#ffffffff"));
    return element(QStringLiteral("shape"), QRectF(x, y, w, h), z, compact(o));
}

} // namespace

bool convertV1(CanvasDocument& canvas, const QString& pageId, ConversionReport& rep) {
    Document& db = canvas.db();
    if (!db.isOpen()) return false;
    const auto pageEl = db.element(pageId);
    if (!pageEl || pageEl->kind != QLatin1String("page")) return false;
    const QPointF pageOrigin(pageEl->x, pageEl->y);
    const double pw = std::max(320.0, pageEl->w - 2.0 * kInset);   // the 1.0 measure
    int z = 1;                                                      // marks sit above the page
    rep = ConversionReport{};
    rep.converted = true;

    db.begin();

    // (1) Sketch blocks → a column of elements beside the page, in document order. Raw rows
    //     (before any model loads): the sketch JSON is read exactly as 1.0 wrote it.
    const double colX = pageEl->x + pageEl->w + kGap;
    double colY = pageEl->y;
    const QHash<QString, QString> legacyInk = db.allInk();
    for (const Document::BlockMeta& bm : db.skinnyScan()) {
        if (bm.type != QLatin1String("media")) continue;
        const QJsonObject root = parse(db.contentFor(bm.id));
        const QString kind = root.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("pdf")) {
            if (!root.value(QStringLiteral("ink")).toObject().isEmpty()) ++rep.pdfInkInline;
            continue;
        }
        if (kind != QLatin1String("sketch")) continue;
        const double w = std::max(64.0, root.value(QStringLiteral("w")).toDouble(760.0));
        const double h = std::max(64.0, root.value(QStringLiteral("h")).toDouble(480.0));
        const QPointF origin(colX, colY);
        // Strokes: frame-normalized → element-local px; widths are already source px.
        const QJsonArray shapes = mapShapes(root.value(QStringLiteral("shapes")).toArray(),
                                            [w, h](QPointF p) { return QPointF(p.x() * w, p.y() * h); }, nullptr);
        db.insertElement(element(QStringLiteral("ink"), QRectF(origin, QSizeF(w, h)), z++, inkJson(shapes)));
        for (const SketchTextSpec& t : parseSketchTexts(root)) {
            db.insertElement(textShape(t, w, origin, QSizeF(w, h), z++));
            ++rep.textShapes;
        }
        for (const QJsonValue& iv : root.value(QStringLiteral("images")).toArray()) {
            const QJsonObject img = iv.toObject();
            QJsonObject media;
            media.insert(QStringLiteral("kind"), QStringLiteral("image"));
            media.insert(QStringLiteral("src"), img.value(QStringLiteral("src")));
            if (img.contains(QStringLiteral("srcW"))) media.insert(QStringLiteral("w"), img.value(QStringLiteral("srcW")));
            if (img.contains(QStringLiteral("srcH"))) media.insert(QStringLiteral("h"), img.value(QStringLiteral("srcH")));
            QJsonObject json; json.insert(QStringLiteral("media"), media);
            const QRectF r(colX + img.value(QStringLiteral("x")).toDouble(0.0) * w,
                           colY + img.value(QStringLiteral("y")).toDouble(0.0) * h,
                           std::max(8.0, img.value(QStringLiteral("w")).toDouble(1.0) * w),
                           std::max(8.0, img.value(QStringLiteral("h")).toDouble(1.0) * h));
            db.insertElement(element(QStringLiteral("media"), r, z++, compact(json)));
            ++rep.sketchImages;
        }
        if (legacyInk.contains(bm.id)) { db.deleteInk(bm.id); ++rep.orphanInk; }   // margin ink on a sketch has no home
        db.deleteBlock(bm.id);
        colY += h + kGap;
        ++rep.sketches;
    }
    // Frame-space ink on media blocks moves into the descriptor during the page load below.
    for (auto it = legacyInk.constBegin(); it != legacyInk.constEnd(); ++it)
        if (parse(it.value()).value(QStringLiteral("space")).toString() == QLatin1String("frame")) ++rep.mediaInkMoved;

    // (2) A headless page model: the load path repairs structure and moves media frame ink into
    //     the descriptors; laid out at the 1.0 measure it gives every block its estimated y.
    {
        BlockModel page;
        page.attachPage(&canvas, pageId);
        page.setContentWidth(pw);
        const QPointF content = pageOrigin + QPointF(kInset, kInset);   // the page's content origin on the canvas
        db.updateElementRect(pageId, pageEl->x, pageEl->y, pageEl->w, std::max(CanvasDocument::kMinPageHeight, page.totalHeight() + 2.0 * kInset));

        // (3) px-space margin ink → ink elements beside the text (+ its chips → text shapes).
        const QHash<QString, QString> remaining = db.allInk();
        for (auto it = remaining.constBegin(); it != remaining.constEnd(); ++it) {
            const int row = page.rowForId(it.key());
            const QJsonObject env = parse(it.value());
            if (row < 0 || env.value(QStringLiteral("space")).toString() != QLatin1String("px")) { ++rep.orphanInk; continue; }
            // px anchor: point = (Δx from the PAGE CENTER, Δy from the block's top), page px.
            const QPointF anchor(content.x() + pw / 2.0, content.y() + page.yForRow(row));
            QRectF bbox;
            const QJsonArray world = mapShapes(env.value(QStringLiteral("shapes")).toArray(),
                                               [anchor](QPointF p) { return anchor + p; }, &bbox);
            if (!bbox.isNull() && !world.isEmpty()) {
                db.insertElement(element(QStringLiteral("ink"), bbox, z++, inkJson(rebase(world, bbox.topLeft()))));
                ++rep.textInkElements;
            }
            for (const SketchTextSpec& t : parseSketchTexts(env)) {   // px chips: x/y/w/size in page px
                db.insertElement(textShape(t, pw, anchor, QSizeF(1, 1), z++));
                ++rep.textShapes;
            }
            if (bbox.isNull() && !env.contains(QStringLiteral("texts"))) ++rep.orphanInk;
        }
        page.detachPage();
    }

    // (4) The 1.0 table goes; canvas defaults; the working copy is a 2.0 store from here on.
    db.dropBlockInkTable();
    if (db.canvasMeta(QStringLiteral("defaultView")).isEmpty()) db.setCanvasMeta(QStringLiteral("defaultView"), QStringLiteral("canvas"));
    db.setCanvasMeta(QStringLiteral("pageWidthDefault"), QString::number(static_cast<int>(pw)));
    db.setCanvasMeta(QStringLiteral("convertedFrom"), QStringLiteral("mnd1"));
    db.stampMeta();
    db.commit();
    return true;
}

} // namespace mn
