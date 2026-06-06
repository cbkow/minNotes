// BottomRail — the status strip along the bottom: the document's file name +
// path on the left, undo/redo flush right (moved off the left rail to free room).

import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    required property var editor       // for returning focus after an action

    height: 28
    color: Theme.colors.surface

    readonly property string docPath: blockModel.documentPath
    readonly property string fileName: docPath.length ? docPath.substring(docPath.lastIndexOf("/") + 1) : "untitled"
    readonly property string dir: docPath.length ? docPath.substring(0, docPath.lastIndexOf("/")) : ""

    // top hairline against the document page
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 1; color: Theme.colors.border
    }

    function act(fn) { fn(); if (editor) editor.forceActiveFocus() }

    // file name + path (left)
    Row {
        anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter; right: actions.left; rightMargin: 10 }
        spacing: 8
        Text {
            text: bar.fileName; color: Theme.colors.text
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            width: Math.min(implicitWidth, parent.width - 120)
            text: bar.dir; color: Theme.colors.textSubtle
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            elide: Text.ElideMiddle
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    // undo / redo (flush right)
    Row {
        id: actions
        anchors { right: parent.right; rightMargin: 6; verticalCenter: parent.verticalCenter }
        spacing: 2
        FlatButton {
            iconName: "arrow-counter-clockwise"; tooltip: "Undo  (⌘Z)"; tooltipSide: "top"
            implicitHeight: 24; implicitWidth: 30; iconSize: 15; radius: 0
            enabled_: blockModel.canUndo
            onClicked: bar.act(function() { blockModel.undo() })
        }
        FlatButton {
            iconName: "arrow-clockwise"; tooltip: "Redo  (⌘⇧Z)"; tooltipSide: "top"
            implicitHeight: 24; implicitWidth: 30; iconSize: 15; radius: 0
            enabled_: blockModel.canRedo
            onClicked: bar.act(function() { blockModel.redo() })
        }
    }
}
