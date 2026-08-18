#include "sketch_text.h"

#include <QAbstractTextDocumentLayout>
#include <QFont>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonValue>
#include <QPainter>
#include <QPalette>
#include <QTextDocument>
#include <QTextOption>

#include <cmath>

namespace mn {

namespace {

// One configured document per call: geometry never runs per-frame (callers
// cache derived heights), so simplicity beats a doc cache here. Text lays
// out inside the chip: width = box width − 2·pad.
void configureDoc(QTextDocument& doc, const SketchTextSpec& t, double srcW)
{
    QFont f(t.family.isEmpty() ? sketchTextFamily() : t.family);
    f.setPixelSize(qMax(1, int(std::lround(t.size))));
    doc.setDefaultFont(f);
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc.setDefaultTextOption(opt);
    doc.setDocumentMargin(0);
    doc.setPlainText(t.text);
    doc.setTextWidth(qMax(1.0, t.w * srcW - 2.0 * sketchTextPadSrc(t)));
}

} // namespace

QColor sketchTextInkColor(const QColor& bg)
{
    // BT.601 luma on the fill; bright chips get black glyphs, dark get white.
    const double luma = 0.299 * bg.redF() + 0.587 * bg.greenF() + 0.114 * bg.blueF();
    return luma > 0.55 ? QColor(0, 0, 0) : QColor(255, 255, 255);
}

double sketchTextPadSrc(const SketchTextSpec& t)
{
    return t.size * 0.4;   // derived, never stored — the height philosophy
}

QString sketchTextFamily()
{
    static const QString family = [] {
        const QStringList all = QFontDatabase::families();
        for (const char* want : { "Inter 18pt", "Inter" })
            if (all.contains(QLatin1String(want)))
                return QString::fromLatin1(want);
        return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    }();
    return family;
}

std::vector<SketchTextSpec> parseSketchTexts(const QJsonObject& root)
{
    std::vector<SketchTextSpec> out;
    const QJsonArray arr = root.value(QStringLiteral("texts")).toArray();
    out.reserve(size_t(arr.size()));
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        SketchTextSpec t;
        t.text = o.value(QStringLiteral("text")).toString();
        t.x    = o.value(QStringLiteral("x")).toDouble();
        t.y    = o.value(QStringLiteral("y")).toDouble();
        t.w    = o.value(QStringLiteral("w")).toDouble();
        t.size = o.value(QStringLiteral("size")).toDouble(16);
        t.color = QColor(o.value(QStringLiteral("color")).toString(
            QStringLiteral("#E4E3E2")));
        out.push_back(std::move(t));
    }
    return out;
}

double sketchTextHeightSrc(const SketchTextSpec& t, double srcW)
{
    if (t.text.isEmpty() || t.w <= 0 || srcW <= 0) return 0.0;
    QTextDocument doc;
    configureDoc(doc, t, srcW);
    return doc.size().height() + 2.0 * sketchTextPadSrc(t);   // chip height
}

QRectF sketchTextRectSrc(const SketchTextSpec& t, double srcW, double srcH)
{
    return QRectF(t.x * srcW, t.y * srcH, t.w * srcW,
                  sketchTextHeightSrc(t, srcW));
}

void paintSketchText(QPainter& p, const SketchTextSpec& t,
                     double srcW, double srcH, double scale)
{
    if (t.text.isEmpty() || t.w <= 0 || srcW <= 0 || scale <= 0) return;
    QTextDocument doc;
    configureDoc(doc, t, srcW);
    const double pad = sketchTextPadSrc(t);
    const double boxH = doc.size().height() + 2.0 * pad;

    p.save();
    p.translate(t.x * srcW * scale, t.y * srcH * scale);
    p.scale(scale, scale);
    p.fillRect(QRectF(0, 0, t.w * srcW, boxH), t.color);   // the chip (squared)
    p.translate(pad, pad);
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, sketchTextInkColor(t.color));
    doc.documentLayout()->draw(&p, ctx);
    p.restore();
}

} // namespace mn
