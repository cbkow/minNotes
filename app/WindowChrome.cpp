#include "WindowChrome.h"

#include <QWindow>
#include <QGuiApplication>


WindowChrome::WindowChrome(bool chromeless, QObject* parent)
    : QObject(parent), chromeless_(chromeless) {}

bool WindowChrome::customButtons() const {
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
    if (!w) return;
    // The probes / --shot run on the offscreen platform: its window ids are not native handles.
    const QString platform = QGuiApplication::platformName();
    window_ = w;
    nativeWindow_ = platform == QLatin1String("cocoa") || platform == QLatin1String("windows");
#ifdef Q_OS_MACOS
    if (chromeless_ && nativeWindow_) attachMac(w);
#endif
#ifdef Q_OS_WIN
    if (chromeless_ && nativeWindow_) attachWin(w);
#endif
    applyAlwaysOnTop();
    // Leaving fullscreen / a maximize round trip: the platform plugin may have re-applied the level from the
    // window flags (which never carry the pin) — assert ours again.
    connect(w, &QWindow::windowStateChanged, this, [this] { applyAlwaysOnTop(); });
}

void WindowChrome::setAlwaysOnTop(bool on) {
    if (alwaysOnTop_ == on) return;
    alwaysOnTop_ = on;
    applyAlwaysOnTop();
    emit alwaysOnTopChanged();
}

void WindowChrome::applyAlwaysOnTop() {
    if (!window_ || !nativeWindow_) return;
    if (window_->visibility() == QWindow::FullScreen) return;   // the OS owns the space; the pin returns with the window
#ifdef Q_OS_MACOS
    setLevelMac(window_, alwaysOnTop_);
#endif
#ifdef Q_OS_WIN
    setTopmostWin(window_, alwaysOnTop_);
#endif
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
