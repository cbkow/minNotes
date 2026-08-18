// Block-pinned margin ink: the serialized envelope for ONE anchor block's
// strokes (the blob BlockModel stores opaquely in the block_ink table).
//
//   { "version": "2.0",
//     "coordinate_system": "block-local",
//     "space": "px" | "frame",
//     "shapes": [ <qcv shape objects, verbatim strokeToJson> ] }
//
// Coordinate spaces (PLAN-document-annotations.md, ratified 2026-06-10):
//   "px"    — text-ish anchors: point = (Δx from PAGE CENTER, Δy from block
//             top), unclamped page pixels; stroke_width in page px.
//   "frame" — media anchors: point = fraction of the media DISPLAY frame
//             (values outside [0,1] = margin overshoot; ink scales with the
//             media resize); stroke_width in media-INTRINSIC px (the sketch
//             sourceWidth convention — render scale = frameW / mediaW).
//
// This is a SIBLING of qcv::AnnotationSerializer's normalized envelope —
// that reader deliberately rejects coordinate_system != "normalized", and
// its painter/hit-tests assume [0,1]. Per-shape JSON is shared verbatim via
// the public strokeToJson/jsonToStroke, so tools/colors/widths stay format-
// compatible with the sketch/video stroke schema.
#pragma once

#include "active_stroke.h"
#include "sketch_text.h"

#include <QString>
#include <vector>

namespace mn {

struct DocInkAnchor {
    enum Space { Px, Frame };
    Space space = Px;
    std::vector<qcv::ActiveStroke> strokes;
    // Text chips (`texts[]` in the envelope, the sketch element schema).
    // Units follow the anchor's space: px anchors store x/y/w/size in page
    // px (x = Δ from page CENTER, like stroke points); frame anchors store
    // x/y/w frame-normalized with size in media-INTRINSIC px (the
    // stroke_width convention). A TYPED member — the writer rebuilds the
    // envelope from this struct, so a mere pass-through key would be
    // destroyed on every stroke commit.
    std::vector<mn::SketchTextSpec> texts;
};

// Serialize an anchor's strokes ("" when empty — the delete-the-row signal
// for BlockModel::setBlockInk).
QString docInkToJson(const DocInkAnchor& anchor);

// Parse a blob; returns false (and leaves `out` empty) on wrong version /
// coordinate_system, or malformed JSON. An empty/null string parses to an
// empty anchor and returns true.
bool docInkFromJson(const QString& json, DocInkAnchor& out);

bool docInkHasStrokes(const QString& json);
// Strokes OR text chips (the ink-presence gate once texts exist).
bool docInkHasContent(const QString& json);

} // namespace mn
