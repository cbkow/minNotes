// An INK element on the canvas (2.0): its strokes are element-local px (the converter's and the
// pen's schema), drawn by the GPU ink item in element-local mode over the element's own rect, so
// a mark lands exactly where it was drawn at every zoom (pxScale = the camera's zoom).
import QtQuick

InkNodeItem {
    id: ink
    required property var editor
    required property var cam
    property string elementId: ""
    anchors.fill: parent
    localPx: true
    envelope: elementId !== "" ? (canvas.inkRevision, canvas.elementJson(elementId)) : ""
    frameW: width; frameH: height
    sourceW: width; sourceH: height
    pxScale: cam.zoom
}
