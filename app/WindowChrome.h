// WindowChrome — the chromeless window (cleanup, 2026-10-21; macOS only since 2026-10-24). The native title bar
// is gone; the path-bar row (TitleBar.qml) is the title bar: every stretch of it drags the window — the bare stretches through the row's
// handle / the native caption, the controls through DragMouseArea.qml (2026-10-23, the ufb model: a press that
// moves a few px becomes the move) — and a double-click on a bare stretch zooms. "As native as possible" (user ruling):
//   macOS   — the window keeps its titled style with Qt's ExpandedClientAreaHint + NoTitleBarBackgroundHint
//             (set in Main.qml): the content extends under a transparent title bar, the NATIVE traffic lights
//             stay (native zoom / fullscreen / tiling keep working), and the row insets its content past them.
//             The system menu bar stays native (QCView learned that hiding it empties the top-of-screen bar).
//   Windows — the NATIVE frame and title bar (2026-10-24, as minRender 0.5.9). Qt's expanded client area painted
//             its own caption buttons: on min / max hover a solid white block that hid the glyph (dark and light
//             OS mode alike, seen on the box), Windows 10 glyphs + a white 1-px border there, and a native drag
//             that often needed a second press. The row is an ordinary toolbar under the title bar; main.cpp's
//             dark colour scheme makes Qt darken the title bar and border. (2026-10-22: our own caption buttons
//             were tried and REVERTED — Qt's hit test fought anything in the title zone.)
//   Resizing is the OS's. `--native-frame` (dev) keeps the native frame on macOS too; the row then has no inset.
// ALWAYS ON TOP (2026-10-22, the pin): `alwaysOnTop` keeps the window above every other app's — View ▸ Always on
// Top (⌥⌘T / Ctrl+Alt+T) and the toolbar's pin button (right of the Inspector toggle), remembered in Settings
// (Main.qml's viewPrefs). It is
// applied at the OS level, never through Qt.WindowStaysOnTopHint: on macOS changing a QWindow's flags re-creates
// the NSWindow (the hidden title + the lights' inset would need re-attaching) — NSWindow.level does it in place;
// on Windows SetWindowPos(HWND_TOPMOST) alone (the hint would re-apply every style with SWP_FRAMECHANGED).
#pragma once

#include <QObject>

class QWindow;

class WindowChrome : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool chromeless READ chromeless CONSTANT)
    Q_PROPERTY(bool nativeLights READ nativeLights CONSTANT)     // the native traffic lights sit in the row (macOS)
    Q_PROPERTY(bool alwaysOnTop READ alwaysOnTop WRITE setAlwaysOnTop NOTIFY alwaysOnTopChanged) // the pin
public:
    explicit WindowChrome(bool chromeless, QObject* parent = nullptr);
    bool chromeless() const { return chromeless_; }
    bool nativeLights() const;
    // Apply what the flags can't once the window exists (macOS: hide the title text) and the remembered pin.
    void attach(QWindow* w);
    bool alwaysOnTop() const { return alwaysOnTop_; }
    // Remembered before the window exists (Settings restore), applied on attach; re-asserted after the OS
    // restores the window from fullscreen / a state change in case the platform plugin reset the level.
    void setAlwaysOnTop(bool on);
    // The row's gestures: a drag past the threshold hands the press to the OS move loop; a double-click zooms.
    Q_INVOKABLE bool startMove(QWindow* w);
    Q_INVOKABLE void toggleZoom(QWindow* w);
signals:
    void alwaysOnTopChanged();
private:
    bool chromeless_ = true;
    bool alwaysOnTop_ = false;
    QWindow* window_ = nullptr;
    bool nativeWindow_ = false;   // the attached window is a real cocoa / win32 one (not the offscreen platform's)
    void applyAlwaysOnTop();
#ifdef Q_OS_MACOS
    void attachMac(QWindow* w);   // WindowChrome_mac.mm: hide the title text AppKit still draws
    void setLevelMac(QWindow* w, bool floating);
#endif
#ifdef Q_OS_WIN
    void setTopmostWin(QWindow* w, bool on);   // WindowChrome_win.cpp
#endif
};
