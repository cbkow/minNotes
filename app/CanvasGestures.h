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
    Q_INVOKABLE void press(QQuickItem* item, qreal x, qreal y, int modifiers = 0);
    Q_INVOKABLE void move(QQuickItem* item, qreal x, qreal y, int modifiers = 0);
    Q_INVOKABLE void release(QQuickItem* item, qreal x, qreal y, int modifiers = 0);
    Q_INVOKABLE void click(QQuickItem* item, qreal x, qreal y, int modifiers = 0);   // Qt.ShiftModifier etc. (the Layers panel's ⇧-click)
    Q_INVOKABLE void doubleClick(QQuickItem* item, qreal x, qreal y, int modifiers = 0);   // Qt.ShiftModifier etc.
    // A drag in `steps` moves (each a real move event with advancing timestamps).
    Q_INVOKABLE void drag(QQuickItem* item, qreal x0, qreal y0, qreal x1, qreal y1, int steps = 8);
    Q_INVOKABLE void dragWith(QQuickItem* item, int button, qreal x0, qreal y0, qreal x1, qreal y1, int steps = 8);   // Qt.MiddleButton etc.
    Q_INVOKABLE void rightClick(QQuickItem* item, qreal x, qreal y);
    // Keys to the focused item: a character per call (text + the key for letters), or a named key.
    Q_INVOKABLE void type(QQuickItem* item, const QString& text);
    Q_INVOKABLE void key(QQuickItem* item, int key, int modifiers = 0);          // a press (and its release, see the .cpp)
    Q_INVOKABLE void keyPress(QQuickItem* item, int key, int modifiers = 0);     // a press alone (hold-to-shuttle)
    Q_INVOKABLE void keyRelease(QQuickItem* item, int key, int modifiers = 0);   // its release
    Q_INVOKABLE void wheel(QQuickItem* item, qreal x, qreal y, int dx, int dy);   // a mouse wheel's angle delta (±120 per notch)
    Q_INVOKABLE qint64 residentBytes() const;   // this process's resident set (the probe's leak check); -1 when unknown
    // The item chain under (x, y) in `item`'s coordinates, top-most leaf first, naming the ones that ACCEPT mouse
    // buttons (the press stops at the first of those) — the probe's "who ate my click" diagnostic.
    Q_INVOKABLE QString itemsUnder(QQuickItem* item, qreal x, qreal y) const;
    Q_INVOKABLE bool grab(QQuickItem* item, const QString& path) const;   // the item's window to a PNG (a probe's mid-run picture)
    Q_INVOKABLE QString focusItemName() const;   // the window's active focus item (class + objectName), for key walks
    Q_INVOKABLE bool envSet(const QString& name) const;
    Q_INVOKABLE QString tempDir() const;   // a writable scratch dir for probe files (the OS temp location)
private:
    void mouse(QQuickItem* item, int type, qreal x, qreal y, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier);
    qint64 nextStamp(qint64 advance);
    qint64 stamp_ = 1000;
};
