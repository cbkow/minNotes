#include "WindowChrome.h"

#include <QWindow>
#include <QGuiApplication>


WindowChrome::WindowChrome(bool chromeless, QObject* parent)
    : QObject(parent), chromeless_(chromeless) {}

bool WindowChrome::captionButtons() const {
#ifdef Q_OS_WIN
    return chromeless_;
#else
    return false;
#endif
}

bool WindowChrome::nativeLights() const {
#ifdef Q_OS_MACOS
    return chromeless_;
#else
    return false;
#endif
}

void WindowChrome::attach(QWindow* w) {
    if (!chromeless_ || !w) return;
    // The probes / --shot run on the offscreen platform: its window ids are not native handles.
    const QString platform = QGuiApplication::platformName();
#ifdef Q_OS_MACOS
    if (platform == QLatin1String("cocoa")) attachMac(w);
#endif
    // Windows: nothing here — the window flags (Main.qml) create the caption-less window.
    Q_UNUSED(platform);
}

bool WindowChrome::startMove(QWindow* w) {
    if (!chromeless_ || !w) return false;
    if (w->visibility() == QWindow::FullScreen) return false;
    return w->startSystemMove();
}

void WindowChrome::toggleZoom(QWindow* w) {
    if (!w) return;
    if (w->visibility() == QWindow::FullScreen) return;
    if (w->windowStates().testFlag(Qt::WindowMaximized)) w->showNormal();
    else w->showMaximized();
}
