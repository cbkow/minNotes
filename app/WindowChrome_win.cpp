// WindowChrome — the Windows half (see WindowChrome.h): the pin. SetWindowPos with HWND_TOPMOST / HWND_NOTOPMOST
// and nothing else (no move, no size, no activation, no SWP_FRAMECHANGED). Qt.WindowStaysOnTopHint would route
// through QWindowsWindow::setWindowFlags, which re-applies every style with SWP_FRAMECHANGED.
#include "WindowChrome.h"

#include <QWindow>

#include <windows.h>

void WindowChrome::setTopmostWin(QWindow* w, bool on) {
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd) return;
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
