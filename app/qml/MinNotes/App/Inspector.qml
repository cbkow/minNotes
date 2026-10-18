import QtQuick
import QtQuick.Controls
import QtCore

// The right-side Inspector — a panel that SLIDES in/out (CLOSED by default since the 2026-10-18 chrome
// re-organisation; the toolbar's toggle opens it; nothing in the panel closes it). ONE scroller with TWO
// COLLAPSIBLE SECTIONS: Comments (the page's threads, or the file's unanchored ones on the canvas) and History (the
// undo log of whatever ⌘Z would walk). No header strip, no view tabs any more. The COLOUR STATE still lives here
// (fgColor / bgColor / drawColor / drawWidth / drawTool / target + their Settings — six files read
// `editor.inspector.*`); its UI is ColorFlyout, behind the toolbar's swatch stack.
Rectangle {
    id: panel
    property var editor: null
    // The panel participates in the push layout via Main.qml's width subtraction. (It floated in
    // the page's annotation mode until 2.0 stage 1 removed that mode.)
    readonly property bool floating: false
    property bool open: false  // open/closed is persisted (see panelStore) and restored on launch; CLOSED by default (2026-10-18)
    property bool _ready: false   // gates the slide animation off during restore

    // Persist whether the panel is open, so it reopens in the user's last state.
    Settings {
        id: panelStore
        category: "inspector"
        property bool open: false
        property bool commentsCollapsed: false
        property bool historyCollapsed: false
    }
    onOpenChanged: if (_ready) panelStore.open = open   // (closing no longer disarms the pen: the tools live in the toolbar now)
    // The two sections' collapse state, persisted.
    property bool commentsCollapsed: false
    property bool historyCollapsed: false
    onCommentsCollapsedChanged: if (_ready) panelStore.commentsCollapsed = commentsCollapsed
    onHistoryCollapsedChanged: if (_ready) panelStore.historyCollapsed = historyCollapsed
    // The expanded thread in the comments section ("" = all collapsed).
    property string openThread: ""
    function showComments(threadId) {
        open = true
        commentsCollapsed = false
        openThread = threadId
    }

    // Colour state (the I/O the editor's apply functions read).
    property color fgColor: Theme.colors.text         // default type colour (E4E3E2)
    property color bgColor: "#FFEC59"                 // highlight colour (classic bright yellow)
    // Draw is the default target (user ruling 2026-08-19): annotation colour
    // is what the panel is aimed at most — Text/Highlight are a tab away.
    property string target: "draw"                     // which colour the picker edits

    // Drawing state (the video studio's annotator reads these; QSettings-
    // backed like QCView's annotation/colorHex). The armed tool IS the mode
    // (2026-08-19 redesign): "type" = regular text editing (the resting tool,
    // never persisted — the app always launches typing); anything else engages
    // the surface's ink — the Document view's margin-ink canvas, or a sketch/
    // video tab's local canvas. "select" moves existing ink.
    property color drawColor: "#FF0000"                // QCView's default stroke red
    property string drawTool: "type"                   // type|select|freehand|rect|oval|arrow|line|text|eraser
    // Arming a draw tool points the colour tabs at Draw — that's the colour
    // the next stroke uses, so the picker should already be aimed at it.
    onDrawToolChanged: if (drawTool !== "type" && drawTool !== "select") target = "draw"
    property real drawWidth: 6                         // source-pixel stroke width (1..64)
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
    // A step LIGHTER than the page so the panel reads as its own raised layer.
    // (The QCView darker-well recipe was tried in the product pass and vetoed
    // — the raised panel is the minNotes look; threads recess INTO it.)
    color: Theme.colors.surfaceRaised
    clip: true                                         // so content clips cleanly while sliding

    function targetColor() {
        return target === "fg" ? fgColor : target === "bg" ? bgColor : drawColor
    }
    Component.onCompleted: {
        if (drawStore.colorHex !== "") drawColor = drawStore.colorHex
        if (drawStore.width > 0) drawWidth = drawStore.width
        open = panelStore.open      // restore last open/closed state (no slide)
        commentsCollapsed = panelStore.commentsCollapsed
        historyCollapsed = panelStore.historyCollapsed
        _ready = true               // toggles from here on animate + persist
    }

    // Left hairline against the document (only meaningful while open).
    Rectangle { width: 1; height: parent.height; color: Theme.colors.border }

    // A section header: a chevron, the title, a count; the whole band toggles. Grey + white, no accent.
    component SectionHeader: Rectangle {
        id: sh
        property string title: ""
        property int count: -1
        property bool collapsed: false
        signal toggled()
        width: panel.panelW; height: 28; radius: 0
        color: shMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border }
        Icon { x: 10; anchors.verticalCenter: parent.verticalCenter; name: sh.collapsed ? "caret-right" : "caret-down"; size: 12; color: Theme.colors.textSubtle }
        Text { x: 28; anchors.verticalCenter: parent.verticalCenter; text: sh.title
               color: sh.collapsed ? Theme.colors.textMuted : Theme.colors.textBright
               font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome; font.bold: true }
        Text { anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter
               visible: sh.count >= 0; text: sh.count
               color: Theme.colors.textSubtle; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
        MouseArea { id: shMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: sh.toggled() }
    }

    // Fixed-width content anchored to the right edge so it doesn't squish during the slide; the panel's animating
    // width just reveals/hides it. ONE scroller: the sections stack at their content height.
    Item {
        width: panel.panelW
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        Flickable {
            id: outer
            anchors.fill: parent
            contentWidth: width
            contentHeight: col.implicitHeight + 8
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: MnScrollBar {}
            Column {
                id: col
                width: outer.width
                spacing: 0
                SectionHeader { title: qsTr("Comments"); count: threadRep.count; collapsed: panel.commentsCollapsed
                                onToggled: panel.commentsCollapsed = !panel.commentsCollapsed }
                Item { width: 1; height: 8; visible: !panel.commentsCollapsed }

        // --- Comments: thread cards (anchored first, then Unanchored) ---
            Column {
                id: threadsCol
                visible: !panel.commentsCollapsed
                x: 12; width: panel.contentW; spacing: 8
                Text {
                    visible: threadRep.count === 0
                    text: panel.editor && panel.editor.activeCanvas && !panel.editor.cardHosted
                          ? "No unanchored comments.\nComments live on pages —\nopen one to see its threads."
                          : "No comments yet.\nSelect text and use\n“Add comment”."
                    color: Theme.colors.textMuted
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                }
                Repeater {
                    id: threadRep
                    // On the canvas: the FILE's unanchored threads (no page is in view); on a page: its threads.
                    model: panel.editor && panel.editor.activeCanvas && !panel.editor.cardHosted
                           ? (blockModel.commentsRevision, canvas.elementsRevision, canvas.orphanThreads())
                           : (blockModel.commentsRevision, blockModel.structureRevision, blockModel.commentThreads())
                    delegate: Rectangle {
                        id: card
                        required property var modelData
                        readonly property bool expanded: panel.openThread === modelData.id
                        width: threadsCol.width
                        height: cardCol.implicitHeight + 16
                        // Card recessed into the raised panel (the input fields
                        // recess a step further). RESOLVED threads fade halfway
                        // toward the panel and dim their content.
                        color: modelData.resolved
                               ? Qt.tint(Theme.colors.card,
                                         Qt.rgba(Theme.colors.surfaceRaised.r, Theme.colors.surfaceRaised.g,
                                                 Theme.colors.surfaceRaised.b, 0.5))
                               : Theme.colors.card
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
                                                color: Theme.colors.surfaceRecess   // input = well, inset into the card
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
                                    color: Theme.colors.surfaceRecess   // input = well, inset into the card
                                    TextEdit {
                                        id: replyEdit
                                        anchors.fill: parent; anchors.margins: 6
                                        wrapMode: TextEdit.Wrap
                                        color: Theme.colors.text
                                        font.family: Theme.font.family
                                        font.pixelSize: Theme.font.sizeSmall
                                        selectByMouse: true
                                        // Enter submits; Shift+Enter = newline.
                                        Keys.onPressed: (e) => {
                                            if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter)
                                                && !(e.modifiers & Qt.ShiftModifier)) {
                                                var b = replyEdit.text.trim()
                                                if (b.length > 0) {
                                                    blockModel.addCommentMessage(modelData.id, b)
                                                    replyEdit.text = ""
                                                }
                                                e.accepted = true
                                            }
                                        }
                                    }
                                }
                                Row {
                                    spacing: 6
                                    FlatButton {
                                        visible: !modelData.resolved
                                        text: qsTr("Submit"); padding: 8
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
                Item { width: 1; height: 10 }
                SectionHeader { title: qsTr("History"); collapsed: panel.historyCollapsed
                                onToggled: panel.historyCollapsed = !panel.historyCollapsed }
                Item { width: 1; height: 6; visible: !panel.historyCollapsed }

        // --- History: the undo stack's ACTIVE PATH, oldest at the top
        // (the Photoshop reading). The current state carries the divider
        // fill; states ahead of now (redoable) dim. Click any row to time-
        // travel — the jump replays undo()/redo() steps, so carets, ink,
        // and page width all restore through the existing machinery. ---
        ListView {
            id: historyList
            visible: !panel.historyCollapsed
            x: 12; width: panel.contentW
            height: contentHeight            // the outer Flickable scrolls; this list never does
            interactive: false
            spacing: 1
            // The log that ⌘Z would walk: the canvas's on the Canvas tab, the page's otherwise.
            readonly property var log: panel.editor ? panel.editor.undoTarget : blockModel
            model: (log.undoRevision, log.undoHistory())
            onModelChanged: Qt.callLater(function() {   // keep "now" in view (the OUTER scroller moves)
                if (panel.historyCollapsed) return
                for (var i = 0; i < historyList.count; ++i)
                    if (historyList.model[i].current) {
                        const y = historyList.mapToItem(col, 0, i * 27).y
                        outer.contentY = Math.max(0, Math.min(outer.contentHeight - outer.height, y - outer.height / 2))
                        return
                    }
            })
            delegate: Rectangle {
                required property var modelData
                readonly property bool marker: modelData.idx === -2   // "… N earlier"
                width: panel.contentW; height: 26
                color: modelData.current ? Theme.colors.divider
                     : (histMA.containsMouse && histMA.enabled ? Theme.colors.surfaceHover
                                                               : "transparent")
                opacity: modelData.future ? 0.5 : 1
                Text {
                    anchors.left: parent.left; anchors.leftMargin: 8
                    anchors.right: histTime.left; anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: modelData.label
                    color: modelData.current ? Theme.colors.textBright
                         : marker ? Theme.colors.textSubtle : Theme.colors.text
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                    font.italic: marker
                }
                Text {
                    id: histTime
                    anchors.right: parent.right; anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.ts > 0 ? Qt.formatTime(new Date(modelData.ts), "hh:mm") : ""
                    color: Theme.colors.textSubtle
                    font.family: Theme.font.mono; font.pixelSize: 10
                }
                MouseArea {
                    id: histMA
                    anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    enabled: !parent.marker && !parent.modelData.current
                    onClicked: {
                        historyList.log.undoJumpTo(parent.modelData.idx)
                        if (panel.editor) panel.editor.forceActiveFocus()
                    }
                }
            }
        }
                Text {   // empty document, empty history
                    visible: !panel.historyCollapsed && historyList.count <= 1
                    x: 12; topPadding: 4
                    text: qsTr("Edits will appear here.")
                    color: Theme.colors.textMuted
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                }
            }
        }
    }
}
