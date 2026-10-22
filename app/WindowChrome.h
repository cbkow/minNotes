// WindowChrome — the chromeless window (cleanup, 2026-10-21). The native title bar is gone; the path-bar row
// (TitleBar.qml) is the title bar: every stretch of it no control claims drags the window, a double-click
// zooms. "As native as possible" (user ruling):
//   macOS   — the window keeps its titled style with Qt's ExpandedClientAreaHint + NoTitleBarBackgroundHint
//             (set in Main.qml): the content extends under a transparent title bar, the NATIVE traffic lights
//             stay (native zoom / fullscreen / tiling keep working), and the row insets its content past them.
//             The system menu bar stays native (QCView learned that hiding it empties the top-of-screen bar).
//   Windows — Qt's expanded client area (Main.qml flags): no top border, the DWM frame's shadow and corners, and
//             Qt draws + hit-tests the Windows 11 caption buttons at the row's right end (snap layouts included);
//             the row only reserves their width. Unclaimed presses on the row are a native caption drag.
//             (2026-10-22: our own buttons + our own drag handle were tried on Windows and REVERTED — Qt's hit test
//             synthesizes presses into QML in the title zone, and anything we put there fought it; the pin lives in
//             the toolbar instead.)
//   Resizing is the OS's on both. `--native-frame` (dev) keeps the native frame; the row then draws no buttons.
// ALWAYS ON TOP (2026-10-22, the pin): `alwaysOnTop` keeps the window above every other app's — View ▸ Always on
// Top (⌥⌘T / Ctrl+Alt+T) and the toolbar's pin button (right of the Inspector toggle), remembered in Settings
// (Main.qml's viewPrefs). It is
// applied at the OS level, never through Qt.WindowStaysOnTopHint: on macOS changing a QWindow's flags re-creates
// the NSWindow (the hidden title + the lights' inset would need re-attaching) — NSWindow.level does it in place;
// on Windows SetWindowPos(HWND_TOPMOST) alone, no SWP_FRAMECHANGED, so the expanded client area stays as created.
#pragma once

#include <QObject>

class QWindow;

class WindowChrome : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool chromeless READ chromeless CONSTANT)
    Q_PROPERTY(bool captionButtons READ captionButtons CONSTANT) // Qt draws Windows' caption buttons over the row's right end
    Q_PROPERTY(bool nativeLights READ nativeLights CONSTANT)     // the native traffic lights sit in the row (macOS)
    Q_PROPERTY(bool alwaysOnTop READ alwaysOnTop WRITE setAlwaysOnTop NOTIFY alwaysOnTopChanged) // the pin
    // Windows: Qt's caption strip measured at the WINDOW'S dpi, in logical px — the row's height and the width of
    // the three buttons it must keep clear of. (QML's SafeArea.margins.top is Qt's title-bar height computed at 96
    // dpi and then divided by the device pixel ratio: 24.8 on a 125 % screen where the buttons are really 31.2 tall
    // and 139 wide — the row came out short and the spacer narrow. 2026-10-22.) 0 elsewhere.
    Q_PROPERTY(qreal captionHeight READ captionHeight NOTIFY captionMetricsChanged)
    Q_PROPERTY(qreal captionWidth READ captionWidth NOTIFY captionMetricsChanged)
public:
    explicit WindowChrome(bool chromeless, QObject* parent = nullptr);
    bool chromeless() const { return chromeless_; }
    bool captionButtons() const;
    bool nativeLights() const;
    // Apply what the flags can't once the window exists (macOS: hide the title text) and the remembered pin.
    void attach(QWindow* w);
    bool alwaysOnTop() const { return alwaysOnTop_; }
    qreal captionHeight() const { return captionHeight_; }
    qreal captionWidth() const { return captionWidth_; }
    // Remembered before the window exists (Settings restore), applied on attach; re-asserted after the OS
    // restores the window from fullscreen / a state change in case the platform plugin reset the level.
    void setAlwaysOnTop(bool on);
    // The row's gestures: a drag past the threshold hands the press to the OS move loop; a double-click zooms.
    Q_INVOKABLE bool startMove(QWindow* w);
    Q_INVOKABLE void toggleZoom(QWindow* w);
signals:
    void alwaysOnTopChanged();
    void captionMetricsChanged();
private:
    bool chromeless_ = true;
    bool alwaysOnTop_ = false;
    qreal captionHeight_ = 0;
    qreal captionWidth_ = 0;
    void updateCaptionMetrics();   // on attach, a screen change (dpi), a state change
    QWindow* window_ = nullptr;
    bool nativeWindow_ = false;   // the attached window is a real cocoa / win32 one (not the offscreen platform's)
    void applyAlwaysOnTop();
#ifdef Q_OS_MACOS
    void attachMac(QWindow* w);   // WindowChrome_mac.mm: hide the title text AppKit still draws
    void setLevelMac(QWindow* w, bool floating);
#endif
#ifdef Q_OS_WIN
    void setTopmostWin(QWindow* w, bool on);   // WindowChrome_win.cpp
    void captionMetricsWin(QWindow* w, qreal& height, qreal& width) const;
#endif
};
