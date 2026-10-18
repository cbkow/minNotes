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
        function mediaPdfPages(row) { return media.pages || 0 }
        function mediaFps(row) { return media.fps || 0 }
        function mediaFrames(row) { return media.frames || 0 }
        function mediaAnchorPath(row) { return canvas.mediaAnchorPath(media.src) }
        function mediaDispWidth(row) { return mi.width }
        function mediaDisplayHeight(row) { return mi.height }
        function mediaPageInk(row, page) { return canvas.mediaElementInk(mi.elementId, page) }
    }
    // The shown PDF page and the video poster frame follow the editor's path-keyed maps (phase A5):
    // page with the pager under the selection frame; the poster is the studio's banked playhead.
    readonly property string pdfKey: standIn.media.kind === "pdf" ? standIn.mediaViewPath(0) : ""
    readonly property int pdfPage: (mi.editor.pdfPageRev, pdfKey !== "" ? mi.editor.pdfPageForKey(pdfKey) : 0)
    readonly property int posterFrame: (mi.editor.videoPlayheadRev, standIn.media.kind === "video" ? mi.editor.videoPlayheadForKey(standIn.mediaAnchorPath(0)) : 0)
    // Routing by descriptor kind (phase A6): folder and file chips have their own cards (name + full path);
    // images / videos / PDFs are MediaBlock over the stand-in. (Bookmarks and audio get cards in A7 / A9.)
    readonly property string mediaKind: standIn.media.kind || "image"
    readonly property bool ownCard: mediaKind === "folder" || mediaKind === "file" || mediaKind === "bookmark"
    readonly property var card: cardLoader.item   // probe
    Loader {
        id: cardLoader
        anchors.fill: parent
        active: mi.elementId !== "" && mi.ownCard
        sourceComponent: mi.mediaKind === "folder" ? folderCard : mi.mediaKind === "bookmark" ? bookmarkCard : fileCard
    }
    Component { id: bookmarkCard; CanvasBookmarkCard { media: standIn.media } }
    Component { id: folderCard; CanvasFolderCard { media: standIn.media; path: standIn.mediaViewPath(0) } }
    Component { id: fileCard;   CanvasFileCard   { media: standIn.media; path: standIn.mediaViewPath(0) } }
    PageContextLoader {
        anchors.fill: parent
        active: mi.elementId !== "" && !mi.ownCard
        pageModel: standIn
        sourceComponent: Component {
            MediaBlock {
                active: true
                logicalRow: 0
                maxWidth: mi.width
                width: mi.width; height: mi.height
                pdfPage: mi.pdfPage
                posterFrame: mi.posterFrame
            }
        }
    }
}
