#include "CanvasCamera.h"

void CanvasCamera::setUserCam(bool v) { if (v == userCam_) return; userCam_ = v; emit userCamChanged(); }

void CanvasCamera::apply(const mn::cam::State& s, bool user) {
    if (user) setUserCam(true);
    if (s.panX == s_.panX && s.panY == s_.panY && s.zoom == s_.zoom) return;
    s_ = s;
    ++rev_;
    emit cameraChanged();
}

void CanvasCamera::zoomAboutPoint(qreal z, qreal ax, qreal ay) { apply(mn::cam::zoomAbout(s_, z, ax, ay), true); }
void CanvasCamera::scrollBy(qreal dx, qreal dy) { mn::cam::State s = s_; s.panX -= dx; s.panY -= dy; apply(s, true); }
void CanvasCamera::setPan(qreal x, qreal y) { mn::cam::State s = s_; s.panX = x; s.panY = y; apply(s, true); }
void CanvasCamera::fitRect(const QRectF& world, qreal vw, qreal vh, qreal pad) { apply(mn::cam::fitRect(world, vw, vh, pad), false); }
void CanvasCamera::setCamera(qreal x, qreal y, qreal z, bool user) {
    mn::cam::State s; s.panX = x; s.panY = y; s.zoom = mn::cam::clampZoom(z);
    apply(s, false);
    setUserCam(user);
}
