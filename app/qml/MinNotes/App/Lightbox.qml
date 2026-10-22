// The LIGHTBOX (2026-10-22, ported from ufb's PreviewLightbox): a Quick-Look-style overlay for ONE media item — an
// image, a video, a PDF (paged) or an audio file — Space on a selected canvas element, Space with the caret on a
// page's image / video / PDF block, a double-click on a canvas image. A scrim over the whole window, the picture fit
// inside a margin, and a BAR along the top: ‹ › step to the neighbouring media item (canvas: the file's media
// elements in z order; page: its media blocks), the file's name, a PDF's pager, and × (Space / Esc / a click on the
// scrim close too). A video plays on the SHARED player with the compact transport under the picture (the element's /
// block's own surface hides while this one shows) and takes the transport keys through the editor (K / W / S,
// Q / E, J / L …) with the studio's full transport bar under the picture; Space is NOT play here — ufb's ruling: play/pause lives on K / W / S so Space can be the one open /
// close key. A PDF pages with PageUp / PageDown and its pager. Any navigation (a frame opening, Back) closes it.
import QtQuick
import QtQuick.Window

Item {
    id: lb
    property var editor: null
    readonly property bool showing: visible
    property string kind: ""          // "image" | "video" | "pdf" | "audio"
    property string source: ""        // the image's URL
    property string title: ""
    property string elementId: ""     // a canvas element …
    property int row: -1              // … or a page row
    property string pdfKey: ""        // a PDF: the editor's page map key
    property string pdfPath: ""
    property int pdfPages: 0
    property string audioKey: ""      // an audio element: the anchor path the shared player compares to
    property int shows: 0             // probe
    visible: false
    readonly property var bar: barRow        // probe
    readonly property var transport: ctLoader.item

    function _fileUrl(path) { return path === "" ? "" : (path.indexOf("file:") === 0 ? path : "file:///" + path.replace(/^\/+/, "")) }
    function _basename(path) { return path.substring(Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\")) + 1) }
    function _b64url(s) {   // the PDF page provider's key (MediaBlock's scheme): base64url of the UTF-8 path
        var enc = encodeURIComponent(s), bytes = []
        for (var i = 0; i < enc.length; ++i) {
            if (enc[i] === '%') { bytes.push(parseInt(enc.substr(i + 1, 2), 16)); i += 2 } else bytes.push(enc.charCodeAt(i))
        }
        var bin = ""; for (var k = 0; k < bytes.length; ++k) bin += String.fromCharCode(bytes[k])
        return Qt.btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "")
    }
    readonly property int pdfPage: (editor ? editor.pdfPageRev : 0, kind === "pdf" && editor ? editor.pdfPageForKey(pdfKey) : 0)
    function pdfStep(d) { if (kind === "pdf" && editor) editor.pdfStepKey(pdfKey, d, pdfPages) }
    function _lightboxable(mk) { return ["image", "video", "pdf", "audio"].indexOf(mk) >= 0 }
    // A canvas element: the loadable file (a preview's PNG for PSD / HEIC / …), named after the original.
    function showElement(id) {
        if (!editor || id === "" || canvas.elementKind(id) !== "media") return
        const mk = canvas.mediaElementKind(id)
        if (!_lightboxable(mk)) return
        _reset(); kind = mk; elementId = id
        title = canvas.mediaElementName(id)
        const md = editor.canvasItem ? editor.canvasItem.mediaOf(id) : {}
        if (mk === "image") source = _fileUrl(canvas.elementLocalPath(id))
        if (mk === "pdf") { pdfPath = canvas.elementLocalPath(id); pdfKey = canvas.resolveMediaPath(md.src); pdfPages = md.pages || 0 }
        if (mk === "audio") audioKey = canvas.mediaAnchorPath(md.src)
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
        if (mk !== "image" && mk !== "video" && mk !== "pdf") return
        _reset(); kind = mk; row = r
        title = _basename(blockModel.mediaSourcePath(r))
        if (mk === "image") source = blockModel.mediaUrl(r)
        if (mk === "pdf") { pdfPath = blockModel.mediaViewPath(r); pdfKey = pdfPath; pdfPages = blockModel.mediaPdfPages(r) }
        _show()
        if (mk === "video" && !(editor.videoPlayingRow === r && editor.videoPlayingKey !== "")) editor.playVideo(r)
    }
    function showImageFile(path) { _reset(); kind = "image"; title = _basename(path); source = _fileUrl(path); _show() }   // dev: --shot-lightbox=<image>
    function _reset() { kind = ""; source = ""; elementId = ""; row = -1; pdfKey = ""; pdfPath = ""; pdfPages = 0; audioKey = "" }
    function _show() { ++shows; visible = true; forceActiveFocus() }
    function close() {
        if (!visible) return
        visible = false; _reset()
        if (editor) editor.forceActiveFocus()   // the selection / caret stays: Space re-opens
    }
    function toggle() { if (visible) close(); else if (editor) editor.lightboxSelected() }
    // ‹ › : the neighbouring media item, in z order on the canvas, in document order in a page.
    function _siblings() {
        if (elementId !== "") return canvas.visibleElements(-1e9, -1e9, 1e9, 1e9).filter(function(id) { return canvas.elementKind(id) === "media" && lb._lightboxable(canvas.mediaElementKind(id)) })
        if (row >= 0) { const a = []; for (let r = 0; r < blockModel.count; ++r) if (blockModel.typeForRow(r) === 3 && ["image", "video", "pdf"].indexOf(blockModel.mediaKind(r)) >= 0) a.push(r); return a }
        return []
    }
    readonly property int siblingCount: (canvas.elementsRevision, blockModel.structureRevision, showing ? _siblings().length : 0)
    function step(dir) {
        const all = _siblings()
        if (all.length < 2) return
        if (elementId !== "" && editor && editor.canvasItem) {
            const i = all.indexOf(elementId); if (i < 0) return
            const next = all[(i + dir + all.length) % all.length]
            editor.canvasItem.select(next); showElement(next)
        } else if (row >= 0) {
            const i = all.indexOf(row); if (i < 0) return
            showRow(all[(i + dir + all.length) % all.length])
        }
    }
    // Any navigation under the overlay (a frame opening from the transport's expand glyph, Back / Forward) closes it.
    Connections { target: lb.editor; function onActiveFrameIdChanged() { if (lb.visible) lb.close() } }

    focus: visible
    Keys.onPressed: (event) => {
        const k = event.key
        if (k === Qt.Key_Escape || k === Qt.Key_Space) { if (!event.isAutoRepeat) close(); event.accepted = true; return }
        if (k === Qt.Key_Left || k === Qt.Key_Right) {
            if (kind === "video" && editor && editor.videoKeyPressed(event)) { event.accepted = true; return }   // frame steps on a video
            if (!event.isAutoRepeat) step(k === Qt.Key_Left ? -1 : 1); event.accepted = true; return
        }
        if (kind === "pdf" && (k === Qt.Key_PageDown || k === Qt.Key_PageUp)) { pdfStep(k === Qt.Key_PageDown ? 1 : -1); event.accepted = true; return }
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
    Item {   // the picture, under the bar (and above a video's transport)
        id: content
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: barRow.bottom; anchors.bottom: parent.bottom
        anchors.margins: 40; anchors.topMargin: 12; anchors.bottomMargin: lb.kind === "video" ? 40 + 40 + 8 : 40
        TapHandler { acceptedButtons: Qt.LeftButton; gesturePolicy: TapHandler.ReleaseWithinBounds }   // a click on the picture stays
        Image {
            id: img
            anchors.fill: parent
            visible: lb.kind === "image" || lb.kind === "pdf"
            source: lb.kind === "image" ? lb.source
                  : lb.kind === "pdf" && lb.pdfPath !== "" ? "image://pdfpage/" + lb._b64url(lb.pdfPath) + "@" + lb.pdfPage : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true; cache: lb.kind === "pdf"; smooth: true; mipmap: lb.kind === "image"
            sourceSize.width: Math.round(content.width * Screen.devicePixelRatio)
            sourceSize.height: Math.round(content.height * Screen.devicePixelRatio)
        }
        VideoSurfaceItem {
            anchors.fill: parent
            visible: lb.kind === "video" && !!lb.editor && lb.editor._videoSurfaceReady && lb.editor.videoPlayingKey !== ""
            videoDecoder: lb.editor ? lb.editor.videoDecoder : null
            fillColor: Qt.rgba(Theme.colors.bg.r, Theme.colors.bg.g, Theme.colors.bg.b, 1.0)
        }
        Column {   // AUDIO: the name and a big play / pause on the shared player
            anchors.centerIn: parent; spacing: 14
            visible: lb.kind === "audio"
            readonly property bool mine: !!lb.editor && lb.editor.audioPlayingKey !== "" && lb.editor.audioPlayingKey === lb.audioKey
            Icon { anchors.horizontalCenter: parent.horizontalCenter; name: "speaker-high"; size: 48; color: Theme.colors.textMuted }
            FlatButton {
                id: audioBtn
                anchors.horizontalCenter: parent.horizontalCenter
                implicitWidth: 44; implicitHeight: 44; iconSize: 22; radius: Theme.dim.radiusSmall
                iconName: parent.mine && lb.editor.audioPlayer.isPlaying ? "pause" : "play"
                tooltip: parent.mine && lb.editor.audioPlayer.isPlaying ? qsTr("Pause (K)") : qsTr("Play (K)"); tooltipSide: "bottom"
                onClicked: { if (lb.editor && lb.elementId !== "") lb.editor.playAudioElement(lb.elementId); lb.forceActiveFocus() }
            }
        }
        Text {   // the decoder warming up, or an image that failed
            anchors.centerIn: parent
            visible: ((lb.kind === "image" || lb.kind === "pdf") && (img.status === Image.Loading || img.status === Image.Error)) || (lb.kind === "video" && !!lb.editor && !lb.editor._videoSurfaceReady)
            text: img.status === Image.Error ? qsTr("unavailable") : qsTr("loading…")
            color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }
    }
    Loader {   // VIDEO: the FULL family transport under the picture (the studio's bar — user 2026-10-22), the page's or the element's
        id: ctLoader
        active: lb.kind === "video" && !!lb.editor
        anchors.left: content.left; anchors.right: content.right; anchors.bottom: parent.bottom; anchors.bottomMargin: 40
        height: 40
        sourceComponent: VideoTransport {
            editor: lb.editor
            dec: lb.editor.videoDecoder
            audio: lb.editor.audioPlayer
            host: lb.elementId !== "" ? lb.editor.canvasVideoHost : null
            row: lb.row
            live: lb.elementId !== "" ? lb.editor.canvasHostPlaying : (lb.row >= 0 && lb.editor.videoPlayingRow === lb.row && lb.editor.videoPlayingKey !== "")
        }
    }
    Item {   // THE BAR, along the top (user 2026-10-22): ‹ › · the name · a PDF's pager · ×
        id: barRow
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.margins: 16; anchors.bottomMargin: 0
        height: 32
        TapHandler { acceptedButtons: Qt.LeftButton; gesturePolicy: TapHandler.ReleaseWithinBounds }   // clicks on the bar stay
        component BarBtn: Rectangle {
            id: bb
            property string icon: ""
            property int iconSize: 14
            property bool can: true
            property string tip: ""
            signal clicked()
            width: 30; height: 30; radius: Theme.dim.radiusSmall
            color: can && bbMA.containsMouse ? Theme.colors.affordanceHover : Theme.colors.affordanceIdleClear
            Behavior on color { ColorAnimation { duration: Theme.dim.hoverMs } }
            Icon { anchors.centerIn: parent; name: bb.icon; size: bb.iconSize; color: !bb.can ? Theme.colors.textSubtle : bbMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted }
            MouseArea { id: bbMA; anchors.fill: parent; hoverEnabled: true; enabled: bb.can; cursorShape: Qt.PointingHandCursor; onClicked: { bb.clicked(); lb.forceActiveFocus() } }
            FlatToolTip { visible: bbMA.containsMouse && bb.can && bb.tip !== ""; text: bb.tip; x: 0; y: bb.height + 6 }
        }
        Row {
            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; spacing: 2
            BarBtn { id: prevBtn; icon: "caret-left"; can: lb.siblingCount > 1; tip: qsTr("Previous (←)"); onClicked: lb.step(-1) }
            BarBtn { id: nextBtn; icon: "caret-right"; can: lb.siblingCount > 1; tip: qsTr("Next (→)"); onClicked: lb.step(1) }
        }
        Text {   // the name, centred
            anchors.centerIn: parent
            width: parent.width - 2 * 200
            horizontalAlignment: Text.AlignHCenter
            text: lb.title
            elide: Text.ElideMiddle
            color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }
        Row {   // a PDF's pager, then ×
            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; spacing: 2
            Row {
                visible: lb.kind === "pdf" && lb.pdfPages > 1
                anchors.verticalCenter: parent.verticalCenter; spacing: 2
                BarBtn { id: pagePrev; icon: "caret-left"; iconSize: 12; can: lb.pdfPage > 0; tip: qsTr("Previous page (PageUp)"); onClicked: lb.pdfStep(-1) }
                Text { anchors.verticalCenter: parent.verticalCenter; text: (lb.pdfPage + 1) + " / " + lb.pdfPages
                       color: Theme.colors.textMuted; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall; leftPadding: 4; rightPadding: 4 }
                BarBtn { id: pageNext; icon: "caret-right"; iconSize: 12; can: lb.pdfPage < lb.pdfPages - 1; tip: qsTr("Next page (PageDown)"); onClicked: lb.pdfStep(1) }
                Item { width: 10; height: 1 }
            }
            BarBtn { id: closeBtn; icon: "x"; tip: qsTr("Close (Esc)"); onClicked: lb.close() }
        }
    }
    readonly property var prevButton: prevBtn   // probe
    readonly property var closeButton: closeBtn
}
