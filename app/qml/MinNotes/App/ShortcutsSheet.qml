// The keyboard-shortcuts sheet (2026-10-21): a modal in-window sheet listing every shortcut from ShortcutCatalog.js
// (the same list the docs page is generated from), two balanced columns of sections, scrolling when the window is
// short. Help ▸ Keyboard Shortcuts… on both platforms, and ⌘/ (Ctrl+/ on Windows). Esc or a click outside closes.
import QtQuick
import QtQuick.Controls
import "ShortcutCatalog.js" as Catalog

Popup {
    id: sheet
    modal: true
    dim: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(700, (Overlay.overlay ? Overlay.overlay.width : 700) - 48)
    height: Math.min(head.height + body.contentHeight + 16, (Overlay.overlay ? Overlay.overlay.height : 600) - 48)
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.dim.popupMs } }
    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.45) }
    background: Rectangle {
        color: Theme.colors.surface; radius: Theme.dim.radius
        border.width: 1; border.color: Theme.colors.border
    }

    readonly property bool mac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property int colGap: 28
    readonly property int sideGap: 20
    readonly property int colW: Math.floor((width - 2 * sideGap - colGap) / 2)
    readonly property int rowH: 22
    // Two columns, balanced by row count (each section is ~2.4 rows of header), filled greedily in catalogue order.
    readonly property var columns: {
        const left = [], right = []
        let lh = 0, rh = 0
        for (let i = 0; i < Catalog.sections.length; ++i) {
            const s = Catalog.sections[i], h = s.rows.length + 2.4
            if (lh <= rh) { left.push(s); lh += h } else { right.push(s); rh += h }
        }
        return [left, right]
    }
    function toggle() { if (opened) close(); else open() }
    Component.onCompleted: if (Qt.application.arguments.indexOf("--shot-shortcuts") >= 0) Qt.callLater(open)   // dev: `--shot=<png> --shot-shortcuts`

    contentItem: Item {
        Item {
            id: head
            width: parent.width; height: 48
            Text {
                x: sheet.sideGap; anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Keyboard Shortcuts")
                color: Theme.colors.textBright
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody; font.bold: true
            }
            Text {
                anchors.right: parent.right; anchors.rightMargin: sheet.sideGap; anchors.verticalCenter: parent.verticalCenter
                text: sheet.mac ? qsTr("Esc closes") : qsTr("Esc closes · ⌘ means Ctrl in the docs")
                color: Theme.colors.textSubtle
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border }
        }
        Flickable {
            id: body
            anchors { top: head.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
            contentWidth: width
            contentHeight: Math.max(colLeft.implicitHeight, colRight.implicitHeight) + 16
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: MnScrollBar {}
            Column { id: colLeft;  x: sheet.sideGap; y: 8; width: sheet.colW; spacing: 10; Repeater { model: sheet.columns[0]; delegate: Section {} } }
            Column { id: colRight; x: sheet.sideGap + sheet.colW + sheet.colGap; y: 8; width: sheet.colW; spacing: 10; Repeater { model: sheet.columns[1]; delegate: Section {} } }
        }
    }

    component Section: Column {
        required property var modelData
        width: sheet.colW
        spacing: 0
        Text {
            width: parent.width
            text: modelData.title
            elide: Text.ElideRight
            color: Theme.colors.textMuted
            font.family: Theme.font.family; font.pixelSize: 11; font.bold: true
            topPadding: 8; bottomPadding: 4
        }
        Text {
            visible: !!modelData.note
            width: parent.width; wrapMode: Text.Wrap
            text: (modelData.note || "").replace(/\s*[—:-]*\s*see \[.*$/, "")   // the docs' cross-link stays in the docs
            color: Theme.colors.textSubtle
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            bottomPadding: 4
        }
        Repeater {
            model: modelData.rows
            delegate: Item {
                required property var modelData
                required property int index
                width: sheet.colW; height: Math.max(sheet.rowH, action.implicitHeight + 6, keys.implicitHeight + 6)
                Rectangle { anchors.fill: parent; radius: Theme.dim.radiusSmall; color: Theme.colors.surfaceHover; opacity: index % 2 ? 0 : 0.35 }
                Text {
                    id: action
                    anchors.left: parent.left; anchors.leftMargin: 6
                    anchors.right: keys.left; anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData[0]
                    wrapMode: Text.Wrap
                    color: Theme.colors.text
                    font.family: parent.parent.modelData.mono ? Theme.font.mono : Theme.font.family
                    font.pixelSize: parent.parent.modelData.mono ? Theme.font.sizeSmall : Theme.font.sizeChrome
                }
                Text {
                    id: keys
                    anchors.right: parent.right; anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, Math.floor(sheet.colW * 0.45))   // a long key phrase wraps rather than squeezing the action
                    wrapMode: Text.Wrap
                    text: Catalog.display(modelData[1], sheet.mac)
                    horizontalAlignment: Text.AlignRight
                    color: Theme.colors.textBright
                    font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
                }
            }
        }
    }
}
