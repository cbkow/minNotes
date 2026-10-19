// A SHAPE element on the canvas (2.0). Text shapes (the converter's chips, the sketch labels) draw
// as a filled chip with contrast glyphs — the sketch_text rule; rect / ellipse / line / arrow land
// with the shape tools (a frame meanwhile).
import QtQuick

Item {
    id: sh
    required property var editor
    required property var cam
    property string elementId: ""
    anchors.fill: parent
    readonly property var spec: {
        canvas.elementsRevision
        try { return JSON.parse(elementId !== "" ? canvas.elementJson(elementId) : "{}") } catch (e) { return {} }
    }
    readonly property string kind: spec.shape || ""
    readonly property bool plain: !spec.fill || spec.fill === "transparent"   // plain text: no box, its own colour (2026-10-18)
    readonly property color fill: !plain ? spec.fill : "transparent"
    readonly property color textColor: plain ? (spec.color ? spec.color : Theme.colors.text)
                                             : ((fill.r * 0.299 + fill.g * 0.587 + fill.b * 0.114) > 0.6 ? "black" : "white")
    readonly property real fontSize: spec.fontSize ? spec.fontSize : Theme.font.sizeBody

    Rectangle {   // text chip (or plain text)
        visible: sh.kind === "text"
        anchors.fill: parent
        color: sh.fill
        radius: 0
        Text {
            anchors.fill: parent; anchors.margins: Math.round(sh.fontSize * 0.3)
            text: sh.spec.text || ""
            wrapMode: Text.Wrap   // an auto-width chip's rect fits its text (CanvasView.chipSize), so this only wraps a PINNED width — or a legacy rect
            color: sh.textColor
            font.family: Theme.font.body; font.pixelSize: sh.fontSize
        }
    }
    Rectangle {   // the other shapes, until their renderer lands
        visible: sh.kind !== "text"
        anchors.fill: parent
        color: "transparent"; border.width: 1; border.color: sh.fill; radius: sh.kind === "ellipse" ? Math.min(width, height) / 2 : 0
    }
}
