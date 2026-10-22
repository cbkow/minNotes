// WindowChrome — the Windows half (see WindowChrome.h): the pin. SetWindowPos with HWND_TOPMOST / HWND_NOTOPMOST
// and nothing else (no move, no size, no activation, NO SWP_FRAMECHANGED — the expanded client area Qt created
// must not be re-measured; stripping / re-applying frame styles after creation is what left the row above the
// screen on 2026-10-21). Qt.WindowStaysOnTopHint would route through QWindowsWindow::setWindowFlags, which
// re-applies every style with SWP_FRAMECHANGED.
#include "WindowChrome.h"

#include <QWindow>

#include <windows.h>

// The window is created with CustomizeWindowHint and no button hints (so Qt paints no caption strip), which also
// leaves WS_MINIMIZEBOX / WS_MAXIMIZEBOX off the style. Put them back: the taskbar's click-to-minimize, Win+↑ / ↓,
// Aero snap and DefWindowProc's caption double-click all read the style at the time of the gesture. A plain
// SetWindowLongPtr — again no SWP_FRAMECHANGED: the two bits change no frame metric.
void WindowChrome::attachWin(QWindow* w) {
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd) return;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
}

void WindowChrome::setTopmostWin(QWindow* w, bool on) {
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd) return;
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
