#pragma once
// Canvas B3: the canvas view's camera — pan/zoom with a change counter QML bindings can key on.
// User input (wheel, drag, pinch, zoom keys) goes through the user* paths and flips `userCam`, which
// ends the auto-fit a fresh canvas does on show and resize (the sketch stage's rule). fitRect and
// setCamera (view-state restore) leave userCam alone. All maths in mn::cam (tested).
#include "../core/CanvasCameraMath.h"
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QtQml/qqmlregistration.h>

class CanvasCamera : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal panX READ panX NOTIFY cameraChanged)
    Q_PROPERTY(qreal panY READ panY NOTIFY cameraChanged)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY cameraChanged)
    Q_PROPERTY(int rev READ rev NOTIFY cameraChanged)
    Q_PROPERTY(bool userCam READ userCam WRITE setUserCam NOTIFY userCamChanged)
public:
    explicit CanvasCamera(QObject* parent = nullptr) : QObject(parent) {}
    qreal panX() const { return s_.panX; }
    qreal panY() const { return s_.panY; }
    qreal zoom() const { return s_.zoom; }
    int rev() const { return rev_; }
    bool userCam() const { return userCam_; }
    void setUserCam(bool v);

    Q_INVOKABLE void zoomAboutPoint(qreal z, qreal ax, qreal ay);           // user: the point under (ax, ay) stays put
    Q_INVOKABLE void scrollBy(qreal dx, qreal dy);                          // user: the view scrolls by (dx, dy) screen px (a Flickable's contentX/Y += d)
    Q_INVOKABLE void setPan(qreal x, qreal y);                              // user: a drag
    Q_INVOKABLE void fitRect(const QRectF& world, qreal vw, qreal vh, qreal pad);   // frame a world rect (not user input)
    Q_INVOKABLE void setCamera(qreal x, qreal y, qreal z, bool user);       // view-state restore
    Q_INVOKABLE QPointF screenToWorld(qreal x, qreal y) const { return mn::cam::screenToWorld(s_, x, y); }
    Q_INVOKABLE QPointF worldToScreen(qreal x, qreal y) const { return mn::cam::worldToScreen(s_, x, y); }
signals:
    void cameraChanged();
    void userCamChanged();
private:
    void apply(const mn::cam::State& s, bool user);
    mn::cam::State s_;
    int rev_ = 0;
    bool userCam_ = false;
};
