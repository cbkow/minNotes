// The TOOLBAR row (2.0 chrome re-organisation, 2026-10-18 — §2.14): the third of the three top rows, under the
// files and components rows. MODE-SWITCHED on the left: page mode (the Document view, a table frame, a LIVE card)
// shows the former left rail's format actions flat plus two flyouts (Heading · Insert block); the canvas shows the
// canvas tools; a PDF / video tab its frame tools (both = the embedded CanvasToolbar, which the probes read as
// `editor.toolbarItem`). A SHARED RIGHT END in every mode: the stream chip (only with a QCView stream folder), the
// 3-stack colour swatch (text · highlight · pen → the colour flyout), undo · redo, the zoom badge, the Inspector
// toggle (accent when open — the standing ruling). House style: 28-px buttons in the 30-px row, 16-px icons,
// radius 0, grey + white selection, no accent except the toggle. Every action hands the keys back to the editor.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    required property var editor          // the Editor (null while no document is open)
    required property var inspector       // the Inspector panel object (colour / tool state lives there)
    height: Theme.dim.toolbarH
    color: Theme.colors.surfaceRaised
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border }

    // Probe hooks.
    readonly property var canvasTools: canvasToolsSeg
    readonly property var pageTools: pageToolsRow
    readonly property var headingFlyout: headingBtn
    readonly property var blockFlyout: blockBtn
    readonly property var swatchStack: swatch
    readonly property var zoomBadge: zoom
    readonly property var inspectorToggle: sideBtn
    // "page" | "canvas" | "frame": a live card is a page (its keys and clicks belong to the editor).
    readonly property string mode: !editor ? "" : (editor.cardHosted || canvasToolsSeg.view === "") ? "page" : canvasToolsSeg.view
    readonly property bool pageMode: mode === "page"

    function act(fn) { fn(); if (editor) editor.forceActiveFocus() }   // run an action, then the keys go back to the editor
    function openColors(target) {   // the swatch stack → the colour flyout
        if (!inspector) return
        inspector.target = target
        if (!colors.visible) colors.open()
    }
    readonly property var colorFlyout: colors
    readonly property var penFlyout: pen   // probe

    component ToolBtn: FlatButton {
        anchors.verticalCenter: parent.verticalCenter
        implicitWidth: Theme.dim.chromeBtn; implicitHeight: Theme.dim.chromeBtn
        iconSize: Theme.dim.chromeIcon; radius: 0; tooltipSide: "bottom"
    }
    component Sep: Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 1; height: 18; color: Theme.colors.border }
    // A colour tool: the flat chassis with a colour underbar (the highlight's current colour).
    component ColorTool: ToolBtn {
        property color underColor: "transparent"
        Rectangle { anchors.bottom: parent.bottom; anchors.bottomMargin: 3; anchors.horizontalCenter: parent.horizontalCenter
                    width: 14; height: 2; radius: 0; color: parent.underColor }
    }
    // One option — the rail's recipe: `item`: { icon, tip, isChecked?(caretType, caretLevel), act(), noRefocus? }.
    component OptionBtn: ToolBtn {
        property var item: ({})
        signal picked()
        anchors.verticalCenter: undefined   // (lives in the flyout's Grid, not a Row)
        iconName: item.icon || ""
        tooltip: item.tip || ""
        checked: {
            if (!bar.editor || !item.isChecked) return false
            const t = bar.editor.caretType, lv = bar.editor.caretLevel   // reactive deps
            return item.isChecked(t, lv)
        }
        onClicked: { if (item.noRefocus) item.act(); else bar.act(function() { item.act() }); picked() }
    }
    // A labelled option row (the Insert-block flyout: the eight kinds read better with their names).
    component OptionRow: Rectangle {
        id: orow
        property var item: ({})
        signal picked()
        width: 168; height: 26; radius: 0
        readonly property bool isChecked: {
            if (!bar.editor || !item.isChecked) return false
            const t = bar.editor.caretType, lv = bar.editor.caretLevel
            return item.isChecked(t, lv)
        }
        color: orowMA.containsMouse ? Theme.colors.surfaceHover : (isChecked ? Theme.colors.divider : "transparent")
        Row {
            anchors.fill: parent; anchors.leftMargin: 8; spacing: 8
            Icon { anchors.verticalCenter: parent.verticalCenter; name: orow.item.icon || ""; size: 16
                   color: orow.isChecked || orowMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted }
            Text { anchors.verticalCenter: parent.verticalCenter; text: orow.item.tip || ""
                   color: orow.isChecked || orowMA.containsMouse ? Theme.colors.textBright : Theme.colors.text
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome }
        }
        MouseArea { id: orowMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: { if (orow.item.noRefocus) orow.item.act(); else bar.act(function() { orow.item.act() }); orow.picked() } }
    }
    // A flyout button: the tool button with a small caret; click opens a Popup of options under it (click-to-open —
    // the rail's hover flyout is the ancestor). Keeps the keys with the editor: no focus of its own.
    component FlyoutBtn: ToolBtn {
        id: fb
        property var items: []
        property bool labelled: false     // true = a column of OptionRows; false = a row of OptionBtns
        readonly property bool isOpen: flyout.visible
        readonly property var popupItem: flyout
        implicitWidth: Theme.dim.chromeBtn + 8
        function itemAt(i) { return fb.labelled ? rowsRep.itemAt(i) : btnsRep.itemAt(i) }
        Icon { anchors.right: parent.right; anchors.rightMargin: 2; anchors.verticalCenter: parent.verticalCenter
               name: "caret-down"; size: 9; color: Theme.colors.textSubtle }
        onClicked: flyout.visible ? flyout.close() : flyout.open()
        Popup {
            id: flyout
            parent: fb
            x: 0; y: fb.height + 2
            padding: 4
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            modal: true; dim: false   // a press outside CLOSES the flyout and goes no further (never through to the page)
            background: Rectangle { color: Theme.colors.surface; radius: 0; border.width: 1; border.color: Theme.colors.border }
            contentItem: Grid {
                columns: fb.labelled ? 1 : 99
                spacing: 2
                Repeater { id: btnsRep; model: fb.labelled ? [] : fb.items
                           delegate: OptionBtn { required property var modelData; item: modelData; onPicked: flyout.close() } }
                Repeater { id: rowsRep; model: fb.labelled ? fb.items : []
                           delegate: OptionRow { required property var modelData; item: modelData; onPicked: flyout.close() } }
            }
            onClosed: if (bar.editor) bar.editor.forceActiveFocus()
        }
    }

    // ---------- LEFT: the mode's tools ----------
    Row {
        id: leftRow
        anchors.left: parent.left; anchors.leftMargin: 6; anchors.verticalCenter: parent.verticalCenter
        height: parent.height
        spacing: 2
        // The canvas / frame tools (the probes' `toolbarItem`): hidden in page mode and while a card is live.
        CanvasToolbar {
            id: canvasToolsSeg
            editor: bar.editor
            visible: view !== "" && !(editor && editor.cardHosted)
            height: bar.height
        }
        Row {
            id: pageToolsRow
            visible: bar.pageMode
            height: parent.height
            spacing: 2
            ToolBtn { iconName: "paragraph"; tooltip: qsTr("Paragraph")
                      checked: !!bar.editor && bar.editor.caretType === 0
                      onClicked: bar.act(function() { bar.editor.setParagraph() }) }
            ToolBtn { text: "B"; boldLabel: true; tooltip: qsTr("Bold  (⌘B)")
                      checked: !!bar.editor && bar.editor.boldArmed
                      onClicked: bar.act(function() { bar.editor.applyFormat("bold") }) }
            ToolBtn { iconName: "text-italic"; tooltip: qsTr("Italic  (⌘I)")
                      checked: !!bar.editor && bar.editor.italicArmed
                      onClicked: bar.act(function() { bar.editor.applyFormat("italic") }) }
            ColorTool { iconName: "highlighter"; tooltip: qsTr("Highlight")
                        checked: !!bar.editor && bar.editor.highlightArmed
                        underColor: bar.inspector ? bar.inspector.bgColor : "#FFEC59"
                        onClicked: if (bar.editor && bar.inspector) bar.act(function() { bar.editor.toggleHighlight(bar.inspector.bgColor) }) }
            ToolBtn { iconName: "code"; tooltip: qsTr("Code")
                      checked: !!bar.editor && bar.editor.codeArmed
                      onClicked: bar.act(function() { bar.editor.applyFormat("code") }) }
            ToolBtn { iconName: "text-strikethrough"; tooltip: qsTr("Strikethrough  (⌘⇧X)")
                      checked: !!bar.editor && bar.editor.strikeArmed
                      onClicked: bar.act(function() { bar.editor.applyFormat("strike") }) }
            ToolBtn { iconName: "text-underline"; tooltip: qsTr("Underline  (⌘U)")
                      checked: !!bar.editor && bar.editor.underlineArmed
                      onClicked: bar.act(function() { bar.editor.applyFormat("underline") }) }
            ToolBtn { iconName: "link"; tooltip: qsTr("Link  (⌘K)")
                      checked: !!bar.editor && bar.editor.linkActive
                      onClicked: if (bar.editor) bar.editor.applyLink() }   // direct: a refocus would steal the URL field's focus
            ToolBtn { iconName: "text-t-slash"; tooltip: qsTr("Clear formatting  (⌘\\)")
                      onClicked: bar.act(function() { bar.editor.clearFormatting() }) }
            Sep {}
            FlyoutBtn {   // Heading: the icon shows the caret's level; lit inside a heading
                id: headingBtn
                tooltip: qsTr("Heading")
                readonly property int level: bar.editor && bar.editor.caretType === 1 ? bar.editor.caretLevel : 0
                iconName: ["text-h", "text-h-one", "text-h-two", "text-h-three", "text-h-four", "text-h-five"][Math.max(0, Math.min(5, level))]
                checked: level > 0
                items: [
                    { icon: "text-h-one",   tip: qsTr("Heading 1"), isChecked: function(t, lv){ return t === 1 && lv === 1 }, act: function(){ bar.editor.setHeading(1) } },
                    { icon: "text-h-two",   tip: qsTr("Heading 2"), isChecked: function(t, lv){ return t === 1 && lv === 2 }, act: function(){ bar.editor.setHeading(2) } },
                    { icon: "text-h-three", tip: qsTr("Heading 3"), isChecked: function(t, lv){ return t === 1 && lv === 3 }, act: function(){ bar.editor.setHeading(3) } },
                    { icon: "text-h-four",  tip: qsTr("Heading 4"), isChecked: function(t, lv){ return t === 1 && lv === 4 }, act: function(){ bar.editor.setHeading(4) } },
                    { icon: "text-h-five",  tip: qsTr("Heading 5"), isChecked: function(t, lv){ return t === 1 && lv === 5 }, act: function(){ bar.editor.setHeading(5) } }
                ]
            }
            FlyoutBtn {   // Insert block: the eight kinds, labelled
                id: blockBtn
                tooltip: qsTr("Insert block")
                iconName: "squares-four"
                labelled: true
                checked: !!bar.editor && (bar.editor.caretType === 2 || bar.editor.caretType === 4 || bar.editor.caretType === 5
                                          || bar.editor.caretType === 8 || bar.editor.caretType === 9)
                items: [
                    { icon: "quotes",            tip: qsTr("Quote"),         isChecked: function(t){ return t === 4 }, act: function(){ bar.editor.toggleBlock(4) } },
                    { icon: "list-bullets",      tip: qsTr("Bullet list"),   isChecked: function(t){ return t === 5 }, act: function(){ bar.editor.toggleBlock(5) } },
                    { icon: "list-numbers",      tip: qsTr("Numbered list"), isChecked: function(t){ return t === 9 }, act: function(){ bar.editor.toggleBlock(9) } },
                    { icon: "check-square",      tip: qsTr("Task list"),     isChecked: function(t){ return t === 8 }, act: function(){ bar.editor.toggleBlock(8) } },
                    { icon: "caret-circle-down", tip: qsTr("Choice chip  (⌥⌘C)"), noRefocus: true, act: function(){ bar.editor.insertChoiceChip() } },
                    { icon: "code-block",        tip: qsTr("Code block"),    isChecked: function(t){ return t === 2 }, act: function(){ bar.editor.toggleCodeBlock() } },
                    { icon: "table",             tip: qsTr("Table"),         act: function(){ bar.editor.insertTableAtCaret() } },
                    { icon: "minus",             tip: qsTr("Divider"),       act: function(){ bar.editor.addDivider() } }
                ]
            }
        }
    }

    // ---------- RIGHT: shared by every mode ----------
    VideoNotesModel { id: streamNotes; sidecarDir: docs.activeStreamDir }   // a QCView stream folder's notes
    Row {
        id: rightRow
        anchors.right: parent.right; anchors.rightMargin: 6; anchors.verticalCenter: parent.verticalCenter
        height: parent.height
        spacing: 4
        ToolBtn {   // the stream chip: insert the stream's notes as blocks at the end
            visible: docs.activeStreamDir.length > 0
            iconName: "download-simple"
            tooltip: (streamNotes.streamName.length ? streamNotes.streamName : qsTr("stream")) + "  ·  " + streamNotes.count
                     + (streamNotes.count === 1 ? qsTr(" note") : qsTr(" notes")) + qsTr(" — insert as blocks at the end")
            enabled_: streamNotes.count > 0 && !!bar.editor
            onClicked: bar.act(function() { streamNotes.insertNotesAsBlocks(blockModel, blockModel.count - 1) })
        }
        ToolBtn {   // the PEN WIDTH: a line as thick as the pen (scaled), its own flyout (walk 7d)
            id: penBtn
            tooltip: qsTr("Pen width") + " (" + Math.round(bar.inspector ? bar.inspector.drawWidth : 6) + " px)"
            checked: pen.visible
            Rectangle {
                anchors.centerIn: parent
                width: 16; height: Math.max(1, Math.min(9, Math.round((bar.inspector ? bar.inspector.drawWidth : 6) / 2.5))); radius: 0
                color: penBtn.checked ? Theme.colors.textBright : Theme.colors.textMuted
            }
            onClicked: pen.visible ? pen.close() : pen.open()
            PenFlyout { id: pen; parent: penBtn; inspector: bar.inspector; editor: bar.editor }
        }
        Item {   // the 3-CLIP swatch: text · highlight · pen as VERTICAL clips side by side (user, 2026-10-18); the target's clip is bright-bordered
            id: swatch
            anchors.verticalCenter: parent.verticalCenter
            width: 82; height: Theme.dim.chromeBtn
            readonly property var layers: [ { key: "fg", tip: qsTr("Text colour") }, { key: "bg", tip: qsTr("Highlight colour") }, { key: "draw", tip: qsTr("Pen colour") } ]
            function colorOf(key) { return !bar.inspector ? "transparent" : key === "fg" ? bar.inspector.fgColor : key === "bg" ? bar.inspector.bgColor : bar.inspector.drawColor }
            function layerAt(i) { return layerRep.itemAt(i) }
            ColorFlyout { id: colors; parent: swatch; inspector: bar.inspector }
            Row {
                anchors.centerIn: parent
                spacing: 2
                Repeater {
                    id: layerRep
                    model: swatch.layers
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: 24; height: 16; radius: 0   // WIDE clips (user, 2026-10-18: twice as wide at least)
                        color: swatch.colorOf(modelData.key)
                        // No selection state on the clips (user, 2026-10-18): all three at full strength; the flyout's tabs show the target.
                        MouseArea { id: layerMA; anchors.fill: parent; anchors.margins: -1; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: bar.openColors(modelData.key) }
                        FlatToolTip { parent: parent; x: -implicitWidth / 2 + 12; y: parent.height + 8; visible: layerMA.containsMouse; text: modelData.tip }
                    }
                }
            }
        }
        ToolBtn { iconName: "arrow-counter-clockwise"; tooltip: qsTr("Undo  (⌘Z)")
                  enabled_: !!bar.editor && bar.editor.undoTarget.canUndo   // routes by focus: the canvas log on the canvas
                  onClicked: bar.act(function() { bar.editor.undoTarget.undo() }) }
        ToolBtn { iconName: "arrow-clockwise"; tooltip: qsTr("Redo  (⌘⇧Z)")
                  enabled_: !!bar.editor && bar.editor.undoTarget.canRedo
                  onClicked: bar.act(function() { bar.editor.undoTarget.redo() }) }
        ZoomBadge {   // the readout + Fit / Full width / 100 % menu (opens downward now)
            id: zoom
            anchors.verticalCenter: parent.verticalCenter
            zoomValue: bar.editor ? bar.editor.zoomValue : 1
            fitLabel: bar.editor && (bar.editor.pdfTabOpen || bar.editor.activeCanvas) ? qsTr("Fit") : qsTr("Full width")
            fitChecked: !!bar.editor && !bar.editor.pdfTabOpen && !bar.editor.activeCanvas && bar.editor.fullWidth
            sliderPos: bar.editor ? bar.editor.zoomSliderPos : 0.5
            onSliderMoved: (p) => { if (bar.editor) bar.editor.zoomSliderTo(p) }
            onFitRequested: bar.act(function() { bar.editor.zoomFit() })
            onHundredRequested: bar.act(function() { bar.editor.zoom100() })
        }
        ToolBtn {   // the Inspector toggle: the arrow points where the panel will go; accent when open (ruling 2026-07-05)
            id: sideBtn
            iconName: bar.inspector && bar.inspector.open ? "arrow-line-right" : "arrow-line-left"
            tooltip: bar.inspector && bar.inspector.open ? qsTr("Close the Inspector") : qsTr("Open the Inspector")
            checked: !!bar.inspector && bar.inspector.open
            checkedColor: Theme.colors.accent
            iconColor: checked ? Theme.colors.textBright : "transparent"
            onClicked: if (bar.inspector) bar.inspector.open = !bar.inspector.open
        }
    }
}
