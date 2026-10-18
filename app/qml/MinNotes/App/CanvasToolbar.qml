// The view's tools (2.0), in the top strip beside the file tabs — only what the view in front can
// use: the canvas gets the pointer, the hand, the page tool, text and the ink tools; a PDF or video
// tab the ink tools it takes frame ink with; the page view none (its formatting is the left rail).
// The tool state stays the Inspector's `drawTool` (the studio and the pen read it); the swatch and
// the width readout open the Inspector's palette, where the colour and the width live.
import QtQuick

Row {
    id: bar
    required property var editor
    readonly property var inspector: editor ? editor.inspector : null
    readonly property string view: !editor || !inspector ? ""
                                  : editor.activeCanvas ? "canvas"
                                  : (editor.pdfTabOpen || editor.videoTabOpen) ? "frame" : ""
    visible: view !== ""
    enabled: !(editor && editor.cardHosted)   // while a page is live its keys and clicks belong to the editor (phase C6)
    spacing: 2
    height: 36
    readonly property var canvasTools: [
        { tool: "select",   icon: "cursor",         tip: qsTr("Select  (V)") },
        { tool: "hand",     icon: "hand",           tip: qsTr("Hand — pan  (H)") },
        { tool: "page",     icon: "file-text",      tip: qsTr("Page — click to place, drag to size  (N)") },
        { tool: "text",     icon: "textbox",        tip: qsTr("Text  (T)") },
        { tool: "freehand", icon: "scribble",       tip: qsTr("Pen  (P)") },
        { tool: "rect",     icon: "rectangle",      tip: qsTr("Rectangle  (R)") },
        { tool: "oval",     icon: "circle",         tip: qsTr("Oval  (O)") },
        { tool: "line",     icon: "line-segment",   tip: qsTr("Line  (L)") },
        { tool: "arrow",    icon: "arrow-up-right", tip: qsTr("Arrow  (A)") },
        { tool: "eraser",   icon: "eraser",         tip: qsTr("Eraser  (E)") }
    ]
    readonly property var frameTools: [
        { tool: "select",   icon: "cursor",         tip: qsTr("Select / Move") },
        { tool: "freehand", icon: "scribble",       tip: qsTr("Freehand") },
        { tool: "rect",     icon: "rectangle",      tip: qsTr("Rectangle") },
        { tool: "oval",     icon: "circle",         tip: qsTr("Oval") },
        { tool: "line",     icon: "line-segment",   tip: qsTr("Line") },
        { tool: "arrow",    icon: "arrow-up-right", tip: qsTr("Arrow") },
        { tool: "text",     icon: "textbox",        tip: qsTr("Text box") },
        { tool: "eraser",   icon: "eraser",         tip: qsTr("Eraser") }
    ]
    function isChecked(tool) {
        const t = inspector ? inspector.drawTool : "type"
        return t === tool || (tool === "select" && t === "type")   // "type" is the resting pointer on the canvas too
    }
    Repeater {
        model: bar.view === "canvas" ? bar.canvasTools : bar.view === "frame" ? bar.frameTools : []
        delegate: FlatButton {
            required property var modelData
            anchors.verticalCenter: parent.verticalCenter
            iconName: modelData.icon; tooltip: modelData.tip; tooltipSide: "bottom"
            implicitHeight: 28; implicitWidth: 30; iconSize: 16; radius: 0
            checked: bar.isChecked(modelData.tool)
            checkedColor: Theme.colors.divider
            iconColor: checked ? Theme.colors.textBright : Theme.colors.textMuted
            onClicked: if (bar.inspector) { bar.inspector.drawTool = modelData.tool; if (bar.editor) bar.editor.forceActiveFocus() }
        }
    }
    Rectangle { width: 1; height: 20; anchors.verticalCenter: parent.verticalCenter; color: Theme.colors.border; visible: bar.view !== "" }
    Rectangle {   // the draw colour: opens the palette
        anchors.verticalCenter: parent.verticalCenter
        width: 18; height: 18; radius: 0
        color: bar.inspector ? bar.inspector.drawColor : "transparent"
        border.width: 1; border.color: Theme.colors.border
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: if (bar.inspector) bar.inspector.view = "palette" }
        FlatToolTip { visible: parent.children[0].containsMouse; text: qsTr("Colour — open the palette") }
    }
    Text {   // the pen width: opens the palette
        anchors.verticalCenter: parent.verticalCenter
        leftPadding: 6; rightPadding: 8
        text: bar.inspector ? bar.inspector.drawWidth + " px" : ""
        color: Theme.colors.textMuted
        font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: if (bar.inspector) bar.inspector.view = "palette" }
    }
}
