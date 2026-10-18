// The view's tools (2.0): the canvas's pointer / hand / page / text / ink tools, a PDF or video tab's ink tools,
// nothing for the page view (its formatting is the toolbar's page-mode set). A SEGMENT of ChromeToolbar since the
// chrome re-organisation (2026-10-18); the probes read it as `editor.toolbarItem` (visible / view / enabled). The
// tool state stays the Inspector's `drawTool` (the studio and the pen read it); the colour and the width live in the
// colour flyout behind the toolbar's swatch stack.
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
    height: Theme.dim.toolbarH
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
            implicitHeight: Theme.dim.chromeBtn; implicitWidth: Theme.dim.chromeBtn; iconSize: Theme.dim.chromeIcon; radius: 0
            checked: bar.isChecked(modelData.tool)
            checkedColor: Theme.colors.divider
            iconColor: checked ? Theme.colors.textBright : Theme.colors.textMuted
            onClicked: if (bar.inspector) { bar.inspector.drawTool = modelData.tool; if (bar.editor) bar.editor.forceActiveFocus() }
        }
    }
}
