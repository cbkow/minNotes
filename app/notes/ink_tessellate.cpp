#include "ink_tessellate.h"
#include <QColor>
#include <algorithm>
#include <cmath>

namespace mn::ink {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kOvalSegments = 64;
constexpr float kSolidHalfW = 1.0e6f;     // a solid vertex: a == b == pos, so d = −halfW → coverage 1

struct Pen { unsigned char r, g, b, a; };

void grow(Mesh& m, float x, float y) {   // explicit min/max: a zero-size QRectF reads as null, so united() would drop it
    if (m.verts.size() == 1) { m.bounds.setCoords(x, y, x, y); return; }
    m.bounds.setCoords(std::min<qreal>(m.bounds.left(), x), std::min<qreal>(m.bounds.top(), y),
                       std::max<qreal>(m.bounds.right(), x), std::max<qreal>(m.bounds.bottom(), y));
}

void vert(Mesh& m, float x, float y, float ax, float ay, float bx, float by, float hw, Pen c) {
    m.verts.push_back(Vertex{ x, y, ax, ay, bx, by, hw, c.r, c.g, c.b, c.a });
    grow(m, x, y);
}

// One capsule a→b (a == b is a dot). The quad extends r = hw + margin beyond the segment on every
// side, so the feather has room and the round caps are inside it.
void capsule(Mesh& m, QPointF a, QPointF b, double hw, double margin, Pen c) {
    const double r = hw + margin;
    double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double len = std::hypot(dx, dy);
    if (len > 1e-9) { dx /= len; dy /= len; } else { dx = 1.0; dy = 0.0; }
    const double nx = -dy, ny = dx;
    const QPointF p0(a.x() - dx * r + nx * r, a.y() - dy * r + ny * r);
    const QPointF p1(a.x() - dx * r - nx * r, a.y() - dy * r - ny * r);
    const QPointF p2(b.x() + dx * r - nx * r, b.y() + dy * r - ny * r);
    const QPointF p3(b.x() + dx * r + nx * r, b.y() + dy * r + ny * r);
    const float ax = float(a.x()), ay = float(a.y()), bx = float(b.x()), by = float(b.y()), h = float(hw);
    const QPointF tri[6] = { p0, p1, p2, p0, p2, p3 };
    for (const QPointF& p : tri) vert(m, float(p.x()), float(p.y()), ax, ay, bx, by, h, c);
}

void solidTri(Mesh& m, QPointF p0, QPointF p1, QPointF p2, Pen c) {
    for (const QPointF& p : { p0, p1, p2 })
        vert(m, float(p.x()), float(p.y()), float(p.x()), float(p.y()), float(p.x()), float(p.y()), kSolidHalfW, c);
}

void polyline(Mesh& m, const std::vector<QPointF>& pts, bool closed, double hw, double margin, Pen c) {
    if (pts.size() == 1) { capsule(m, pts[0], pts[0], hw, margin, c); return; }
    for (size_t i = 1; i < pts.size(); ++i)
        if (pts[i] != pts[i - 1]) capsule(m, pts[i - 1], pts[i], hw, margin, c);
    if (closed && pts.size() > 2 && pts.back() != pts.front()) capsule(m, pts.back(), pts.front(), hw, margin, c);
}

} // namespace

void tessellateStroke(const qcv::ActiveStroke& s, QSizeF scale, double widthScale, double margin, Mesh& out) {
    using qcv::DrawingTool;
    if (s.points.empty()) return;
    const Pen pen{ static_cast<unsigned char>(s.color.red()), static_cast<unsigned char>(s.color.green()),
                   static_cast<unsigned char>(s.color.blue()), static_cast<unsigned char>(s.color.alpha()) };
    const double penW = std::max(1.0, double(s.strokeWidth) * widthScale);   // == paintStroke
    const double hw = penW * 0.5;
    auto px = [&](QPointF p) { return QPointF(p.x() * scale.width(), p.y() * scale.height()); };
    const double outlineHw = 0.5;    // the one-px anti-aliased edge of a solid shape

    switch (s.tool) {
    case DrawingTool::Freehand: {
        std::vector<QPointF> pts; pts.reserve(s.points.size());
        for (const QPointF& p : s.points) pts.push_back(px(p));
        polyline(out, pts, false, hw, margin, pen);
        break;
    }
    case DrawingTool::Line: {
        if (s.points.size() < 2) break;
        capsule(out, px(s.points[0]), px(s.points[1]), hw, margin, pen);
        break;
    }
    case DrawingTool::Rectangle: {
        if (s.points.size() < 4) break;
        const QRectF r = QRectF(px(s.points[0]), px(s.points[2])).normalized();
        const std::vector<QPointF> corners{ r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft() };
        if (s.filled) {
            solidTri(out, corners[0], corners[1], corners[2], pen);
            solidTri(out, corners[0], corners[2], corners[3], pen);
            polyline(out, corners, true, outlineHw, margin, pen);
        } else {
            polyline(out, corners, true, hw, margin, pen);
        }
        break;
    }
    case DrawingTool::Oval: {
        if (s.points.size() < 2) break;
        const QPointF c = px(s.points[0]);
        const double rx = s.points[1].x() * scale.width(), ry = s.points[1].y() * scale.height();   // a VECTOR: scale only
        std::vector<QPointF> ring; ring.reserve(kOvalSegments);
        for (int i = 0; i < kOvalSegments; ++i) {
            const double t = 2.0 * kPi * i / kOvalSegments;
            ring.emplace_back(c.x() + rx * std::cos(t), c.y() + ry * std::sin(t));
        }
        if (s.filled) {
            for (int i = 0; i < kOvalSegments; ++i) solidTri(out, c, ring[size_t(i)], ring[size_t((i + 1) % kOvalSegments)], pen);
            polyline(out, ring, true, outlineHw, margin, pen);
        } else {
            polyline(out, ring, true, hw, margin, pen);
        }
        break;
    }
    case DrawingTool::Arrow: {
        if (s.points.size() < 2) break;
        const QPointF start = px(s.points[0]), end = px(s.points[1]);
        double dirX = end.x() - start.x(), dirY = end.y() - start.y();
        const double length = std::hypot(dirX, dirY);
        if (length < 0.001) break;
        dirX /= length; dirY /= length;
        // paintStroke's head: 25°, size leaning on the width (a thin line gets a modest head).
        const double arrowSize = 4.5 * widthScale + penW * 2.3;
        const double cosA = std::cos(0.436332), sinA = std::sin(0.436332);
        const QPointF lineEnd(end.x() - dirX * arrowSize * cosA, end.y() - dirY * arrowSize * cosA);
        capsule(out, start, lineEnd, hw, margin, pen);
        const QPointF a1(end.x() - arrowSize * (dirX * cosA - dirY * sinA), end.y() - arrowSize * (dirY * cosA + dirX * sinA));
        const QPointF a2(end.x() - arrowSize * (dirX * cosA + dirY * sinA), end.y() - arrowSize * (dirY * cosA - dirX * sinA));
        solidTri(out, end, a1, a2, pen);
        polyline(out, { end, a1, a2 }, true, outlineHw, margin, pen);
        break;
    }
    case DrawingTool::None:
    case DrawingTool::Eraser:
    default:
        break;
    }
}

} // namespace mn::ink
