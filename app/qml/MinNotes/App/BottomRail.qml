// BottomRail — the status strip along the bottom: the document's file name +
// path on the left, the zoom group and undo/redo flush right (moved off the left rail to free
// room). The zoom group is THE zoom interface (ruling 2026-09-16): −, a Word-style slider
// (100 % dead centre), +, and the percent readout that opens Fit / 100 % — all on
// the ACTIVE surface: the document, a PDF tab or the canvas.

import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    required property var editor       // for returning focus after an action

    height: 28
    color: Theme.colors.surfaceRaised   // sidebar tone — the bottom strips read as chrome, like the Inspector

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
        Text {   // File ▸ Page breadcrumb (2.0): the focused page's title when the file has several pages
            visible: canvas.documentOpen && canvas.pageIds.length > 1 && canvas.focusPage !== ""
            text: (canvas.pagesRevision, blockModel.contentRevision, "▸ " + canvas.pageTitle(canvas.focusPage))
            color: Theme.colors.text
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

    // QCView stream notes (plan C): the folder this document came from. The
    // notes QCView adds while the feed runs land in the folder; one click
    // brings them in as blocks (image + ink + caption) below the document.
    VideoNotesModel {
        id: streamNotes
        sidecarDir: docs.activeStreamDir
    }
    Row {
        id: streamGroup
        anchors { right: zoomGroup.left; rightMargin: 14; verticalCenter: parent.verticalCenter }
        spacing: 8
        visible: docs.activeStreamDir.length > 0
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: (streamNotes.streamName.length ? streamNotes.streamName : "stream")
                  + "  ·  " + streamNotes.count + (streamNotes.count === 1 ? " note" : " notes")
            color: Theme.colors.textSubtle
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }
        FlatButton {
            iconName: "download-simple"; text: "Insert notes"; tooltip: "Insert this stream's notes as blocks at the end"; tooltipSide: "top"
            implicitHeight: 24; radius: 0
            enabled_: streamNotes.count > 0
            onClicked: bar.act(function() { streamNotes.insertNotesAsBlocks(blockModel, blockModel.count - 1) })
        }
    }

    // zoom group (left of undo / redo)
    Row {
        id: zoomGroup
        anchors { right: actions.left; rightMargin: 14; verticalCenter: parent.verticalCenter }
        spacing: 2
        visible: blockModel.documentOpen
        FlatButton {
            iconName: "minus"; tooltip: "Zoom out  (⌘−)"; tooltipSide: "top"
            implicitHeight: 24; implicitWidth: 24; iconSize: 13; radius: 0
            onClicked: bar.act(function() { bar.editor.zoomStep(-1) })
        }
        FlatSlider {
            width: 96; height: 24
            anchors.verticalCenter: parent.verticalCenter
            from: 0; to: 1
            value: bar.editor ? bar.editor.zoomSliderPos : 0.5
            fillColor: Theme.colors.textSubtle          // chrome, not accent (rationed)
            onMoved: if (bar.editor) bar.editor.zoomSliderTo(value)
            Rectangle {   // the 100 % centre tick
                x: parent.leftPadding + parent.availableWidth / 2 - 0.5; y: parent.height / 2 - 5
                width: 1; height: 10; color: Theme.colors.textSubtle
            }
        }
        FlatButton {
            iconName: "plus"; tooltip: "Zoom in  (⌘+)"; tooltipSide: "top"
            implicitHeight: 24; implicitWidth: 24; iconSize: 13; radius: 0
            onClicked: bar.act(function() { bar.editor.zoomStep(1) })
        }
        ZoomBadge {   // the readout + Fit / 100 % menu (opens upward)
            anchors.verticalCenter: parent.verticalCenter
            zoomValue: bar.editor ? bar.editor.zoomValue : 1
            fitLabel: bar.editor && (bar.editor.pdfTabOpen || bar.editor.activeCanvas) ? qsTr("Fit") : qsTr("Full width")
            fitChecked: !!bar.editor && !bar.editor.pdfTabOpen && !bar.editor.activeCanvas && bar.editor.fullWidth
            onFitRequested: bar.act(function() { bar.editor.zoomFit() })
            onHundredRequested: bar.act(function() { bar.editor.zoom100() })
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
            enabled_: !!bar.editor && bar.editor.undoTarget.canUndo   // routes by focus: the canvas log on the canvas
            onClicked: bar.act(function() { bar.editor.undoTarget.undo() })
        }
        FlatButton {
            iconName: "arrow-clockwise"; tooltip: "Redo  (⌘⇧Z)"; tooltipSide: "top"
            implicitHeight: 24; implicitWidth: 30; iconSize: 15; radius: 0
            enabled_: !!bar.editor && bar.editor.undoTarget.canRedo
            onClicked: bar.act(function() { bar.editor.undoTarget.redo() })
        }
    }
}
