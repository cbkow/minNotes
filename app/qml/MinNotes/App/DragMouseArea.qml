// A MouseArea that is ALSO a window-drag handle (2026-10-23, the ufb model — ufb's DragTabMouseArea): the title
// row's controls (the path segments, back / forward) would otherwise swallow the press
// and shrink the row's drag surface to the gaps between them. The press is recorded; a plain click (press +
// release without movement) reaches onClicked as before; once the pointer has moved a few px the press is handed
// to the OS move loop instead (WindowChrome::startMove → QWindow::startSystemMove), which cancels this area's grab
// so no click fires. Right-button presses, fullscreen, Windows (native title bar since 2026-10-24) and the
// `--native-frame` dev mode are untouched (not chromeless: the press stays an ordinary one).
import QtQuick

MouseArea {
    property real _px: 0
    property real _py: 0
    property bool _armed: false
    onPressed: (mouse) => {
        _px = mouse.x; _py = mouse.y
        _armed = windowChrome.chromeless && mouse.button === Qt.LeftButton
    }
    onPositionChanged: (mouse) => {
        if (!_armed || !pressed) return
        if (Math.abs(mouse.x - _px) + Math.abs(mouse.y - _py) < 6) return
        _armed = false
        windowChrome.startMove(win)
    }
    onReleased: _armed = false
    onCanceled: _armed = false
}
