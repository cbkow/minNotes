import QtQuick
import QtQuick.Controls
import QtCore

ApplicationWindow {
    id: win
    visible: true
    width: 960
    height: 760
    title: "minNotes"
    color: Theme.colors.bg

    // First app setting: window geometry persists across launches. Family
    // pattern (QtCore Settings) with UFB's hard-won rules: capture only
    // windowed-mode geometry (never the maximized rect, so un-maximize keeps
    // its target), debounce the capture, restore the size always but the
    // position only when it still overlaps a screen, and gate capture until
    // the restore has run (else the restore itself overwrites the store).
    Settings {
        id: winState
        category: "window"
        property int  w: 960
        property int  h: 760
        property int  wx: -1
        property int  wy: -1
        property bool maximized: false
    }
    property bool _geomRestored: false
    function _fitsAScreen(wx, wy, ww, wh) {
        var screens = Qt.application.screens
        for (var i = 0; i < screens.length; ++i) {
            var s = screens[i]
            if (wx + ww - 50 >= s.virtualX && wx + 50 <= s.virtualX + s.width
                && wy + wh - 50 >= s.virtualY && wy + 50 <= s.virtualY + s.height)
                return true
        }
        return false
    }
    Component.onCompleted: {
        width = Math.max(640, winState.w)
        height = Math.max(480, winState.h)
        if (winState.wx >= 0 && _fitsAScreen(winState.wx, winState.wy, width, height)) {
            x = winState.wx; y = winState.wy
        }
        if (winState.maximized) win.showMaximized()
        _geomRestored = true
    }
    function _captureGeometry() {
        if (!_geomRestored) return
        if (win.visibility === Window.Maximized || win.visibility === Window.FullScreen) {
            winState.maximized = (win.visibility === Window.Maximized)
            return
        }
        winState.maximized = false
        winState.w = win.width; winState.h = win.height
        winState.wx = win.x;    winState.wy = win.y
    }
    onWidthChanged: geomSave.restart()
    onHeightChanged: geomSave.restart()
    onXChanged: geomSave.restart()
    onYChanged: geomSave.restart()
    onVisibilityChanged: geomSave.restart()
    Timer { id: geomSave; interval: 500; onTriggered: win._captureGeometry() }

    // Thin left action rail + (editor page over a bottom status strip) + a
    // collapsible right inspector that slides in/out (default hidden → "min" by
    // default). The left rail's palette button toggles the inspector.
    Row {
        anchors.fill: parent
        LeftRail { id: rail; height: parent.height; editor: editor; inspector: inspector }
        Column {
            width: parent.width - rail.width - inspector.width; height: parent.height
            // The validated passive-surface editor (model owns the cursor; blocks
            // are passive; overlay-drawn caret/selection). Ported from spike Arm C.
            Editor { id: editor; width: parent.width; height: parent.height - tabs.height - bottom.height
                     inspector: inspector }
            TableTabs { id: tabs; width: parent.width; editor: editor }
            BottomRail { id: bottom; width: parent.width; editor: editor }
        }
        Inspector { id: inspector; height: parent.height; editor: editor }
    }
}
