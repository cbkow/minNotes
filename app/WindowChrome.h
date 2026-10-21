// WindowChrome — the chromeless window (cleanup, 2026-10-21). The native title bar is gone; the path-bar row
// (TitleBar.qml) is the title bar: every stretch of it no control claims drags the window, a double-click
// zooms. "As native as possible" (user ruling):
//   macOS   — the window keeps its titled style with Qt's ExpandedClientAreaHint + NoTitleBarBackgroundHint
//             (set in Main.qml): the content extends under a transparent title bar, the NATIVE traffic lights
//             stay (native zoom / fullscreen / tiling keep working), and the row insets its content past them.
//             The system menu bar stays native (QCView learned that hiding it empties the top-of-screen bar).
//   Windows — Qt creates the window WITHOUT a caption (Main.qml: CustomizeWindowHint minus WindowTitleHint, the
//             min / max / close hints kept): WS_THICKFRAME stays so the OS resize edges + Win+arrow snapping keep
//             working, DWM rounds the corners, and the row draws its OWN minimize / maximize / close at the right
//             end. (QCView's post-creation SetWindowLongPtr strip was tried first — Qt's cached frame margins then
//             sat 30 px stale and the restored window's row landed above the screen.)
//   Resizing is the OS's on both. `--native-frame` (dev) keeps the native frame; the row then draws no buttons.
#pragma once

#include <QObject>

class QWindow;

class WindowChrome : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool chromeless READ chromeless CONSTANT)
    Q_PROPERTY(bool customButtons READ customButtons CONSTANT)   // the row draws minimize / maximize / close
    Q_PROPERTY(bool nativeLights READ nativeLights CONSTANT)     // the native traffic lights sit in the row (macOS)
public:
    explicit WindowChrome(bool chromeless, QObject* parent = nullptr);
    bool chromeless() const { return chromeless_; }
    bool customButtons() const;
    bool nativeLights() const;
    // Apply what the flags can't once the window exists (macOS: hide the title text). No-op elsewhere.
    void attach(QWindow* w);
    // The row's gestures: a drag past the threshold hands the press to the OS move loop; a double-click zooms.
    Q_INVOKABLE bool startMove(QWindow* w);
    Q_INVOKABLE void toggleZoom(QWindow* w);
private:
    bool chromeless_ = true;
#ifdef Q_OS_MACOS
    void attachMac(QWindow* w);   // WindowChrome_mac.mm: hide the title text AppKit still draws
#endif
};
