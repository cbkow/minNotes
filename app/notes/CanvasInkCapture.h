#pragma once
// The canvas pen (2.0 stage 2 step 7c): a screen-space overlay over the canvas that captures one
// stroke at a time with the Inspector's tool / colour / width — freehand through the ink-stroke
// modeler (the studio's smoothing; compiled into the app only), rect / oval / line / arrow as the
// qcv shape schema (rect = four corners, oval = centre + radii, line and arrow = start + end) — in
// WORLD px through the camera, and commits it as ONE ink element: rect = the stroke's bbox plus
// half the pen, points element-local (strokeCommitted → canvas.addInkElement). The live stroke is
// published as a normalized envelope over this item (liveEnvelope) for a preview InkNodeItem. The
// eraser reports a world point (eraseAt); the host removes the topmost ink element under it.
// begin/move/endStroke are invokable so the probe drives the same path the mouse does.
#include <QColor>
#include <QElapsedTimer>
#include <QPointF>
#include <QQuickItem>
#include <QRectF>
#include <QString>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <vector>

namespace qcv { class InkStrokeModelerWrapper; }

class CanvasInkCapture : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)            // freehand|rect|oval|arrow|line|eraser (else inert)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(qreal strokeWidth READ strokeWidth WRITE setStrokeWidth NOTIFY strokeWidthChanged)   // world px
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY cameraChanged)
    Q_PROPERTY(qreal panX READ panX WRITE setPanX NOTIFY cameraChanged)
    Q_PROPERTY(qreal panY READ panY WRITE setPanY NOTIFY cameraChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)      // takes the mouse when true
    Q_PROPERTY(bool drawing READ drawing NOTIFY drawingChanged)
    Q_PROPERTY(QString liveEnvelope READ liveEnvelope NOTIFY liveChanged)         // the in-progress stroke, normalized over this item
public:
    explicit CanvasInkCapture(QQuickItem* parent = nullptr);
    ~CanvasInkCapture() override;
    QString tool() const { return tool_; }
    void setTool(const QString& t);
    QColor color() const { return color_; }
    void setColor(const QColor& c);
    qreal strokeWidth() const { return width_; }
    void setStrokeWidth(qreal w);
    qreal zoom() const { return zoom_; }
    void setZoom(qreal z);
    qreal panX() const { return panX_; }
    void setPanX(qreal v);
    qreal panY() const { return panY_; }
    void setPanY(qreal v);
    bool active() const { return active_; }
    void setActive(bool a);
    bool drawing() const { return drawing_; }
    QString liveEnvelope() const { return live_; }
    // Item (screen) px. The same path the mouse takes.
    Q_INVOKABLE void beginStroke(qreal x, qreal y);
    Q_INVOKABLE void movePoint(qreal x, qreal y);
    Q_INVOKABLE void endStroke(qreal x, qreal y);
    Q_INVOKABLE void cancelStroke();
    Q_INVOKABLE QPointF toWorld(qreal x, qreal y) const { return QPointF((x - panX_) / zoom_, (y - panY_) / zoom_); }
signals:
    void toolChanged();
    void colorChanged();
    void strokeWidthChanged();
    void cameraChanged();
    void activeChanged();
    void drawingChanged();
    void liveChanged();
    void strokeCommitted(const QString& envelopeJson, const QRectF& worldRect);
    void eraseAt(qreal worldX, qreal worldY);
protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
private:
    bool isStrokeTool() const;
    void rebuildLive();
    std::vector<QPointF> shapePoints() const;   // world px for the current tool
    QString tool_ = QStringLiteral("type");
    QColor color_ = QColor(255, 0, 0);
    qreal width_ = 6;
    qreal zoom_ = 1, panX_ = 0, panY_ = 0;
    bool active_ = false;
    bool drawing_ = false;
    bool modeled_ = false;
    QString live_;
    QPointF press_, current_;            // screen px
    std::vector<QPointF> freehand_;      // world px (smoothed when the modeler runs)
    QElapsedTimer clock_;
#if MN_CANVAS_STROKE_MODELER
    std::unique_ptr<qcv::InkStrokeModelerWrapper> modeler_;   // the app only; the tests take the raw-point path
#endif
};
