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
    property var inspector: null      // the right inspector panel this rail toggles

    width: 46
    color: Theme.colors.surface

    // Buttons inset 1px each side so they nestle between (not over) the edges /
    // the right hairline.
    readonly property int btnWidth: width - 2

    // Collapsible-section state (proves the rail-grows-down direction).
    property bool blocksOpen: true

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
        // A section toggle: a short chevron header that collapses/expands the
        // group below it. caret-down = open, caret-right = collapsed.
        component RailChevron: FlatButton {
            width: rail.btnWidth
            implicitHeight: 20
            iconSize: 13
            radius: 0
            iconColor: Theme.colors.textSubtle
        }

        // (Undo/redo moved to the bottom status rail.)
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
            iconName: "text-strikethrough"; tooltip: "Strikethrough  (⌘⇧X)"
            enabled_: !!rail.editor
            checked: !!rail.editor && rail.editor.strikeArmed
            onClicked: rail.act(function() { rail.editor.applyFormat("strike") })
        }
        RailBtn {
            iconName: "text-underline"; tooltip: "Underline  (⌘U)"
            enabled_: !!rail.editor
            checked: !!rail.editor && rail.editor.underlineArmed
            onClicked: rail.act(function() { rail.editor.applyFormat("underline") })
        }
        RailBtn {
            iconName: "link"; tooltip: "Link  (⌘K)"
            enabled_: !!rail.editor
            checked: !!rail.editor && rail.editor.linkActive
            // Direct (not rail.act): act() refocuses the editor, which would steal
            // focus from the URL popup's input field the moment it opens.
            onClicked: if (rail.editor) rail.editor.applyLink()
        }
        RailBtn {
            iconName: "text-t-slash"; tooltip: "Clear formatting  (⌘\\)"
            enabled_: !!rail.editor       // acts on the caret block; no selection needed
            onClicked: rail.act(function() { rail.editor.clearFormatting() })
        }
        // Colours / highlight → toggles the right inspector panel (slides in/out).
        RailBtn {
            iconName: "palette"; tooltip: "Colors"
            enabled_: !!rail.editor
            checked: !!rail.inspector && rail.inspector.open
            onClicked: if (rail.inspector) rail.inspector.open = !rail.inspector.open
        }

        RailSep {}

        // ── Headings → a popout menu (H1–H5). Acts on the caret's block; click the
        // active level to toggle it off. ──
        RailBtn {
            id: headingsBtn
            iconName: "text-h"; tooltip: "Headings"
            enabled_: !!rail.editor
            checked: !!rail.editor && rail.editor.caretType === 1
            onClicked: headingMenu.visible ? headingMenu.close() : headingMenu.open()

            Popup {
                id: headingMenu
                parent: headingsBtn
                x: headingsBtn.width + 2
                y: 0
                padding: 4
                background: Rectangle { color: Theme.colors.surface; border.width: 1; border.color: Theme.colors.border }
                contentItem: Column {
                    spacing: 2
                    Repeater {
                        model: 5
                        delegate: FlatButton {
                            required property int index
                            readonly property int level: index + 1
                            width: rail.btnWidth; implicitHeight: Theme.dim.toolStripHeight
                            iconSize: Theme.icon.sizeToolbar; radius: 0
                            iconName: ["text-h-one", "text-h-two", "text-h-three", "text-h-four", "text-h-five"][index]
                            tooltip: "Heading " + level
                            checked: !!rail.editor && rail.editor.caretType === 1 && rail.editor.caretLevel === level
                            onClicked: { rail.act(function() { rail.editor.setHeading(level) }); headingMenu.close() }
                        }
                    }
                }
            }
        }

        RailSep {}

        // ── Blocks (collapsible). Quote/list toggle the caret block; divider
        // inserts after it. ──
        RailChevron {
            iconName: rail.blocksOpen ? "caret-down" : "caret-right"
            tooltip: "Blocks"
            onClicked: rail.blocksOpen = !rail.blocksOpen
        }
        Column {
            width: rail.btnWidth; spacing: 2
            visible: rail.blocksOpen
            RailBtn {
                iconName: "quotes"; tooltip: "Quote"
                enabled_: !!rail.editor
                checked: !!rail.editor && rail.editor.caretType === 4
                onClicked: rail.act(function() { rail.editor.toggleBlock(4) })
            }
            RailBtn {
                iconName: "list-bullets"; tooltip: "Bullet list"
                enabled_: !!rail.editor
                checked: !!rail.editor && rail.editor.caretType === 5
                onClicked: rail.act(function() { rail.editor.toggleBlock(5) })
            }
            RailBtn {
                iconName: "code-block"; tooltip: "Code block"
                enabled_: !!rail.editor
                checked: !!rail.editor && rail.editor.caretType === 2
                onClicked: rail.act(function() { rail.editor.toggleCodeBlock() })
            }
            RailBtn {
                iconName: "table"; tooltip: "Table"
                enabled_: !!rail.editor
                onClicked: rail.act(function() { rail.editor.insertTableAtCaret() })
            }
            RailBtn {
                iconName: "minus"; tooltip: "Divider"
                enabled_: !!rail.editor
                onClicked: rail.act(function() { rail.editor.addDivider() })
            }
        }
    }

    // ── Quick-apply colour swatches, flush to the bottom edge. Each is a pure,
    // button-sized swatch of the inspector's current text / highlight colour;
    // click applies it to the selection. (Change the colours in the inspector —
    // the palette button above.) ──
    component Swatch: Rectangle {
        id: sw
        property color swatchColor: "white"
        property string tip: ""
        signal activated()
        width: rail.btnWidth
        height: Theme.dim.toolStripHeight
        color: swatchColor
        // hairline so white / near-surface swatches still read as a tile; brightens on hover
        Rectangle {
            anchors.fill: parent; color: "transparent"
            border.width: 1
            border.color: sma.containsMouse ? Theme.colors.textBright : Qt.rgba(1, 1, 1, 0.18)
        }
        MouseArea {
            id: sma; anchors.fill: parent; hoverEnabled: true
            cursorShape: Qt.PointingHandCursor; onClicked: sw.activated()
        }
        FlatToolTip {
            text: sw.tip; visible: sw.tip.length > 0 && sma.containsMouse
            x: sw.width + 6; y: (sw.height - implicitHeight) / 2
        }
    }

    Column {
        anchors {
            bottom: parent.bottom; left: parent.left; right: parent.right
            leftMargin: 1; rightMargin: 1
        }
        spacing: 1
        Swatch {
            swatchColor: rail.inspector ? rail.inspector.fgColor : Theme.colors.textBright
            tip: "Apply text color"
            onActivated: if (rail.editor) rail.act(function() { rail.editor.applyTextColor(rail.inspector.fgColor) })
        }
        Swatch {
            swatchColor: rail.inspector ? rail.inspector.bgColor : "#7a6a36"
            tip: "Apply highlight"
            onActivated: if (rail.editor) rail.act(function() { rail.editor.applyHighlight(rail.inspector.bgColor) })
        }
    }
}
