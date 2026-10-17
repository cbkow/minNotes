#pragma once
// Dev-only (2.0 canvas probe): synthetic pointer + key gestures posted into the window as REAL
// QMouseEvent / QKeyEvent — so a probe exercises the same handler chain the mouse does (tap /
// drag / double-tap handlers, the pen overlay, the text editor), not just the functions behind
// them. Coordinates are LOCAL to the item passed; events are sent synchronously (sendEvent), so
// the probe can check state right after a call. Exposed to QML as the `gestures` context property.
#include <QObject>
#include <QPointF>
#include <QQuickItem>

class CanvasGestures : public QObject {
    Q_OBJECT
public:
    explicit CanvasGestures(QObject* parent = nullptr) : QObject(parent) {}
    Q_INVOKABLE void press(QQuickItem* item, qreal x, qreal y);
    Q_INVOKABLE void move(QQuickItem* item, qreal x, qreal y);
    Q_INVOKABLE void release(QQuickItem* item, qreal x, qreal y);
    Q_INVOKABLE void click(QQuickItem* item, qreal x, qreal y);
    Q_INVOKABLE void doubleClick(QQuickItem* item, qreal x, qreal y);
    // A drag in `steps` moves (each a real move event with advancing timestamps).
    Q_INVOKABLE void drag(QQuickItem* item, qreal x0, qreal y0, qreal x1, qreal y1, int steps = 8);
    // Keys to the focused item: a character per call (text + the key for letters), or a named key.
    Q_INVOKABLE void type(QQuickItem* item, const QString& text);
    Q_INVOKABLE void key(QQuickItem* item, int key, int modifiers = 0);
private:
    void mouse(QQuickItem* item, int type, qreal x, qreal y, Qt::MouseButton button, Qt::MouseButtons buttons);
    qint64 nextStamp(qint64 advance);
    qint64 stamp_ = 1000;
};
