// sketch_text — the shared layout/paint core for sketch TEXT elements.
//
// A text box persists in the sketch descriptor's `texts[]` array as
// {x, y, w, text, size, color}: x/y/w normalized to the frame, `size` = font
// px in SOURCE px (the stroke-width rule: marks are canvas-absolute). Height
// is NEVER stored — always derived here from (text, width, size), so nothing
// can desync between the editor overlay, the canvas paint, fit-to-ink, and
// the export raster.
//
// Layout happens in SOURCE space (font at `size` px, width = w×srcW px) and
// the painter scales the result — wrap points are therefore identical at any
// zoom, in the inline embed, and in exports, by construction. QTextDocument
// is the engine on purpose: it is what backs QML TextEdit, so the editing
// overlay wraps where the committed paint wraps.
//
// Consumers: SketchCanvas::paint (ghost + clipped passes), Exporter's
// renderSketch, and BlockModel's fit-to-ink bounds.

#pragma once

#include <QColor>
#include <QJsonObject>
#include <QRectF>
#include <QString>

#include <vector>

class QPainter;

namespace mn {

struct SketchTextSpec {
    QString text;
    double  x = 0, y = 0, w = 0;   // normalized to the frame
    double  size = 16;             // font px in SOURCE px
    QColor  color;
    QString family;                // resolved family (caller supplies)
};

// The app font for contexts with no QML binding (Exporter, BlockModel): a
// cached QFontDatabase probe — "Inter 18pt" → "Inter" → the system font.
// SketchCanvas does NOT use this; it takes the QML-resolved family by
// property so canvas and overlay can never disagree.
QString sketchTextFamily();

// A text box renders as a filled CHIP: `color` is the box fill (the draw
// color at creation) and the glyphs auto-pick black/white for contrast —
// labels stay legible over screenshots and ink. Padding is DERIVED from the
// font size (never stored, like height): text lays out at w − 2·pad.
QColor sketchTextInkColor(const QColor& bg);        // black or white, by luminance
double sketchTextPadSrc(const SketchTextSpec& t);   // chip padding, source px

// The descriptor's `texts[]` array → specs (family left empty).
std::vector<SketchTextSpec> parseSketchTexts(const QJsonObject& root);

// One spec → its element JSON (x/y/w/text/size/color — family is a render
// concern, never serialized). The inverse of the parser, kept beside it so
// the sketch descriptor and the doc-ink envelope can never diverge on the
// element schema.
QJsonObject sketchTextToJson(const SketchTextSpec& t);

// Derived height of the wrapped text in SOURCE px.
double sketchTextHeightSrc(const SketchTextSpec& t, double srcW);

// The element's full rect in SOURCE px (position + derived height).
QRectF sketchTextRectSrc(const SketchTextSpec& t, double srcW, double srcH);

// Paint at `scale` (screen px per source px): the painter is translated to
// the element's scaled origin and scaled, then the source-space layout draws.
void paintSketchText(QPainter& p, const SketchTextSpec& t,
                     double srcW, double srcH, double scale);

} // namespace mn
