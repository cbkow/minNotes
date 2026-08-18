// ZoomBadge — a full-frame tab's one piece of zoom chrome: a quiet corner
// percent readout that is also a button. Click → a small popup (the in-editor
// context-menu recipe): Fit ⌘1, 100% ⌘0, and Fit ink when overflow ink
// exists. Gestures and keys do the real zooming; this is the readout plus the
// teaching surface for the shortcuts. Props/signals only, no sketch
// dependencies — the PDF tab could mount it verbatim.

import QtQuick
import QtQuick.Controls.Basic

Item {
    id: badge
    property real zoomValue: 1.0
    property bool showFitInk: false
    signal fitRequested()
    signal hundredRequested()
    signal fitInkRequested()

    implicitWidth: label.implicitWidth + 16
    implicitHeight: 24

    Rectangle {
        anchors.fill: parent
        radius: 0
        color: badgeMA.containsMouse || menu.visible ? Theme.colors.surfaceRaised : "transparent"
        border.width: 1
        border.color: badgeMA.containsMouse || menu.visible ? Theme.colors.divider : "transparent"
    }
    Text {
        id: label
        anchors.centerIn: parent
        text: Math.round(badge.zoomValue * 100) + "%"
        color: badgeMA.containsMouse || menu.visible ? Theme.colors.textBright
                                                     : Theme.colors.textSubtle
        font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
    }
    MouseArea {
        id: badgeMA
        anchors.fill: parent; hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: menu.visible ? menu.close() : menu.open()
    }

    component ZoomRow: Rectangle {
        property alias text: rowLabel.text
        property alias shortcut: rowShortcut.text
        signal activated()
        width: 148; height: 24
        color: rowMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
        Text {
            id: rowLabel
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: 10
            color: Theme.colors.text
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }
        Text {
            id: rowShortcut
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right; anchors.rightMargin: 10
            color: Theme.colors.textSubtle
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }
        MouseArea {
            id: rowMA
            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
            onClicked: { parent.activated(); menu.close() }
        }
    }

    Popup {
        id: menu
        x: badge.width - width
        y: -height - 4          // opens upward from the bottom-corner badge
        padding: 4; z: 60
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: Theme.colors.surfaceRaised; radius: 0
            border.width: 1; border.color: Theme.colors.border
        }
        contentItem: Column {
            ZoomRow { text: qsTr("Fit");  shortcut: "⌘ 1"; onActivated: badge.fitRequested() }
            ZoomRow { text: qsTr("100%"); shortcut: "⌘ 0"; onActivated: badge.hundredRequested() }
            ZoomRow {
                visible: badge.showFitInk
                text: qsTr("Fit ink")
                onActivated: badge.fitInkRequested()
            }
        }
    }
}
