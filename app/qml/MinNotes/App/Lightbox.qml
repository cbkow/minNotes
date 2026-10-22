// The LIGHTBOX (2026-10-22, ported from ufb's PreviewLightbox): a Quick-Look-style overlay for ONE image or video —
// Space on a selected canvas image / video element, or on a page's image / video block with the caret; a double-click
// on a canvas image opens it here too (the ladder's full form). A scrim over the whole window, the picture fit
// inside a margin, the file's name below. Space or Esc closes; ← / → step to the neighbouring media element (canvas)
// or block (page); a video plays on the SHARED player (the element's / block's own surface hides while this one
// shows) and takes the transport keys through the editor (K / W / S, Q / E, J / L …). Space is NOT play here —
// ufb's ruling, kept: play/pause lives on K / W / S so Space can be the one open / close key.
import QtQuick
import QtQuick.Window

Item {
    id: lb
    property var editor: null
    readonly property bool showing: visible
    property string kind: ""          // "image" | "video"
    property string source: ""        // the image's URL
    property string title: ""
    property string elementId: ""     // a canvas element …
    property int row: -1              // … or a page row
    property int shows: 0             // probe
    visible: false

    function _fileUrl(path) { return path === "" ? "" : (path.indexOf("file:") === 0 ? path : "file:///" + path.replace(/^\/+/, "")) }
    function _basename(path) { return path.substring(Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\")) + 1) }
    // A canvas element: the loadable file (a preview's PNG for PSD / HEIC / …), named after the original.
    function showElement(id) {
        if (!editor || id === "" || canvas.elementKind(id) !== "media") return
        const mk = canvas.mediaElementKind(id)
        if (mk !== "image" && mk !== "video") return
        kind = mk; elementId = id; row = -1
        title = canvas.mediaElementName(id)
        source = mk === "image" ? _fileUrl(canvas.elementLocalPath(id)) : ""
        _show()
        if (mk === "video" && editor.canvasItem) {
            editor.canvasItem.select(id)
            const h = editor.canvasVideoHost
            if (h && h.valid && !editor.canvasHostPlaying) editor.playFromHost(h)
        }
    }
    // A page's media block.
    function showRow(r) {
        if (!editor || r < 0 || blockModel.typeForRow(r) !== 3) return
        const mk = blockModel.mediaKind(r)
        if (mk !== "image" && mk !== "video") return
        kind = mk; row = r; elementId = ""
        title = _basename(blockModel.mediaSourcePath(r))
        source = mk === "image" ? blockModel.mediaUrl(r) : ""
        _show()
        if (mk === "video" && !(editor.videoPlayingRow === r && editor.videoPlayingKey !== "")) editor.playVideo(r)
    }
    function showImageFile(path) { kind = "image"; elementId = ""; row = -1; title = _basename(path); source = _fileUrl(path); _show() }   // dev: --shot-lightbox=<image>
    function _show() { ++shows; visible = true; forceActiveFocus() }
    function close() {
        if (!visible) return
        visible = false; kind = ""; source = ""
        if (editor) editor.forceActiveFocus()   // the selection / caret stays: Space re-opens
    }
    function toggle() { if (visible) close(); else if (editor) editor.lightboxSelected() }
    // ← / → : the neighbouring image / video, in z order on the canvas, in document order in a page.
    function step(dir) {
        if (elementId !== "" && editor && editor.canvasItem) {
            const all = canvas.visibleElements(-1e9, -1e9, 1e9, 1e9).filter(function(id) {
                return canvas.elementKind(id) === "media" && ["image", "video"].indexOf(canvas.mediaElementKind(id)) >= 0 })
            const i = all.indexOf(elementId)
            if (i < 0 || all.length < 2) return
            const next = all[(i + dir + all.length) % all.length]
            editor.canvasItem.select(next); showElement(next)
        } else if (row >= 0) {
            const n = blockModel.count
            for (let k = 1; k < n; ++k) {
                const r = (row + dir * k + n) % n
                if (blockModel.typeForRow(r) === 3 && ["image", "video"].indexOf(blockModel.mediaKind(r)) >= 0) { showRow(r); return }
            }
        }
    }

    focus: visible
    Keys.onPressed: (event) => {
        const k = event.key
        if (k === Qt.Key_Escape || k === Qt.Key_Space) { if (!event.isAutoRepeat) close(); event.accepted = true; return }
        if (k === Qt.Key_Left || k === Qt.Key_Right) {
            if (kind === "video" && editor && editor.videoKeyPressed(event)) { event.accepted = true; return }   // frame steps on a video
            if (!event.isAutoRepeat) step(k === Qt.Key_Left ? -1 : 1); event.accepted = true; return
        }
        if (kind === "video" && editor && editor.videoKeyPressed(event)) { event.accepted = true; return }
        event.accepted = true   // nothing types through the lightbox
    }
    Keys.onReleased: (event) => { if (kind === "video" && editor && editor.videoKeyReleased(event)) event.accepted = true }

    Rectangle {   // the scrim: a click on it closes
        anchors.fill: parent
        color: Qt.rgba(Theme.colors.bg.r, Theme.colors.bg.g, Theme.colors.bg.b, 0.94)
        TapHandler { acceptedButtons: Qt.LeftButton; gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: lb.close() }
        WheelHandler { }   // (swallowed: the canvas under the scrim must not zoom)
    }
    Item {
        id: content
        anchors.fill: parent; anchors.margins: 40; anchors.bottomMargin: 56
        TapHandler { acceptedButtons: Qt.LeftButton; gesturePolicy: TapHandler.ReleaseWithinBounds }   // a click on the picture stays
        Image {
            id: img
            anchors.fill: parent
            visible: lb.kind === "image"
            source: lb.kind === "image" ? lb.source : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true; cache: false; smooth: true; mipmap: true
            sourceSize.width: Math.round(content.width * Screen.devicePixelRatio)
            sourceSize.height: Math.round(content.height * Screen.devicePixelRatio)
        }
        VideoSurfaceItem {
            anchors.fill: parent
            visible: lb.kind === "video" && !!lb.editor && lb.editor._videoSurfaceReady && lb.editor.videoPlayingKey !== ""
            videoDecoder: lb.editor ? lb.editor.videoDecoder : null
            fillColor: Qt.rgba(Theme.colors.bg.r, Theme.colors.bg.g, Theme.colors.bg.b, 1.0)
        }
        Text {   // the decoder warming up, or an image that failed
            anchors.centerIn: parent
            visible: (lb.kind === "image" && (img.status === Image.Loading || img.status === Image.Error)) || (lb.kind === "video" && !!lb.editor && !lb.editor._videoSurfaceReady)
            text: img.status === Image.Error ? qsTr("image unavailable") : qsTr("loading…")
            color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }
    }
    Text {   // the name, under the picture
        anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 16
        width: parent.width - 32
        text: lb.title
        elide: Text.ElideMiddle
        color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
    }
}
