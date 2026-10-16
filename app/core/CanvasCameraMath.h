#pragma once
// Canvas B3: the camera's maths, header-only and Qt-Core-only so the regression suite can pin it.
// screen = world · zoom + pan. The QML-facing CanvasCamera (app/notes) is a thin QObject over this.
#include <QPointF>
#include <QRectF>
#include <algorithm>

namespace mn::cam {

constexpr double kMinZoom = 0.05;
constexpr double kMaxZoom = 4.0;

struct State { double panX = 0.0, panY = 0.0, zoom = 1.0; };

inline double clampZoom(double z) { return std::clamp(z, kMinZoom, kMaxZoom); }

// The world point under the screen anchor (ax, ay) stays under it: pan' = a − (a − pan)·z'/z.
inline State zoomAbout(State s, double newZoom, double ax, double ay) {
    newZoom = clampZoom(newZoom);
    const double r = newZoom / s.zoom;
    s.panX = ax - (ax - s.panX) * r;
    s.panY = ay - (ay - s.panY) * r;
    s.zoom = newZoom;
    return s;
}

// Frame `world` centred in a vw × vh viewport with `pad` screen px of margin; zoom clamped.
inline State fitRect(const QRectF& world, double vw, double vh, double pad) {
    State s;
    if (world.width() <= 0.0 || world.height() <= 0.0 || vw <= 0.0 || vh <= 0.0) return s;
    s.zoom = clampZoom(std::min((vw - 2.0 * pad) / world.width(), (vh - 2.0 * pad) / world.height()));
    s.panX = (vw - world.width() * s.zoom) / 2.0 - world.x() * s.zoom;
    s.panY = (vh - world.height() * s.zoom) / 2.0 - world.y() * s.zoom;
    return s;
}

inline QPointF screenToWorld(const State& s, double x, double y) { return { (x - s.panX) / s.zoom, (y - s.panY) / s.zoom }; }
inline QPointF worldToScreen(const State& s, double x, double y) { return { x * s.zoom + s.panX, y * s.zoom + s.panY }; }

} // namespace mn::cam
