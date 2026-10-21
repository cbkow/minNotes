#include "WindowChrome.h"

#include <QWindow>
#include <QGuiApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

WindowChrome::WindowChrome(bool chromeless, QObject* parent)
    : QObject(parent), chromeless_(chromeless) {}

bool WindowChrome::customButtons() const {
#ifdef Q_OS_MACOS
    return false;                 // the native traffic lights stay
#else
    return chromeless_;
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
#ifdef Q_OS_WIN
    // QCView's compact-mode recipe (native_fullscreen_win.cpp): the caption, system menu and caption buttons go;
    // WS_THICKFRAME stays so the resize borders and Win+arrow snapping keep working. No DWM call — Windows 11
    // rounds the corners itself.
    if (platform != QLatin1String("windows")) return;
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd) return;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR strip = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_DLGFRAME | WS_BORDER;
    SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~strip) | WS_THICKFRAME);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle & ~(WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
    if (w->visibility() == QWindow::Maximized) w->showNormal();
    else w->showMaximized();
}
