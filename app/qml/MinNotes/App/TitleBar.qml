// The TITLE BAR (cleanup 2026-10-21 — the chromeless window, see WindowChrome.h): the one row that is always
// there, document or not. Left to right — on macOS the inset where the NATIVE traffic lights sit · the app MENU
// button (Windows only — macOS has its native menu bar; ruling 2026-10-21) · the PathBar (• File ▾ › Canvas › …,
// only while a document is open). On macOS the WHOLE row drags the window (2026-10-23, the ufb model): the bare
// stretches through the handle below and the controls through DragMouseArea — a press that moves a few px is handed
// to the OS move loop, so clicks stay clicks and the path's long file name is drag surface too. A double-click on a
// bare stretch zooms. On Windows the row is a toolbar under the native title bar (2026-10-24) and drags nothing.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    property var editor: null
    readonly property var pathBar: pathLoader.item          // the PathBar's probe hooks (null while no document is open)
    readonly property var backButton: backBtn               // probe
    readonly property var forwardButton: forwardBtn
    readonly property bool lightsInset: windowChrome.nativeLights && win.visibility !== Window.FullScreen
    readonly property int lightsWidth: 70                   // close · minimize · zoom end at x ≈ 59 on a standard title bar
    readonly property var appMenuButton: menuBtn            // probe
    height: Theme.dim.tabRowH
    color: Theme.colors.bgStrip
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border; z: 2 }

    // The move handle (macOS): under everything else in the row — the controls above it are DragMouseAreas, so a
    // press on a segment / button drags through them instead.
    MouseArea {
        id: drag
        anchors.fill: parent
        enabled: windowChrome.nativeLights
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
            DragMouseArea { id: menuMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: { if (!win.appMenu) return; if (win.appMenu.visible) win.appMenu.close(); else win.appMenu.popup(menuBtn, 0, menuBtn.height + 1) } }
            FlatToolTip { visible: menuMA.containsMouse && !menuBtn.menuOpen; text: qsTr("Menu"); x: 0; y: menuBtn.height + 6 }
        }
        // BACK / FORWARD (2026-10-22): the visited views, browser-like — dimmed when there is nowhere to go.
        component NavBtn: Rectangle {
            id: nb
            required property int dir
            readonly property bool can: dir < 0 ? win.nav.canBack : win.nav.canForward
            width: 24; height: parent.height
            radius: Theme.dim.radiusSmall
            color: can && nbMA.containsMouse ? Theme.colors.affordanceHover : Theme.colors.affordanceIdleClear
            Behavior on color { ColorAnimation { duration: Theme.dim.hoverMs } }
            Icon { anchors.centerIn: parent; name: nb.dir < 0 ? "caret-left" : "caret-right"; size: 12
                   color: !nb.can ? Theme.colors.textSubtle : nbMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted }
            DragMouseArea { id: nbMA; anchors.fill: parent; hoverEnabled: true; enabled: nb.can; cursorShape: Qt.PointingHandCursor
                        onClicked: win.nav.go(nb.dir) }
            FlatToolTip { visible: nbMA.containsMouse && nb.can; x: 0; y: nb.height + 6
                          text: (nb.dir < 0 ? qsTr("Back to ") : qsTr("Forward to ")) + (nb.dir < 0 ? win.nav.backLabel : win.nav.forwardLabel) }
        }
        Item { width: 2; height: 1 }
        NavBtn { id: backBtn; dir: -1 }
        NavBtn { id: forwardBtn; dir: 1 }
        Item { width: 4; height: 1 }
    }

    // The path: • File ▾ › Canvas › Page ▾ › Frame ▾ — the document's hierarchy, while there is one.
    Loader {
        id: pathLoader
        anchors.left: left.right; anchors.right: parent.right; anchors.top: parent.top
        height: bar.height
        active: blockModel.documentOpen
        sourceComponent: PathBar { editor: bar.editor; color: "transparent" }
    }
}
