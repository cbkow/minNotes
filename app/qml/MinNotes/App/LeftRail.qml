// LeftRail — the vertical action rail down the left edge. Built from the
// family's FlatButton/Icon chassis (Phosphor icons), laid out vertically so it
// can grow collapsible labelled SECTIONS later without fighting a top bar.
//
// v1 groups: History (undo/redo) and Format (bold/italic/code/clear). Buttons
// act on the Editor's model-owned cursor, so clicking the rail never loses the
// selection; focus is handed back to the editor after each action.

import QtQuick
import QtQuick.Controls

Rectangle {
    id: rail
    required property var editor      // the Editor instance to act on

    width: 46
    color: Theme.colors.surface

    // Buttons inset 1px each side so they nestle between (not over) the edges /
    // the right hairline.
    readonly property int btnWidth: width - 2

    // right hairline against the document page
    Rectangle {
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: 1; color: Theme.colors.border
    }

    // run an action, then return focus to the editor so typing continues
    function act(fn) { fn(); if (editor) editor.forceActiveFocus() }

    Column {
        anchors {
            top: parent.top; left: parent.left; right: parent.right
            topMargin: 6; leftMargin: 1; rightMargin: 1
        }
        spacing: 2

        component RailBtn: FlatButton {
            width: rail.btnWidth
            implicitHeight: Theme.dim.toolStripHeight
            iconSize: Theme.icon.sizeToolbar
            radius: 0                              // squared, family flat style
        }
        component RailSep: Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: rail.btnWidth - 12; height: 1
            color: Theme.colors.divider
        }

        // ── History ──
        RailBtn {
            iconName: "arrow-counter-clockwise"; tooltip: "Undo  (⌘Z)"
            enabled_: blockModel.canUndo
            onClicked: rail.act(function() { blockModel.undo() })
        }
        RailBtn {
            iconName: "arrow-clockwise"; tooltip: "Redo  (⌘⇧Z)"
            enabled_: blockModel.canRedo
            onClicked: rail.act(function() { blockModel.redo() })
        }

        RailSep {}

        // ── Format (act on the selection) ──
        // Bold/italic/code: with a selection they apply to it; with no selection
        // they're a Word-style toggle (lit = armed for the next typing).
        RailBtn {
            text: "B"; boldLabel: true; tooltip: "Bold  (⌘B)"
            enabled_: !!rail.editor
            checked: !!rail.editor && rail.editor.boldArmed
            onClicked: rail.act(function() { rail.editor.applyFormat("bold") })
        }
        RailBtn {
            iconName: "text-italic"; tooltip: "Italic  (⌘I)"
            enabled_: !!rail.editor
            checked: !!rail.editor && rail.editor.italicArmed
            onClicked: rail.act(function() { rail.editor.applyFormat("italic") })
        }
        RailBtn {
            iconName: "code"; tooltip: "Code"
            enabled_: !!rail.editor
            checked: !!rail.editor && rail.editor.codeArmed
            onClicked: rail.act(function() { rail.editor.applyFormat("code") })
        }
        RailBtn {
            iconName: "text-t-slash"; tooltip: "Clear formatting  (⌘\\)"
            enabled_: !!rail.editor       // acts on the caret block; no selection needed
            onClicked: rail.act(function() { rail.editor.clearFormatting() })
        }

        RailSep {}

        // ── Headings (act on the caret's block; click active level to toggle off) ──
        Repeater {
            model: 5
            delegate: FlatButton {
                required property int index
                readonly property int level: index + 1
                width: rail.btnWidth
                implicitHeight: Theme.dim.toolStripHeight
                iconSize: Theme.icon.sizeToolbar
                radius: 0
                iconName: ["text-h-one", "text-h-two", "text-h-three", "text-h-four", "text-h-five"][index]
                tooltip: "Heading " + level
                checked: !!rail.editor && rail.editor.caretType === 1 && rail.editor.caretLevel === level
                onClicked: rail.act(function() { rail.editor.setHeading(level) })
            }
        }
    }
}
