// WindowChrome — the Windows half (see WindowChrome.h): the pin. SetWindowPos with HWND_TOPMOST / HWND_NOTOPMOST
// and nothing else (no move, no size, no activation, NO SWP_FRAMECHANGED — the expanded client area Qt created
// must not be re-measured; stripping / re-applying frame styles after creation is what left the row above the
// screen on 2026-10-21). Qt.WindowStaysOnTopHint would route through QWindowsWindow::setWindowFlags, which
// re-applies every style with SWP_FRAMECHANGED.
#include "WindowChrome.h"

#include <QWindow>

#include <windows.h>

void WindowChrome::setTopmostWin(QWindow* w, bool on) {
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd) return;
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Qt's caption strip (qwindowswindow.cpp, getTitleBarHeight_sys): title-bar height = SM_CYCAPTION + the resize
// border (SM_CXSIZEFRAME + SM_CXPADDEDBORDER), all at the window's dpi, in NATIVE pixels; each of the three buttons
// is int(height × 1.5) wide. Reported here in logical pixels for the row.
void WindowChrome::captionMetricsWin(QWindow* w, qreal& height, qreal& width) const {
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd) return;
    const UINT dpi = GetDpiForWindow(hwnd);
    const int native = GetSystemMetricsForDpi(SM_CYCAPTION, dpi)
                     + GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                     + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const int button = int(native * 1.5);
    const qreal dpr = w->devicePixelRatio() > 0 ? w->devicePixelRatio() : 1.0;
    height = native / dpr;
    width = button * 3 / dpr;
}
