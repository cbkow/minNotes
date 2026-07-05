// LeftRail — the vertical action rail down the left edge. Built from the
// family's FlatButton/Icon chassis (Phosphor icons), laid out vertically so it
// can grow collapsible labelled SECTIONS later without fighting a top bar.
//
// v1 groups: History (undo/redo) and Format (bold/italic/code/clear). Buttons
// act on the Editor's model-owned cursor, so clicking the rail never loses the
// selection; focus is handed back to the editor after each action.

import QtQuick
import QtQuick.Controls
import QtCore

Rectangle {
    id: rail
    required property var editor      // the Editor instance to act on
    property var inspector: null      // the right inspector panel this rail toggles

    width: 46
    color: Theme.colors.surface

    // Buttons inset 1px each side so they nestle between (not over) the edges /
    // the right hairline.
    readonly property int btnWidth: width - 2

    // Per-section collapse state (persisted). Collapsed = a single icon whose
    // hover reveals the options as a flyout menu; expanded = the options inline.
    Settings {
        id: railState
        category: "rail"
        property bool headingsCollapsed: true    // compact by default (avoids rail overflow); expands + persists
        property bool blocksCollapsed: false
    }

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
        // A colour tool: the rail's flat-button chassis with a colour underbar
        // showing the current text / highlight colour. Click applies it; the
        // colours themselves are chosen in the inspector (the palette button below).
        component ColorTool: RailBtn {
            property color underColor: "transparent"
            Rectangle {
                anchors.bottom: parent.bottom; anchors.bottomMargin: 3
                anchors.horizontalCenter: parent.horizontalCenter
                width: 18; height: 3; radius: 1.5; color: parent.underColor
            }
        }

        // One option button — shared by the inline list AND the collapsed flyout.
        // `item`: { icon, tip, isChecked?(caretType, caretLevel), act() }.
        component RailOptionBtn: RailBtn {
            property var item: ({})
            signal picked()
            iconName: item.icon || ""
            tooltip: item.tip || ""
            enabled_: !!rail.editor
            checked: {
                if (!rail.editor || !item.isChecked) return false
                var t = rail.editor.caretType, lv = rail.editor.caretLevel   // reactive deps
                return item.isChecked(t, lv)
            }
            onClicked: { rail.act(function() { item.act() }); picked() }
        }

        // A rail group that's either EXPANDED (header + options inline) or
        // COLLAPSED (just the header icon, whose HOVER reveals the options as a
        // flyout menu). Collapsing folds options into the menu — never hides them.
        component RailSection: Column {
            id: section
            property string repIcon: ""
            property string repTooltip: ""
            property bool repChecked: false
            property var items: []
            property bool collapsed: false
            signal toggleCollapsed()
            width: rail.btnWidth
            spacing: 2

            // Flyout open/close with a small grace so moving button→menu doesn't drop it.
            readonly property bool hoverActive: repBtn.hovered || flyoutHover.hovered
            property bool flyoutOpen: false
            onHoverActiveChanged: { if (hoverActive && collapsed) { flyoutOpen = true; flyoutGuard.stop() }
                                    else if (!hoverActive) flyoutGuard.restart() }
            onCollapsedChanged: if (!collapsed) flyoutOpen = false
            Timer { id: flyoutGuard; interval: 180; onTriggered: section.flyoutOpen = false }

            // EXPANDED header: just the slim collapse caret (no full button).
            RailChevron {
                visible: !section.collapsed
                iconName: "caret-down"
                tooltip: section.repTooltip
                onClicked: section.toggleCollapsed()
            }
            // COLLAPSED header: the full rep icon button + hover flyout of options.
            RailBtn {
                id: repBtn
                visible: section.collapsed
                iconName: section.repIcon
                tooltip: ""                       // the flyout is the affordance
                enabled_: !!rail.editor
                checked: section.repChecked
                onClicked: section.toggleCollapsed()
                Icon {
                    anchors.right: parent.right; anchors.rightMargin: 3
                    anchors.verticalCenter: parent.verticalCenter
                    name: "caret-right"; size: 9; color: Theme.colors.textSubtle
                }
                Popup {
                    id: flyout
                    parent: repBtn
                    x: repBtn.width; y: 0
                    padding: 4
                    closePolicy: Popup.NoAutoClose
                    visible: section.collapsed && section.flyoutOpen
                    background: Rectangle { color: Theme.colors.surface; radius: 0
                                            border.width: 1; border.color: Theme.colors.border }
                    contentItem: Column {
                        spacing: 2
                        HoverHandler { id: flyoutHover }
                        Repeater {
                            model: section.items
                            delegate: RailOptionBtn {
                                required property var modelData
                                item: modelData
                                onPicked: section.flyoutOpen = false
                            }
                        }
                    }
                }
            }
            // EXPANDED: the options inline, under the caret.
            Column {
                width: parent.width; spacing: 2
                visible: !section.collapsed
                Repeater {
                    model: section.items
                    delegate: RailOptionBtn { required property var modelData; item: modelData }
                }
            }
        }

        // ── Palette (top): opens the colour picker. Colours are applied LIVE to
        // the selection by picking in the palette — no separate apply button. ──
        RailBtn {
            iconName: "palette"; tooltip: "Palette"
            enabled_: !!rail.editor
            checked: !!rail.inspector && rail.inspector.open
            onClicked: if (rail.inspector) rail.inspector.open = !rail.inspector.open
        }
        // Annotation mode: draw block-pinned margin ink over the document.
        // Locks the page to 760 (pan reaches the margins) and floats the
        // Inspector; Esc exits. Toggling on opens the Inspector's Draw tools.
        RailBtn {
            iconName: "pen-nib"; tooltip: "Annotate"
            enabled_: !!rail.editor
            checked: !!rail.editor && rail.editor.inkMode
            onClicked: rail.editor.setInkMode(!rail.editor.inkMode)
        }

        RailSep {}

        // ── Format (act on the selection) ──
        // Paragraph: reset the block to plain body text. Lit when the block is
        // already a paragraph (the "nothing else" state). First, as the base style.
        RailBtn {
            iconName: "paragraph"; tooltip: "Paragraph"
            enabled_: !!rail.editor
            checked: !!rail.editor && rail.editor.caretType === 0
            onClicked: rail.act(function() { rail.editor.setParagraph() })
        }
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
        // Highlight toggle — applies/removes the palette's highlight colour on the
        // selection (or arms it for typing); the underbar shows the colour.
        ColorTool {
            iconName: "highlighter"; tooltip: "Highlight"
            enabled_: !!rail.editor
            checked: !!rail.editor && rail.editor.highlightArmed
            underColor: rail.inspector ? rail.inspector.bgColor : "#FFEC59"
            onClicked: if (rail.editor && rail.inspector) rail.act(function() { rail.editor.toggleHighlight(rail.inspector.bgColor) })
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

        RailSep {}

        // ── Headings (H1–H5). Inline by default; collapse → an "H" icon whose
        // hover reveals the levels as a flyout. ──
        RailSection {
            repIcon: "text-h"; repTooltip: "Headings"
            collapsed: railState.headingsCollapsed
            repChecked: !!rail.editor && rail.editor.caretType === 1
            onToggleCollapsed: railState.headingsCollapsed = !railState.headingsCollapsed
            items: [
                { icon: "text-h-one",   tip: "Heading 1", isChecked: function(t, lv){ return t === 1 && lv === 1 }, act: function(){ rail.editor.setHeading(1) } },
                { icon: "text-h-two",   tip: "Heading 2", isChecked: function(t, lv){ return t === 1 && lv === 2 }, act: function(){ rail.editor.setHeading(2) } },
                { icon: "text-h-three", tip: "Heading 3", isChecked: function(t, lv){ return t === 1 && lv === 3 }, act: function(){ rail.editor.setHeading(3) } },
                { icon: "text-h-four",  tip: "Heading 4", isChecked: function(t, lv){ return t === 1 && lv === 4 }, act: function(){ rail.editor.setHeading(4) } },
                { icon: "text-h-five",  tip: "Heading 5", isChecked: function(t, lv){ return t === 1 && lv === 5 }, act: function(){ rail.editor.setHeading(5) } }
            ]
        }

        RailSep {}

        // ── Blocks. Inline by default; collapse → a blocks icon whose hover
        // reveals the options as a flyout. Quote/list/task/code toggle the caret
        // block; table/sketch/divider insert. ──
        RailSection {
            repIcon: "squares-four"; repTooltip: "Blocks"
            collapsed: railState.blocksCollapsed
            repChecked: !!rail.editor && (rail.editor.caretType === 2 || rail.editor.caretType === 4
                                          || rail.editor.caretType === 5 || rail.editor.caretType === 8
                                          || rail.editor.caretType === 9)
            onToggleCollapsed: railState.blocksCollapsed = !railState.blocksCollapsed
            items: [
                { icon: "quotes",        tip: "Quote",      isChecked: function(t){ return t === 4 }, act: function(){ rail.editor.toggleBlock(4) } },
                { icon: "list-bullets",  tip: "Bullet list",isChecked: function(t){ return t === 5 }, act: function(){ rail.editor.toggleBlock(5) } },
                { icon: "list-numbers",  tip: "Numbered list", isChecked: function(t){ return t === 9 }, act: function(){ rail.editor.toggleBlock(9) } },
                { icon: "check-square",  tip: "Task list",  isChecked: function(t){ return t === 8 }, act: function(){ rail.editor.toggleBlock(8) } },
                { icon: "code-block",    tip: "Code block", isChecked: function(t){ return t === 2 }, act: function(){ rail.editor.toggleCodeBlock() } },
                { icon: "table",         tip: "Table",      act: function(){ rail.editor.insertTableAtCaret() } },
                { icon: "scribble",      tip: "Sketch",     act: function(){ rail.editor.insertSketchAtCaret() } },
                { icon: "minus",         tip: "Divider",    act: function(){ rail.editor.addDivider() } }
            ]
        }
    }
}
