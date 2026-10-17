// A MEDIA element on the canvas (2.0): the same MediaBlock the page uses, rendered through a
// PageContextLoader whose `blockModel` is a STAND-IN for one media row — the element's descriptor
// (json.media, the page descriptor verbatim) answers every media* read MediaBlock makes, sized to
// the element's rect, resolved through the file's MediaStore. Element-local frame ink
// (json.ink[page]) reaches MediaBlock's own InkNodeItem through mediaPageInk.
import QtQuick

Item {
    id: mi
    required property var editor
    required property var cam
    property string elementId: ""
    anchors.fill: parent

    QtObject {
        id: standIn
        readonly property int contentRevision: canvas.elementsRevision + canvas.inkRevision
        readonly property int layoutRevision: canvas.geometryRevision
        readonly property var media: {
            contentRevision
            try { return JSON.parse(mi.elementId !== "" ? canvas.elementJson(mi.elementId) : "{}").media || {} } catch (e) { return {} }
        }
        readonly property int count: 1
        function typeForRow(row) { return 3 }
        function mediaKind(row) { return media.kind || "image" }
        function mediaW(row) { return media.w || 0 }
        function mediaH(row) { return media.h || 0 }
        function mediaDurationMs(row) { return media.durMs || 0 }
        function mediaFileName(row) {
            if (media.name) return media.name
            const s = typeof media.src === "string" ? media.src : (media.src && media.src.rel) || ""
            return s.substring(s.lastIndexOf("/") + 1)
        }
        function mediaUrl(row) { return canvas.resolveMediaUrl(media.src) }
        function mediaViewPath(row) { return canvas.resolveMediaPath(media.src) }
        function mediaPlaybackSource(row) { return canvas.mediaPlaybackSource(media.src) }
        function mediaExtracting(row) { return canvas.mediaExtracting(media.src) }
        function mediaDispWidth(row) { return mi.width }
        function mediaDisplayHeight(row) { return mi.height }
        function mediaPageInk(row, page) { return canvas.mediaElementInk(mi.elementId, page) }
    }
    PageContextLoader {
        anchors.fill: parent
        active: mi.elementId !== ""
        pageModel: standIn
        sourceComponent: Component {
            MediaBlock {
                active: true
                logicalRow: 0
                maxWidth: mi.width
                width: mi.width; height: mi.height
            }
        }
    }
}
