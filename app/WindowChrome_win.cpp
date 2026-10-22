// WindowChrome — the Windows half (see WindowChrome.h): the pin. SetWindowPos with HWND_TOPMOST / HWND_NOTOPMOST
// and nothing else (no move, no size, no activation, NO SWP_FRAMECHANGED — the expanded client area Qt created
// must not be re-measured; stripping / re-applying frame styles after creation is what left the row above the
// screen on 2026-10-21). Qt.WindowStaysOnTopHint would route through QWindowsWindow::setWindowFlags, which
// re-applies every style with SWP_FRAMECHANGED.
#include "WindowChrome.h"

#include <QWindow>

#include <windows.h>

// The window is created with CustomizeWindowHint + WindowTitleHint and no button hints (Main.qml): Qt then paints
// no caption buttons, keeps its hit test on the title-row branch, and leaves WS_MINIMIZEBOX / WS_MAXIMIZEBOX off
// the style. Two repairs: (1) put the two styles back — the taskbar's click-to-minimize, Win+↑ / ↓, Aero snap and
// DefWindowProc's caption double-click read the style at the time of the gesture; a plain SetWindowLongPtr, no
// SWP_FRAMECHANGED, the bits change no frame metric. (2) Hide the layered child window (class "_q_titlebar",
// qwindowswindow.cpp) into which Qt paints the app icon + title for the title hint — it sits over our row. Qt
// re-shows it only from setWindowFlags_sys, which nothing here calls; resizes just MoveWindow it (stays hidden).
void WindowChrome::attachWin(QWindow* w) {
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd) return;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    if (HWND bar = FindWindowExW(hwnd, nullptr, L"_q_titlebar", nullptr))
        ShowWindow(bar, SW_HIDE);
}

void WindowChrome::setTopmostWin(QWindow* w, bool on) {
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd) return;
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
