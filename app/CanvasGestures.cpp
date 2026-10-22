#include "CanvasGestures.h"
#include <QCoreApplication>
#include <algorithm>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QQuickWindow>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QQuickItem>
#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#endif

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

void CanvasGestures::press(QQuickItem* item, qreal x, qreal y, int modifiers)   { mouse(item, QEvent::MouseButtonPress, x, y, Qt::LeftButton, Qt::LeftButton, Qt::KeyboardModifiers(modifiers)); }
void CanvasGestures::move(QQuickItem* item, qreal x, qreal y, int modifiers)    { mouse(item, QEvent::MouseMove, x, y, Qt::NoButton, Qt::LeftButton, Qt::KeyboardModifiers(modifiers)); }
void CanvasGestures::release(QQuickItem* item, qreal x, qreal y, int modifiers) { mouse(item, QEvent::MouseButtonRelease, x, y, Qt::LeftButton, Qt::NoButton, Qt::KeyboardModifiers(modifiers)); }

void CanvasGestures::click(QQuickItem* item, qreal x, qreal y, int modifiers) {
    nextStamp(1000);   // never within a double-click window of the previous tap
    const Qt::KeyboardModifiers m = Qt::KeyboardModifiers(modifiers);
    mouse(item, QEvent::MouseButtonPress, x, y, Qt::LeftButton, Qt::LeftButton, m);
    mouse(item, QEvent::MouseButtonRelease, x, y, Qt::LeftButton, Qt::NoButton, m);
}

bool CanvasGestures::grab(QQuickItem* item, const QString& path) const {
    if (!item || !item->window()) return false;
    return item->window()->grabWindow().save(path);
}

void CanvasGestures::doubleClick(QQuickItem* item, qreal x, qreal y, int modifiers) {
    nextStamp(1000);
    const Qt::KeyboardModifiers m = Qt::KeyboardModifiers(modifiers);
    for (int i = 0; i < 2; ++i) {   // two taps inside the double-click interval: TapHandler counts them
        mouse(item, QEvent::MouseButtonPress, x, y, Qt::LeftButton, Qt::LeftButton, m);
        mouse(item, QEvent::MouseButtonRelease, x, y, Qt::LeftButton, Qt::NoButton, m);
    }
}

void CanvasGestures::wheel(QQuickItem* item, qreal x, qreal y, int dx, int dy) {
    if (!item || !item->window()) return;
    QQuickWindow* w = item->window();
    const QPointF scene = item->mapToScene(QPointF(x, y));
    const QPointF global = w->mapToGlobal(scene.toPoint());
    QWheelEvent ev(scene, global, QPoint(), QPoint(dx, dy), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    ev.setTimestamp(static_cast<quint64>(nextStamp(40)));
    QCoreApplication::sendEvent(w, &ev);
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

void CanvasGestures::keyPress(QQuickItem* item, int key, int modifiers) {
    if (!item || !item->window()) return;
    QKeyEvent p(QEvent::KeyPress, key, Qt::KeyboardModifiers(modifiers));
    QCoreApplication::sendEvent(item->window(), &p);
}
void CanvasGestures::keyRelease(QQuickItem* item, int key, int modifiers) {
    if (!item || !item->window()) return;
    QKeyEvent r(QEvent::KeyRelease, key, Qt::KeyboardModifiers(modifiers));
    QCoreApplication::sendEvent(item->window(), &r);
}

static void collectUnder(QQuickItem* item, const QPointF& p, int depth, QStringList& out) {
    if (!item || depth > 60) return;
    auto kids = item->childItems();                     // sibling order, then z: the paint order (bottom → top)
    std::stable_sort(kids.begin(), kids.end(), [](QQuickItem* a, QQuickItem* b) { return a->z() < b->z(); });
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {   // top-most first, EVERY branch (the delivery agent's walk)
        QQuickItem* c = *it;
        if (!c->isVisible() || !c->isEnabled()) continue;
        const QPointF q = item->mapToItem(c, p);
        const bool inside = c->contains(q);
        if (!inside && c->clip()) continue;
        if (inside && c->acceptedMouseButtons() != Qt::NoButton) {
            const QString name = QString::fromLatin1(c->metaObject()->className()).section(QLatin1Char('_'), 0, 0);
            out << QStringLiteral("%1%2[%3]").arg(name, c->objectName().isEmpty() ? QString() : QLatin1Char('#') + c->objectName()).arg(int(c->acceptedMouseButtons()));
        }
        collectUnder(c, q, depth + 1, out);
    }
}
QString CanvasGestures::itemsUnder(QQuickItem* item, qreal x, qreal y) const {
    QStringList out;
    collectUnder(item, QPointF(x, y), 0, out);
    return out.join(QStringLiteral(" > "));
}

qint64 CanvasGestures::residentBytes() const {
#if defined(Q_OS_MACOS)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<qint64>(info.resident_size);
#endif
    return -1;
}

QString CanvasGestures::focusItemName() const {
    QObject* o = QGuiApplication::focusObject();
    if (!o) return QStringLiteral("(none)");
    QString n = QString::fromLatin1(o->metaObject()->className());
    if (!o->objectName().isEmpty()) n += QLatin1Char('#') + o->objectName();
    if (auto* it = qobject_cast<QQuickItem*>(o)) n += it->isVisible() ? QStringLiteral(" visible") : QStringLiteral(" HIDDEN");
    return n;
}
bool CanvasGestures::envSet(const QString& name) const {
    const QByteArray v = qgetenv(name.toLatin1());
    return !v.isEmpty() && v != "0";
}

QString CanvasGestures::tempDir() const { return QStandardPaths::writableLocation(QStandardPaths::TempLocation); }
