import QtQuick
import QtQuick.Controls
import QtCore

// The right-side inspector — a collapsible panel that SLIDES in/out (default
// hidden, so the resting view is just the thin left rail + document). The left
// rail's palette button toggles `open`; the editor column reflows to make room.
// It stays open while you select text and apply, never click-away-dismissed.
//
// Today it holds the colour tools (text colour + highlight, an HSV picker with a
// Text/Highlight target toggle, revert). It's built as a real panel — not a
// floating popout — so it has room to grow into a full interface (annotation
// tools, etc.) without obscuring the document.
Rectangle {
    id: panel
    property var editor: null
    // Annotation mode FLOATS the panel (an overlay — the editor column keeps
    // its full width, so opening the palette can't push the locked 760 page).
    // Outside the mode it participates in the push layout via Main.qml's
    // width subtraction.
    readonly property bool floating: !!editor && editor.inkMode === true
    property bool open: true   // open/closed is persisted (see panelStore) and
                               // restored on launch; this is just the pre-restore default
    property bool _ready: false   // gates the slide animation off during restore

    // Persist whether the panel is open, so it reopens in the user's last state.
    Settings {
        id: panelStore
        category: "inspector"
        property bool open: true
        property string view: "palette"
    }
    onOpenChanged: if (_ready) panelStore.open = open

    // Which interface the panel shows: the colour/drawing palette or the
    // comment threads. Persisted like `open`.
    property string view: "palette"
    onViewChanged: if (_ready) panelStore.view = view
    // The expanded thread in the comments view ("" = all collapsed).
    property string openThread: ""
    function showComments(threadId) {
        open = true
        view = "comments"
        openThread = threadId
    }

    // Colour state (the I/O the editor's apply functions read).
    property color fgColor: Theme.colors.text         // default type colour (E4E3E2)
    property color bgColor: "#FFEC59"                 // highlight colour (classic bright yellow)
    property string target: "fg"                       // which colour the picker edits

    // Drawing state (the video studio's annotator reads these; QSettings-
    // backed like QCView's annotation/colorHex). drawTool "" = disarmed.
    property color drawColor: "#FF0000"                // QCView's default stroke red
    property string drawTool: ""                       // freehand|rect|oval|arrow|line|eraser
    property real drawWidth: 6                         // source-pixel stroke width (1..24)
    Settings {
        id: drawStore
        category: "drawing"
        property string colorHex: ""
        property real width: 0
    }
    onDrawColorChanged: drawStore.colorHex = "" + drawColor
    onDrawWidthChanged: drawStore.width = drawWidth

    readonly property int panelW: 248
    readonly property int contentW: panelW - 24   // inside the x:12 margins
    width: open ? panelW : 0
    Behavior on width { enabled: panel._ready; NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    color: Theme.colors.surfaceRaised                  // a step lighter than the page, so the panel reads as its own layer
    clip: true                                         // so content clips cleanly while sliding

    function targetColor() {
        return target === "fg" ? fgColor : target === "bg" ? bgColor : drawColor
    }
    // Picking a colour auto-applies it to the document selection (live). But a
    // PROGRAMMATIC picker.value set (tab switch, init, revert) must NOT — so route
    // those through setPickerValue, which suppresses the apply during the assign.
    property bool _suppressApply: false
    function setPickerValue(v) { _suppressApply = true; picker.value = v; _suppressApply = false }

    // Switching target retargets the picker — deselect first, or the reassign
    // would write the OTHER target's colour into the selected swatch.
    onTargetChanged: { selPreset = -1; selUser = -1
                       setPickerValue(targetColor()) }
    // Reset the picker to defaults AND strip fg/bg colour from the current
    // selection (table cells or text) — "revert to default" means uncoloured.
    function revertTarget() {
        selPreset = -1; selUser = -1                    // (don't drag a swatch back to default)
        if (target === "fg")      fgColor = Theme.colors.text   // default type colour (E4E3E2)
        else if (target === "bg") bgColor = "#FFEC59"
        else                      drawColor = "#FF0000"
        setPickerValue(targetColor())
        if (editor) editor.revertColors()               // strip fg+bg from the selection
    }

    // --- Swatches: a FIXED bright preset grid + a row of user slots. Clicking
    // one selects it and loads its colour. Only user slots are editable: while
    // a user slot is selected, picker edits write back into it (persisted via
    // Settings); presets are immutable — editing while one is selected just
    // deselects it (the swatch no longer matches the picker). Empty user slots
    // capture the current colour on first click.
    readonly property var presets: [
        "#FF5768", "#FF6F68", "#FC6238", "#FFA23A", "#FFBF65", "#FFD872", "#FFEC59", "#CFF800", "#86E36B", "#43E8D8",
        "#4DD091", "#00CDAC", "#8DD7BF", "#00B0BA", "#00A5E3", "#6C88C4", "#C05780", "#FF96C5", "#9D8DF1", "#FF60A8"]
    property var userSlots: ["", "", "", "", "", "", "", "", "", ""]
    property int selPreset: -1
    property int selUser: -1
    Settings {
        id: swatchStore
        category: "swatches"
        property string user: ""
    }
    Component.onCompleted: {
        // Pad/truncate the saved slots to the current count so growing the row
        // doesn't discard a user's existing swatches (they just gain empties).
        try { var u = JSON.parse(swatchStore.user)
              if (u && u.length) { var n = userSlots.slice()
                                   for (var i = 0; i < n.length && i < u.length; i++) n[i] = u[i]
                                   userSlots = n } } catch (e) {}
        if (drawStore.colorHex !== "") drawColor = drawStore.colorHex
        if (drawStore.width > 0) drawWidth = drawStore.width
        open = panelStore.open      // restore last open/closed state (no slide)
        if (panelStore.view === "comments") view = "comments"
        _ready = true               // toggles from here on animate + persist
    }
    function saveSwatches() { swatchStore.user = JSON.stringify(userSlots) }
    function noteEdit(c) {                              // picker moved
        var hex = "" + c
        if (selUser >= 0) { var u = userSlots.slice(); u[selUser] = hex; userSlots = u; saveSwatches() }
        else if (selPreset >= 0 && hex.toLowerCase() !== presets[selPreset].toLowerCase())
            selPreset = -1                              // presets are fixed; a tweak detaches
    }

    // Left hairline against the document (only meaningful while open).
    Rectangle { width: 1; height: parent.height; color: Theme.colors.border }

    // Target toggle (Text / Highlight / Draw) — flat inline buttons (no box
    // border) carrying a colour chip. Active uses the family selection language:
    // divider-grey fill + bright text. Selecting one points the picker AND the
    // Apply button at that colour.
    component Tab: Rectangle {
        property string label: ""
        property string t: ""
        readonly property bool sel: panel.target === t
        width: tabRow.implicitWidth + 20; height: 28
        color: sel ? Theme.colors.divider : (tma.containsMouse ? Theme.colors.surfaceHover : "transparent")
        Row {
            id: tabRow
            anchors.centerIn: parent; spacing: 6
            Rectangle { width: 12; height: 12; radius: 2; anchors.verticalCenter: parent.verticalCenter
                        color: parent.parent.t === "fg" ? panel.fgColor
                             : parent.parent.t === "bg" ? panel.bgColor : panel.drawColor
                        border.width: 1; border.color: Theme.colors.border }
            Text { text: parent.parent.label; anchors.verticalCenter: parent.verticalCenter
                   color: parent.parent.sel ? Theme.colors.textBright : Theme.colors.textMuted
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
        }
        MouseArea { id: tma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: panel.target = parent.t }
    }

    // Fixed-width content anchored to the right edge so it doesn't squish during the
    // slide; the panel's animating width just reveals/hides it.
    Item {
        width: panel.panelW
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }

        // Header: view toggle (Palette | Comments) + close.
        Item {
            id: header
            x: 12; width: parent.width - 24; height: 30; y: 6
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14
                Text { text: "Palette"
                       color: panel.view === "palette" ? Theme.colors.textBright : Theme.colors.textMuted
                       font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                       font.bold: panel.view === "palette"
                       MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                   onClicked: panel.view = "palette" } }
                Text { text: "Comments"
                       color: panel.view === "comments" ? Theme.colors.textBright : Theme.colors.textMuted
                       font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                       font.bold: panel.view === "comments"
                       MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                   onClicked: panel.view = "comments" } }
            }
            FlatButton {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: 24; height: 24; radius: 0; iconName: "x"; iconSize: 13
                tooltip: "Close"; onClicked: panel.open = false
            }
        }

        // --- Comments view: thread cards (anchored first, then Unanchored) ---
        Flickable {
            visible: panel.view === "comments"
            anchors.top: header.bottom; anchors.topMargin: 6; anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            x: 12; width: panel.contentW
            contentHeight: threadsCol.implicitHeight + 8
            clip: true
            Column {
                id: threadsCol
                width: parent.width; spacing: 8
                Text {
                    visible: threadRep.count === 0
                    text: "No comments yet.\nSelect text and use\n“Add comment”."
                    color: Theme.colors.textMuted
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                }
                Repeater {
                    id: threadRep
                    model: (blockModel.commentsRevision, blockModel.contentRevision,
                            blockModel.commentThreads())
                    delegate: Rectangle {
                        id: card
                        required property var modelData
                        readonly property bool expanded: panel.openThread === modelData.id
                        width: threadsCol.width
                        height: cardCol.implicitHeight + 16
                        // Flat recess against the panel. EXPANDED threads
                        // brighten a touch so the reply field's dark recess
                        // stands out inside them; RESOLVED threads fade
                        // halfway toward the panel colour (still readable as
                        // a card, clearly settled) and dim their content.
                        color: modelData.resolved
                               ? Qt.tint(Theme.colors.surfaceRaised,
                                         Qt.rgba(Theme.colors.surface.r, Theme.colors.surface.g,
                                                 Theme.colors.surface.b, 0.5))
                               : card.expanded
                                 ? Qt.tint(Theme.colors.surface, Qt.rgba(1, 1, 1, 0.05))
                                 : Theme.colors.surface
                        Behavior on color { ColorAnimation { duration: 160 } }
                        Column {
                            id: cardCol
                            x: 8; y: 8; width: parent.width - 16; spacing: 6
                            opacity: card.modelData.resolved ? 0.55 : 1   // text fades with the card
                            Behavior on opacity { NumberAnimation { duration: 160 } }
                            // Header: excerpt (or Unanchored) + resolved dot.
                            Item {
                                width: parent.width; height: 16
                                Rectangle {   // resolved indicator
                                    width: 8; height: 8; radius: 4
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: modelData.resolved ? Theme.colors.accent : "transparent"
                                    border.width: 1; border.color: Theme.colors.textMuted
                                }
                                Text {
                                    x: 14; width: parent.width - 14
                                    anchors.verticalCenter: parent.verticalCenter
                                    elide: Text.ElideRight
                                    text: modelData.row >= 0
                                          ? "“" + modelData.excerpt + "”"
                                          : qsTr("(unanchored)")
                                    color: modelData.row >= 0 ? Theme.colors.text : Theme.colors.textMuted
                                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                                    font.italic: modelData.row < 0
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        panel.openThread = card.expanded ? "" : modelData.id
                                        if (modelData.row >= 0 && panel.editor)
                                            panel.editor.ensureVisible(modelData.row)
                                    }
                                }
                            }
                            // Messages + reply + actions, only when expanded.
                            Column {
                                visible: card.expanded
                                width: parent.width; spacing: 6
                                // Each message = a timestamp header + body, with a
                                // hairline between entries — so a thread reads as a
                                // CONVERSATION (comment, then replies), not one blob.
                                Repeater {
                                    model: card.expanded
                                           ? (blockModel.commentsRevision,
                                              blockModel.commentMessages(modelData.id)) : []
                                    delegate: Column {
                                        id: msg
                                        required property int index
                                        required property var modelData
                                        width: cardCol.width
                                        spacing: 2
                                        property bool editing: false
                                        Rectangle {
                                            visible: msg.index > 0
                                            width: parent.width; height: 1
                                            color: Theme.colors.divider
                                        }
                                        // Header: label · timestamp, with hover
                                        // edit/delete controls on the right.
                                        Item {
                                            width: parent.width; height: 16
                                            Text {
                                                anchors.verticalCenter: parent.verticalCenter
                                                text: (msg.index === 0 ? qsTr("Comment") : qsTr("Reply %1").arg(msg.index))
                                                      + " · "
                                                      + new Date(msg.modelData.created)
                                                            .toLocaleString(Qt.locale(), "MMM d, h:mm ap")
                                                color: Theme.colors.textSubtle
                                                font.family: Theme.font.family
                                                font.pixelSize: Theme.font.sizeSmall - 1
                                            }
                                            MouseArea {   // hover scope for the controls
                                                id: msgHover
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                acceptedButtons: Qt.NoButton
                                            }
                                            Row {
                                                anchors.right: parent.right
                                                anchors.verticalCenter: parent.verticalCenter
                                                spacing: 0
                                                visible: msgHover.containsMouse || editMA.containsMouse
                                                         || delMA.containsMouse || msg.editing
                                                Item {
                                                    width: 18; height: 16
                                                    Icon { anchors.centerIn: parent; name: "pencil-simple"; size: 12
                                                           color: editMA.containsMouse ? Theme.colors.textBright
                                                                                       : Theme.colors.textMuted }
                                                    MouseArea { id: editMA; anchors.fill: parent; hoverEnabled: true
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: { msgEdit.text = msg.modelData.body
                                                                             msg.editing = true
                                                                             msgEdit.forceActiveFocus() } }
                                                }
                                                Item {
                                                    width: 18; height: 16
                                                    Icon { anchors.centerIn: parent; name: "trash"; size: 12
                                                           color: delMA.containsMouse ? Theme.colors.textBright
                                                                                      : Theme.colors.textMuted }
                                                    MouseArea { id: delMA; anchors.fill: parent; hoverEnabled: true
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: blockModel.removeCommentMessage(msg.modelData.id) }
                                                }
                                            }
                                        }
                                        Text {
                                            visible: !msg.editing
                                            width: msg.width
                                            wrapMode: Text.Wrap
                                            text: msg.modelData.body
                                            color: Theme.colors.text
                                            font.family: Theme.font.family
                                            font.pixelSize: Theme.font.sizeSmall
                                        }
                                        // Inline editor — Save commits, Cancel (or Esc) reverts.
                                        Column {
                                            visible: msg.editing
                                            width: parent.width; spacing: 4
                                            Rectangle {
                                                width: parent.width
                                                height: Math.max(40, msgEdit.implicitHeight + 12)
                                                color: Theme.colors.bg
                                                TextEdit {
                                                    id: msgEdit
                                                    anchors.fill: parent; anchors.margins: 6
                                                    wrapMode: TextEdit.Wrap
                                                    color: Theme.colors.text
                                                    font.family: Theme.font.family
                                                    font.pixelSize: Theme.font.sizeSmall
                                                    selectByMouse: true
                                                    Keys.onEscapePressed: msg.editing = false
                                                }
                                            }
                                            Row {
                                                spacing: 6
                                                FlatButton {
                                                    text: qsTr("Save"); variant: "primary"; padding: 6
                                                    onClicked: {
                                                        var b = msgEdit.text.trim()
                                                        if (b.length > 0)
                                                            blockModel.updateCommentMessage(msg.modelData.id, b)
                                                        msg.editing = false
                                                    }
                                                }
                                                FlatButton {
                                                    text: qsTr("Cancel"); padding: 6
                                                    onClicked: msg.editing = false
                                                }
                                            }
                                        }
                                    }
                                }
                                // Resolved threads take no new replies — the
                                // field disappears until the thread reopens.
                                Rectangle {
                                    visible: !modelData.resolved
                                    width: parent.width; height: 54
                                    color: Theme.colors.bg   // flat recess, no border
                                    TextEdit {
                                        id: replyEdit
                                        anchors.fill: parent; anchors.margins: 6
                                        wrapMode: TextEdit.Wrap
                                        color: Theme.colors.text
                                        font.family: Theme.font.family
                                        font.pixelSize: Theme.font.sizeSmall
                                        selectByMouse: true
                                    }
                                }
                                Row {
                                    spacing: 6
                                    FlatButton {
                                        visible: !modelData.resolved
                                        text: qsTr("Reply"); padding: 8
                                        onClicked: {
                                            var b = replyEdit.text.trim()
                                            if (b.length > 0) {
                                                blockModel.addCommentMessage(modelData.id, b)
                                                replyEdit.text = ""
                                            }
                                        }
                                    }
                                    FlatButton {
                                        text: modelData.resolved ? qsTr("Reopen") : qsTr("Resolve")
                                        padding: 8
                                        onClicked: blockModel.setThreadResolved(modelData.id, !modelData.resolved)
                                    }
                                    FlatButton {
                                        text: qsTr("Delete"); padding: 8
                                        onClicked: blockModel.deleteThread(modelData.id)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Column {
            visible: panel.view === "palette"
            anchors.top: header.bottom; anchors.topMargin: 6
            x: 12; spacing: 8

            // === Tools (drawing) — above the colour interface ===
            Rectangle { width: panel.contentW; height: 1; color: Theme.colors.divider }
            Text { text: "Tools"
                   color: Theme.colors.textBright
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody; font.bold: true }

            // Select / move — in a sketch tab or the video studio. Active whenever
            // no draw tool is armed.
            Rectangle {
                visible: !!panel.editor && (panel.editor.activeSketchRow >= 0
                                            || panel.editor.activeVideoRow >= 0
                                            || panel.editor.inkMode)
                width: panel.contentW; height: 28
                readonly property bool sel: panel.drawTool === "" || panel.drawTool === "select"
                color: sel ? Theme.colors.divider
                     : (selMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
                Row {
                    anchors.centerIn: parent; spacing: 6
                    Icon { name: "cursor"; size: 15
                           color: parent.parent.sel ? Theme.colors.textBright : Theme.colors.textMuted
                           anchors.verticalCenter: parent.verticalCenter }
                    Text { text: "Select / Move"
                           color: parent.parent.sel ? Theme.colors.textBright : Theme.colors.textMuted
                           font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                           anchors.verticalCenter: parent.verticalCenter }
                }
                MouseArea { id: selMA; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.drawTool = "select" }
            }
            Grid {
                columns: 6; spacing: 4
                Repeater {
                    model: [
                        { tool: "freehand", icon: "scribble",      tip: qsTr("Freehand") },
                        { tool: "rect",     icon: "rectangle",     tip: qsTr("Rectangle") },
                        { tool: "oval",     icon: "circle",        tip: qsTr("Oval") },
                        { tool: "arrow",    icon: "arrow-up-right",tip: qsTr("Arrow") },
                        { tool: "line",     icon: "line-segment",  tip: qsTr("Line") },
                        { tool: "eraser",   icon: "eraser",        tip: qsTr("Eraser") }
                    ]
                    delegate: FlatButton {
                        required property var modelData
                        width: (panel.contentW - 20) / 6; height: width
                        iconName: modelData.icon
                        iconSize: 18
                        checked: panel.drawTool === modelData.tool
                        checkedColor: Theme.colors.divider   // grey — family selection language
                        iconColor: checked ? Theme.colors.textBright : Theme.colors.textMuted
                        tooltip: modelData.tip; tooltipSide: "top"
                        onClicked: panel.drawTool =
                            (panel.drawTool === modelData.tool) ? "" : modelData.tool
                    }
                }
            }
            Row {
                spacing: 8
                Text {
                    text: "Width"
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.colors.textMuted
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                }
                FlatSlider {
                    width: panel.contentW - 80
                    anchors.verticalCenter: parent.verticalCenter
                    from: 1; to: 24
                    value: panel.drawWidth
                    fillColor: Theme.colors.textMuted
                    onMoved: panel.drawWidth = value
                }
                Text {
                    text: Math.round(panel.drawWidth)
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.colors.textSubtle
                    font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
                }
            }

            // === Colors === (section divider with extra padding above the line)
            Item { width: panel.contentW; height: 9
                   Rectangle { width: parent.width; height: 1; anchors.bottom: parent.bottom
                               color: Theme.colors.divider } }
            Text { text: "Colors"
                   color: Theme.colors.textBright
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody; font.bold: true }

            Row {
                spacing: 4
                Tab { label: "Text"; t: "fg" }
                Tab { label: "Highlight"; t: "bg" }
                Tab { label: "Draw"; t: "draw" }
            }

            ColorPickerInline {
                id: picker
                onValueChanged: {
                    if (panel.target === "fg")      panel.fgColor = value
                    else if (panel.target === "bg") panel.bgColor = value
                    else                            panel.drawColor = value
                    panel.noteEdit(value)               // selected swatch tracks the edit
                    // Live auto-apply to the document selection (text / highlight),
                    // unless this is a programmatic value set (tab switch / revert).
                    // Text colour also arms the typing "pen" (pickTextColor).
                    if (!panel._suppressApply && panel.editor) {
                        if (panel.target === "fg")      panel.editor.pickTextColor("" + value)
                        else if (panel.target === "bg") panel.editor.pickHighlight("" + value)
                    }
                }
                Component.onCompleted: panel.setPickerValue(panel.fgColor)   // init: no apply/arm
            }

            // (Apply lives on the left rail's bottom swatches — pick here, apply there.)

            // Revert: soft grey (brighter than the page, not loud).
            Rectangle {
                width: panel.contentW; height: 28
                color: revertMA.containsMouse ? "#3a3a3a" : "#2e2e2e"
                Row {
                    anchors.centerIn: parent; spacing: 6
                    Icon { name: "arrow-counter-clockwise"; size: 14; color: Theme.colors.text
                           anchors.verticalCenter: parent.verticalCenter }
                    Text { text: "Revert to default"; color: Theme.colors.text
                           font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                           anchors.verticalCenter: parent.verticalCenter }
                }
                MouseArea { id: revertMA; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor; onClicked: panel.revertTarget() }
            }

            // --- Preset + user swatches (see the property block up top) ---
            Text { text: "Presets"; color: Theme.colors.textMuted; topPadding: 6
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
            Grid {
                columns: 10; spacing: 4
                Repeater {
                    model: panel.presets.length
                    delegate: Swatch { required property int index; idx: index }
                }
            }
            Text { text: "Yours"; color: Theme.colors.textMuted; topPadding: 2
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
            Row {
                spacing: 4
                Repeater {
                    model: panel.userSlots.length
                    delegate: Swatch { required property int index; idx: index; userSlot: true }
                }
            }
        }
    }

    // One swatch. Selected = white border (the family selection language);
    // empty user slots show a faint + and capture the current colour on click.
    component Swatch: Rectangle {
        property int idx: -1
        property bool userSlot: false
        readonly property string hex: userSlot ? panel.userSlots[idx] : panel.presets[idx]
        readonly property bool selected: userSlot ? panel.selUser === idx : panel.selPreset === idx
        width: 18; height: 18
        color: hex !== "" ? hex : "transparent"
        border.width: 1
        border.color: selected ? Theme.colors.textBright : Theme.colors.border
        Text {
            visible: parent.hex === ""
            anchors.centerIn: parent
            text: "+"; color: Theme.colors.textSubtle
            font.family: Theme.font.family; font.pixelSize: 11
        }
        MouseArea {
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
            acceptedButtons: parent.userSlot ? (Qt.LeftButton | Qt.RightButton) : Qt.LeftButton
            onClicked: (m) => {
                if (parent.userSlot && m.button === Qt.RightButton) {
                    if (parent.hex === "") return       // nothing to clear
                    var p = mapToItem(panel, m.x, m.y)
                    slotMenu.idx = parent.idx
                    slotMenu.x = Math.max(8, Math.min(p.x, panel.width - slotMenu.width - 8))
                    slotMenu.y = p.y + 4
                    slotMenu.open()
                    return
                }
                if (parent.userSlot) {
                    panel.selPreset = -1; panel.selUser = parent.idx
                    if (parent.hex === "") {            // empty slot captures the current colour
                        var cur = "" + panel.targetColor()
                        var u = panel.userSlots.slice(); u[parent.idx] = cur
                        panel.userSlots = u; panel.saveSwatches()
                    } else picker.value = parent.hex
                } else {
                    panel.selUser = -1; panel.selPreset = parent.idx
                    picker.value = parent.hex
                }
            }
        }
    }

    // Right-click menu for a user slot: clear it back to empty (+).
    Popup {
        id: slotMenu
        property int idx: -1
        padding: 4
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Rectangle {
            implicitWidth: 104; implicitHeight: 26
            color: clearMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
            Text {
                anchors.verticalCenter: parent.verticalCenter; x: 8
                text: "Clear slot"; color: Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            MouseArea {
                id: clearMA
                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: {
                    var u = panel.userSlots.slice(); u[slotMenu.idx] = ""
                    panel.userSlots = u; panel.saveSwatches()
                    if (panel.selUser === slotMenu.idx) panel.selUser = -1
                    slotMenu.close()
                }
            }
        }
    }
}
