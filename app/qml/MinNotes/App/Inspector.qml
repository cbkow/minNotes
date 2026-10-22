import QtQuick
import QtQuick.Controls
import QtCore

// The right-side Inspector — a panel that SLIDES in/out (CLOSED by default since the 2026-10-18 chrome
// re-organisation; the toolbar's toggle opens it; nothing in the panel closes it). ONE flat HISTORY list (the undo
// log of whatever ⌘Z would walk) — the Comments section left with the comments feature (2026-10-21), and with it the
// section headers and their collapse state. No header strip, no view tabs. The COLOUR STATE still lives here
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
    }
    onOpenChanged: if (_ready) panelStore.open = open   // (closing no longer disarms the pen: the tools live in the toolbar now)

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
    // — the raised panel is the minNotes look.)
    color: Theme.colors.surfaceRaised
    clip: true                                         // so content clips cleanly while sliding

    function targetColor() {
        return target === "fg" ? fgColor : target === "bg" ? bgColor : drawColor
    }
    Component.onCompleted: {
        if (drawStore.colorHex !== "") drawColor = drawStore.colorHex
        if (drawStore.width > 0) drawWidth = drawStore.width
        open = panelStore.open      // restore last open/closed state (no slide)
        _ready = true               // toggles from here on animate + persist
    }

    // Left hairline against the document (only meaningful while open).
    Rectangle { width: 1; height: parent.height; color: Theme.colors.border }

    // Fixed-width content anchored to the right edge so it doesn't squish during the slide; the panel's animating
    // width just reveals/hides it. While the CANVAS is up: the Layers panel on top (2026-10-21) over the history;
    // in a page, the history alone. ONE scroller over the history list.
    readonly property bool layersShown: !!editor && editor.canvasShown
    readonly property alias layers: layerPanel
    readonly property alias frameSection: framePanel   // probe
    Item {
        width: panel.panelW
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        FramePanel {   // the FRAME section (2026-10-22): the one selected frame's title / tint / sizing
            id: framePanel
            width: panel.panelW
            anchors.top: parent.top
            editor: panel.editor
            visible: panel.layersShown && frameId !== ""
        }
        Text {
            id: layersLabel
            visible: panel.layersShown
            x: 12; anchors.top: framePanel.bottom; anchors.topMargin: 8; height: visible ? 18 : 0
            text: qsTr("Layers")
            color: Theme.colors.textMuted
            font.family: Theme.font.family; font.pixelSize: 11; font.bold: true
        }
        LayerPanel {
            id: layerPanel
            objectName: "layerPanel"
            visible: panel.layersShown
            width: panel.panelW   // full width like the history's scroller, so both scrollbars ride the panel's edge; the rows inset themselves
            anchors.top: layersLabel.bottom
            height: visible ? Math.round((parent.height - 40 - framePanel.height) * 0.6) : 0
            editor: panel.editor
        }
        Rectangle {
            id: layersRule
            visible: panel.layersShown
            anchors.top: layerPanel.bottom; anchors.topMargin: 8
            width: parent.width; height: visible ? 1 : 0
            color: Theme.colors.border
        }
        Text {
            id: historyLabel
            visible: panel.layersShown
            x: 12; anchors.top: layersRule.bottom; anchors.topMargin: 6; height: visible ? 18 : 0
            text: qsTr("History")
            color: Theme.colors.textMuted
            font.family: Theme.font.family; font.pixelSize: 11; font.bold: true
        }
        Flickable {
            id: outer
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; top: historyLabel.bottom }
            contentWidth: width
            contentHeight: col.implicitHeight + 8
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: MnScrollBar {}
            Column {
                id: col
                width: outer.width
                spacing: 0
                Item { width: 1; height: panel.layersShown ? 2 : 8 }

        // --- History: the undo stack's ACTIVE PATH, oldest at the top
        // (the Photoshop reading). The current state carries the divider
        // fill; states ahead of now (redoable) dim. Click any row to time-
        // travel — the jump replays undo()/redo() steps, so carets, ink,
        // and page width all restore through the existing machinery. ---
        ListView {
            id: historyList
            x: 12; width: panel.contentW
            height: contentHeight            // the outer Flickable scrolls; this list never does
            interactive: false
            spacing: 1
            // The log that ⌘Z would walk: the canvas's on the Canvas tab, the page's otherwise.
            readonly property var log: panel.editor ? panel.editor.undoTarget : blockModel
            model: (log.undoRevision, log.undoHistory())
            onModelChanged: Qt.callLater(function() {   // keep "now" in view (the OUTER scroller moves)
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
                    visible: historyList.count <= 1
                    x: 12; topPadding: 4
                    text: qsTr("Edits will appear here.")
                    color: Theme.colors.textMuted
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                }
            }
        }
    }
}
