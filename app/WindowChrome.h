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
//   Resizing is the OS's on both. `--native-frame` (dev) keeps the native frame; the row then draws no buttons.
#pragma once

#include <QObject>

class QWindow;

class WindowChrome : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool chromeless READ chromeless CONSTANT)
    Q_PROPERTY(bool captionButtons READ captionButtons CONSTANT) // Qt draws Windows' caption buttons over the row's right end
    Q_PROPERTY(bool nativeLights READ nativeLights CONSTANT)     // the native traffic lights sit in the row (macOS)
public:
    explicit WindowChrome(bool chromeless, QObject* parent = nullptr);
    bool chromeless() const { return chromeless_; }
    bool captionButtons() const;
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
