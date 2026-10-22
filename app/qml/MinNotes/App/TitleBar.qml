// The TITLE BAR (cleanup 2026-10-21 — the chromeless window, see WindowChrome.h): the one row that is always
// there, document or not. Left to right — on macOS the inset where the NATIVE traffic lights sit · the app MENU
// button (Windows only — macOS has its native menu bar; ruling 2026-10-21) · the PathBar (• File ▾ › Canvas › …,
// only while a document is open) · the PIN · on Windows our own minimize / maximize / close. Every stretch of the
// row no control claims DRAGS the window (ruling: all non-tab space), a double-click zooms — through our own
// handle on BOTH platforms (the drag starts after a small movement, so clicks stay clicks; QWindow::startSystemMove
// is AppKit's performWindowDrag / Windows' SC_DRAGMOVE loop, so edge snap and the OS motion stay native). The PIN
// (2026-10-22) is the same toggle as View ▸ Always on Top (⌥⌘T / Ctrl+Alt+T): the window stays above every other
// app's. The pin and the three window buttons are ONE component (ChromeButton: 34 × the row, 14-px glyph, grey
// hover, the close in red) — the user's walk of Qt's own 32 × 48 caption buttons: too large, and the pin mismatched.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    property var editor: null
    readonly property var pathBar: pathLoader.item          // the PathBar's probe hooks (null while no document is open)
    readonly property bool lightsInset: windowChrome.nativeLights && win.visibility !== Window.FullScreen
    readonly property int lightsWidth: 70                   // close · minimize · zoom end at x ≈ 59 on a standard title bar
    readonly property var appMenuButton: menuBtn            // probe
    readonly property var pinButton: pinBtn                 // probe
    height: Theme.dim.tabRowH
    color: Theme.colors.bgStrip
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border; z: 2 }

    // The move handle: under everything else in the row, so a press on a segment / button never reaches it. Windows
    // too (2026-10-22): leaving the bare strip to Qt's expanded-client-area hit test (an unaccepted press → HTCAPTION)
    // was unreliable — Qt detects the press through GetAsyncKeyState's "pressed since last call" bit, so a hover hit
    // test after a click eats the next press and the drag needs a second try. Accepting every press here makes the
    // hit test HTCLIENT consistently; the drag then goes through startSystemMove (SC_DRAGMOVE), and Qt's
    // WM_EXITSIZEMOVE handler syncs the release the move loop swallowed.
    MouseArea {
        id: drag
        anchors.fill: parent
        enabled: windowChrome.chromeless
        property real px: 0; property real py: 0
        property bool armed: false
        onPressed: (mouse) => { px = mouse.x; py = mouse.y; armed = windowChrome.chromeless }
        onPositionChanged: (mouse) => {
            if (!armed || !pressed) return
            if (Math.abs(mouse.x - px) + Math.abs(mouse.y - py) < 4) return
            armed = false
            windowChrome.startMove(win)
        }
        onReleased: armed = false
        onDoubleClicked: (mouse) => { if (windowChrome.chromeless) windowChrome.toggleZoom(win) }
    }

    Row {
        id: left
        anchors.left: parent.left; anchors.top: parent.top
        height: bar.height - 1
        spacing: 0
        Item { width: bar.lightsInset ? bar.lightsWidth : 6; height: 1 }   // the traffic lights' room, or the strip's edge
        // The app menu (Windows): the hamburger, where the menu bar used to be. macOS has its native menu bar.
        Rectangle {
            id: menuBtn
            visible: !win.nativeMenuBar
            width: visible ? 30 : 0; height: parent.height
            radius: Theme.dim.radiusSmall
            readonly property bool menuOpen: !!win.appMenu && win.appMenu.visible
            color: menuOpen ? Theme.colors.divider : (menuMA.containsMouse ? Theme.colors.affordanceHover : Theme.colors.affordanceIdleClear)
            Behavior on color { ColorAnimation { duration: Theme.dim.hoverMs } }
            Icon { anchors.centerIn: parent; name: "list"; size: 16
                   color: menuBtn.menuOpen || menuMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted }
            MouseArea { id: menuMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: { if (!win.appMenu) return; if (win.appMenu.visible) win.appMenu.close(); else win.appMenu.popup(menuBtn, 0, menuBtn.height + 1) } }
            FlatToolTip { visible: menuMA.containsMouse && !menuBtn.menuOpen; text: qsTr("Menu"); x: 0; y: menuBtn.height + 6 }
        }
    }

    // The path: • File ▾ › Canvas › Page ▾ › Frame ▾ — the document's hierarchy, while there is one.
    Loader {
        id: pathLoader
        anchors.left: left.right; anchors.right: pinBtn.left; anchors.top: parent.top
        height: bar.height
        active: blockModel.documentOpen
        sourceComponent: PathBar { editor: bar.editor; color: "transparent" }
    }

    // The row's buttons: one look for the pin and the window buttons.
    component ChromeButton: Rectangle {
        id: cb
        property string icon: ""
        property int iconSize: 14
        property string iconWeight: "regular"
        property bool checked: false
        property bool danger: false      // the close: hover in red
        property string tip: ""
        signal clicked()
        width: 34; height: bar.height - 1
        radius: Theme.dim.radiusSmall
        color: checked ? Theme.colors.divider
             : cbMA.containsMouse ? (danger ? Theme.colors.error : Theme.colors.affordanceHover) : Theme.colors.affordanceIdleClear
        Behavior on color { ColorAnimation { duration: Theme.dim.hoverMs } }
        Icon { anchors.centerIn: parent; name: cb.icon; size: cb.iconSize; weight: cb.iconWeight
               color: cb.checked || cbMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted }
        MouseArea { id: cbMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: cb.clicked() }
        FlatToolTip { visible: cbMA.containsMouse; text: cb.tip; x: Math.min(0, cb.width - implicitWidth); y: cb.height + 6 }
    }

    // The pin: Always on Top. Filled glyph + the checked fill while pinned.
    ChromeButton {
        id: pinBtn
        anchors.right: buttons.left; anchors.top: parent.top; anchors.rightMargin: windowChrome.customButtons ? 4 : 6
        icon: "push-pin"; iconWeight: checked ? "fill" : "regular"
        checked: windowChrome.alwaysOnTop
        tip: checked ? qsTr("Always on Top — click to unpin") : qsTr("Always on Top")
        onClicked: windowChrome.alwaysOnTop = !windowChrome.alwaysOnTop
    }

    // Windows: our own minimize / maximize / close at the row's right end.
    Row {
        id: buttons
        anchors.right: parent.right; anchors.top: parent.top; anchors.rightMargin: visible ? 4 : 0
        height: bar.height - 1
        visible: windowChrome.customButtons
        width: visible ? implicitWidth : 0
        readonly property var minimizeButton: minBtn   // probe
        ChromeButton { id: minBtn; icon: "minus"; tip: qsTr("Minimize"); onClicked: win.showMinimized() }
        ChromeButton { icon: win.visibility === Window.Maximized ? "browsers" : "square"; iconSize: 13
                       tip: win.visibility === Window.Maximized ? qsTr("Restore") : qsTr("Maximize")
                       onClicked: windowChrome.toggleZoom(win) }
        ChromeButton { icon: "x"; danger: true; tip: qsTr("Close"); onClicked: win.close() }
    }
}
