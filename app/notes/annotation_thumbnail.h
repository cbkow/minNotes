// annotation_thumbnail — CPU recomposite of a note's display thumbnail.
//
// Notes store a durable, square (raw storage-resolution) "clean" PNG of the
// frame plus their strokes normalized [0,1] in notes.json. Both are pixel-
// aspect-independent. The displayed / exported thumbnail is DERIVED from
// those: un-squeeze the clean frame to the clip's effective (pixel-aspect-
// corrected) dimensions, then draw the strokes on top at uniform thickness.
//
// Deriving (rather than baking a fixed annotated capture) means a later
// change to the clip's pixel aspect just re-derives — nothing goes stale —
// and stroke line-weight stays uniform instead of distorting with the
// horizontal un-squeeze (the trap that stretching a baked PNG would hit).
//
// Pure CPU (QPainter); reused by the live regen path and the notes exporter.
//
// minNotes adaptation (2026-06-11): paintStroke() is exposed (QCView keeps
// it file-local) with a lineWidthScale param — the studio's live overlay
// (VideoAnnotator, a QQuickPaintedItem) renders the SAME geometry at stage
// scale instead of source-pixel scale. Thumbnail callers pass 1.0 →
// byte-identical output to QCView's.

#pragma once

#include "active_stroke.h"

#include <QImage>
#include <QRectF>

#include <vector>

class QPainter;

namespace qcv {

// Normalized [0,1] bounding box of a stroke. Handles the shape encodings —
// an Oval stores {center, radii}, not two corners — so callers (hit-testing,
// selection) get the TRUE visual bounds, not the bbox of the raw points.
QRectF strokeBoundsNorm(const ActiveStroke &s);

// Draw one stroke (normalized [0,1] coords) onto a w×h pixel target.
// strokeWidth is stored in SOURCE-pixel units; lineWidthScale maps it to
// the target (target_width / source_width — 1.0 when target IS source-res).
void paintStroke(QPainter &p, const ActiveStroke &s, double w, double h,
                 double lineWidthScale = 1.0);

// Recomposite a note's display thumbnail.
//   cleanSquare : the durable square clean frame (raw storage pixels).
//   strokes     : the note's strokes (normalized [0,1] coords).
//   parNum/parDen : the clip's pixel aspect (1/1 = square → identity).
// Returns a null QImage if cleanSquare is null/empty. The result is in the
// effective (un-squeezed) display aspect with strokes drawn at uniform
// thickness, matching the live viewport.
QImage renderNoteThumbnail(const QImage &cleanSquare,
                           const std::vector<ActiveStroke> &strokes,
                           int parNum, int parDen);

} // namespace qcv
