// WindowChrome — the macOS half (see WindowChrome.h). Qt's ExpandedClientAreaHint + NoTitleBarBackgroundHint
// (set in Main.qml) extend the content under a transparent title bar and keep the native traffic lights; what
// Qt leaves on is the TITLE TEXT, which AppKit still centres in the (now transparent) title bar over our row.
// Hide it — the path bar's file segment is the title. Everything else stays AppKit's: zoom, fullscreen, tiling.
#include "WindowChrome.h"

#include <QWindow>

#import <AppKit/AppKit.h>

void WindowChrome::attachMac(QWindow* w) {
    NSView* view = reinterpret_cast<NSView*>(w->winId());
    NSWindow* win = view ? view.window : nil;
    if (!win) return;
    win.titleVisibility = NSWindowTitleHidden;
    win.titlebarAppearsTransparent = YES;
}

// The pin: NSFloatingWindowLevel keeps the window over every normal-level window of every app (and under
// panels / the menu bar); NSNormalWindowLevel is the plain state. In place — the NSWindow is never re-created.
void WindowChrome::setLevelMac(QWindow* w, bool floating) {
    NSView* view = reinterpret_cast<NSView*>(w->winId());
    NSWindow* win = view ? view.window : nil;
    if (!win) return;
    win.level = floating ? NSFloatingWindowLevel : NSNormalWindowLevel;
}
