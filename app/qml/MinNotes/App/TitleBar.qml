// The TITLE BAR (cleanup 2026-10-21 — the chromeless window, see WindowChrome.h): the one row that is always
// there, document or not. Left to right — on macOS the inset where the NATIVE traffic lights sit · the app MENU
// button (Windows only — macOS has its native menu bar; ruling 2026-10-21) · the PathBar (• File ▾ › Canvas › …,
// only while a document is open) · on Windows our own minimize / maximize / close. Every stretch of the row no
// control claims DRAGS the window (ruling: all non-tab space), a double-click zooms. The drag starts after a
// small movement, so clicks and double-clicks on the bare strip stay clicks.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    property var editor: null
    readonly property var pathBar: pathLoader.item          // the PathBar's probe hooks (null while no document is open)
    readonly property bool lightsInset: windowChrome.nativeLights && win.visibility !== Window.FullScreen
    readonly property int lightsWidth: 70                   // close · minimize · zoom end at x ≈ 59 on a standard title bar
    readonly property var appMenuButton: menuBtn            // probe
    height: Theme.dim.tabRowH
    color: Theme.colors.bgStrip
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border; z: 2 }

    // The move handle: under everything else in the row, so a press on a segment / button never reaches it.
    MouseArea {
        id: drag
        anchors.fill: parent
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
            radius: 0
            readonly property bool menuOpen: !!win.appMenu && win.appMenu.visible
            color: menuOpen ? Theme.colors.divider : (menuMA.containsMouse ? Theme.colors.affordanceHover : "transparent")
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
        anchors.left: left.right; anchors.right: buttons.left; anchors.top: parent.top
        height: bar.height
        active: blockModel.documentOpen
        sourceComponent: PathBar { editor: bar.editor; color: "transparent" }
    }

    // Our own caption buttons (Windows): the OS's widths (46), the row's height, grey hover, the close in red.
    Row {
        id: buttons
        anchors.right: parent.right; anchors.top: parent.top
        height: bar.height - 1
        visible: windowChrome.customButtons
        width: visible ? implicitWidth : 0
        component CaptionButton: Rectangle {
            id: cb
            property string icon: ""
            property int iconSize: 14
            property bool danger: false
            property string tip: ""
            signal clicked()
            width: 46; height: parent.height; radius: 0
            color: cbMA.containsMouse ? (danger ? Theme.colors.error : Theme.colors.affordanceHover) : "transparent"
            Icon { anchors.centerIn: parent; name: cb.icon; size: cb.iconSize
                   color: cbMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted }
            MouseArea { id: cbMA; anchors.fill: parent; hoverEnabled: true; onClicked: cb.clicked() }
            FlatToolTip { visible: cbMA.containsMouse; text: cb.tip; x: (cb.width - implicitWidth) / 2; y: cb.height + 6 }
        }
        CaptionButton { icon: "minus"; tip: qsTr("Minimize"); onClicked: win.showMinimized() }
        CaptionButton { icon: win.visibility === Window.Maximized ? "browsers" : "square"; iconSize: 12
                        tip: win.visibility === Window.Maximized ? qsTr("Restore") : qsTr("Maximize")
                        onClicked: windowChrome.toggleZoom(win) }
        CaptionButton { icon: "x"; danger: true; tip: qsTr("Close"); onClicked: win.close() }
    }
}
