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
    readonly property color fill: spec.fill ? spec.fill : "#ffffffff"
    readonly property real fontSize: spec.fontSize ? spec.fontSize : 16

    Rectangle {   // text chip
        visible: sh.kind === "text"
        anchors.fill: parent
        color: sh.fill
        radius: 0
        Text {
            anchors.fill: parent; anchors.margins: Math.round(sh.fontSize * 0.3)
            text: sh.spec.text || ""
            wrapMode: Text.Wrap
            color: (sh.fill.r * 0.299 + sh.fill.g * 0.587 + sh.fill.b * 0.114) > 0.6 ? "black" : "white"
            font.family: Theme.font.body; font.pixelSize: sh.fontSize
        }
    }
    Rectangle {   // the other shapes, until their renderer lands
        visible: sh.kind !== "text"
        anchors.fill: parent
        color: "transparent"; border.width: 1; border.color: sh.fill; radius: sh.kind === "ellipse" ? Math.min(width, height) / 2 : 0
    }
}
