// The TITLE BAR (cleanup 2026-10-21 — the chromeless window, see WindowChrome.h): the one row that is always
// there, document or not. Left to right — on macOS the inset where the NATIVE traffic lights sit · the app MENU
// button (Windows only — macOS has its native menu bar; ruling 2026-10-21) · the PathBar (• File ▾ › Canvas › …,
// only while a document is open) · on Windows the space Qt's caption buttons occupy. Every stretch of the row no
// control claims DRAGS the window (ruling: all non-tab space), a double-click zooms — on macOS through our own
// handle (the drag starts after a small movement, so clicks stay clicks), on Windows natively.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    property var editor: null
    readonly property var pathBar: pathLoader.item          // the PathBar's probe hooks (null while no document is open)
    readonly property bool lightsInset: windowChrome.nativeLights && win.visibility !== Window.FullScreen
    readonly property int lightsWidth: 70                   // close · minimize · zoom end at x ≈ 59 on a standard title bar
    readonly property var appMenuButton: menuBtn            // probe
    // Windows: Qt's title-bar height (32 at 100 %), so its caption glyphs centre on the row; elsewhere the house row.
    height: windowChrome.captionButtons && win.SafeArea.margins.top > 0 ? win.SafeArea.margins.top : Theme.dim.tabRowH
    color: Theme.colors.bgStrip
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border; z: 2 }

    // The move handle (macOS): under everything else in the row, so a press on a segment / button never reaches it.
    // Windows needs none — a press no item accepts is the native caption (Qt's expanded-client-area hit test).
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
            color: menuOpen ? Theme.colors.divider : (menuMA.containsMouse ? Theme.colors.affordanceHover : "transparent")
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
        anchors.left: left.right; anchors.right: buttons.left; anchors.top: parent.top
        height: bar.height
        active: blockModel.documentOpen
        sourceComponent: PathBar { editor: bar.editor; color: "transparent" }
    }

    // Windows: Qt draws and hit-tests the caption buttons here (three of titleBarHeight × 1.5 each, Qt's rule);
    // the row just keeps clear of them.
    Item {
        id: buttons
        anchors.right: parent.right; anchors.top: parent.top
        height: parent.height
        width: windowChrome.captionButtons ? Math.round(win.SafeArea.margins.top * 4.5) : 0
    }
}
