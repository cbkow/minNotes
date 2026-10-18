#include "CanvasGestures.h"
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>

qint64 CanvasGestures::nextStamp(qint64 advance) { stamp_ += advance; return stamp_; }

void CanvasGestures::mouse(QQuickItem* item, int type, qreal x, qreal y, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods) {
    if (!item || !item->window()) return;
    QQuickWindow* w = item->window();
    const QPointF scene = item->mapToScene(QPointF(x, y));
    const QPointF global = w->mapToGlobal(scene.toPoint());
    QMouseEvent ev(static_cast<QEvent::Type>(type), scene, scene, global, button, buttons, mods);
    ev.setTimestamp(static_cast<quint64>(nextStamp(type == QEvent::MouseMove ? 16 : 40)));
    QCoreApplication::sendEvent(w, &ev);
}

void CanvasGestures::press(QQuickItem* item, qreal x, qreal y)   { mouse(item, QEvent::MouseButtonPress, x, y, Qt::LeftButton, Qt::LeftButton); }
void CanvasGestures::move(QQuickItem* item, qreal x, qreal y)    { mouse(item, QEvent::MouseMove, x, y, Qt::NoButton, Qt::LeftButton); }
void CanvasGestures::release(QQuickItem* item, qreal x, qreal y) { mouse(item, QEvent::MouseButtonRelease, x, y, Qt::LeftButton, Qt::NoButton); }

void CanvasGestures::click(QQuickItem* item, qreal x, qreal y) {
    nextStamp(1000);   // never within a double-click window of the previous tap
    press(item, x, y);
    release(item, x, y);
}

void CanvasGestures::doubleClick(QQuickItem* item, qreal x, qreal y, int modifiers) {
    nextStamp(1000);
    const Qt::KeyboardModifiers m = Qt::KeyboardModifiers(modifiers);
    for (int i = 0; i < 2; ++i) {   // two taps inside the double-click interval: TapHandler counts them
        mouse(item, QEvent::MouseButtonPress, x, y, Qt::LeftButton, Qt::LeftButton, m);
        mouse(item, QEvent::MouseButtonRelease, x, y, Qt::LeftButton, Qt::NoButton, m);
    }
}

void CanvasGestures::drag(QQuickItem* item, qreal x0, qreal y0, qreal x1, qreal y1, int steps) {
    dragWith(item, int(Qt::LeftButton), x0, y0, x1, y1, steps);
}

void CanvasGestures::dragWith(QQuickItem* item, int button, qreal x0, qreal y0, qreal x1, qreal y1, int steps) {
    const Qt::MouseButton b = Qt::MouseButton(button);
    nextStamp(1000);
    mouse(item, QEvent::MouseButtonPress, x0, y0, b, b);
    const int n = std::max(1, steps);
    for (int i = 1; i <= n; ++i) mouse(item, QEvent::MouseMove, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, Qt::NoButton, b);
    mouse(item, QEvent::MouseButtonRelease, x1, y1, b, Qt::NoButton);
}

void CanvasGestures::rightClick(QQuickItem* item, qreal x, qreal y) {
    nextStamp(1000);
    mouse(item, QEvent::MouseButtonPress, x, y, Qt::RightButton, Qt::RightButton);
    mouse(item, QEvent::MouseButtonRelease, x, y, Qt::RightButton, Qt::NoButton);
}

void CanvasGestures::type(QQuickItem* item, const QString& text) {
    if (!item || !item->window()) return;
    for (const QChar ch : text) {
        const int k = ch.isLetter() ? int(Qt::Key_A) + (ch.toUpper().unicode() - 'A') : ch.unicode();
        QKeyEvent p(QEvent::KeyPress, k, Qt::NoModifier, QString(ch));
        QKeyEvent r(QEvent::KeyRelease, k, Qt::NoModifier, QString(ch));
        QCoreApplication::sendEvent(item->window(), &p);
        QCoreApplication::sendEvent(item->window(), &r);
    }
}

void CanvasGestures::key(QQuickItem* item, int key, int modifiers) {
    if (!item || !item->window()) return;
    QKeyEvent p(QEvent::KeyPress, key, Qt::KeyboardModifiers(modifiers));
    QKeyEvent r(QEvent::KeyRelease, key, Qt::KeyboardModifiers(modifiers));
    QCoreApplication::sendEvent(item->window(), &p);
    QCoreApplication::sendEvent(item->window(), &r);
}
