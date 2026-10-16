#pragma once
// Canvas B5: strokes → GPU geometry. Every stroke becomes a union of CAPSULES (a segment with
// round caps), one quad per segment carrying the segment's endpoints and half width; the fragment
// shader evaluates the capsule's signed distance and feathers one screen pixel — which is exactly
// what qcv::paintStroke's round-cap/round-join pen draws, so page (QPainter), export and canvas
// match. Filled rects/ovals/arrow heads are solid triangles (a == b == pos, a huge half width →
// always inside) with a one-px capsule outline for the anti-aliased edge. Pure: points in, verts out.
#include "active_stroke.h"
#include <QRectF>
#include <QSizeF>
#include <vector>

namespace mn::ink {

struct Vertex {                       // 32 bytes; matches InkNodeItem's attribute set and ink.vert
    float x, y;                       // the quad corner (local px)
    float ax, ay, bx, by;             // the capsule's segment
    float halfW;                      // the capsule's half width (px)
    unsigned char r, g, b, a;         // straight (un-premultiplied) sRGB
};

struct Mesh {
    std::vector<Vertex> verts;        // triangles, 3 verts each
    QRectF bounds;                    // of every emitted corner
    bool empty() const { return verts.empty(); }
};

// Append stroke `s` to `out`. `scale` maps the stroke's local units to px (a px anchor is 1 × 1, a
// media anchor its display frame); the pen is strokeWidth × widthScale px (floor 1, as the painter);
// `margin` px of quad beyond the half width gives the feather room (≥ one screen px in local units).
void tessellateStroke(const qcv::ActiveStroke& s, QSizeF scale, double widthScale, double margin, Mesh& out);

} // namespace mn::ink
