import QtQuick

// Inline render for a Media block. Images show a plain async Image; video shows
// a real frame thumbnail (the first frame at rest, or the remembered playhead
// after playback) via the "videoframe" image provider — there is NO play
// affordance on the frame; the per-video toolbar is the sole transport. The
// intrinsic dims come from the model, so height is reserved BEFORE anything
// loads (no layout jump; the value also feeds the Fenwick index).
//
// While a video is the active player its live surface (a root overlay) covers
// this poster, so we drop the thumbnail then (isActivePlayer) to avoid a
// pointless decode.
Item {
    id: mb
    property int  logicalRow: -1
    property bool active: false
    property real maxWidth: 760
    property int  posterFrame: 0       // frame the poster shows (0, or remembered playhead)
    property bool isActivePlayer: false
    // While the editor surface is resizing, drop the texture render entirely
    // (no scaling/reload glitches) — the accent-border frame below holds the
    // reserved geometry until the size settles.
    property bool suspended: false

    readonly property int _rev: blockModel.contentRevision
    readonly property string kind: active ? (mb._rev, blockModel.mediaKind(logicalRow)) : ""
    readonly property bool   isVideo: kind === "video"
    readonly property bool   isFile: kind === "file"
    readonly property bool   isPdf: kind === "pdf"
    readonly property bool   isSketch: kind === "sketch"
    readonly property string sketchJson: (active && isSketch)
        ? (mb._rev, blockModel.sketchResolvedJson(logicalRow)) : ""
    property int  pdfPage: 0          // current page (driven by the editor's nav)
    readonly property string pdfPath: (active && isPdf) ? (mb._rev, blockModel.mediaLocalPath(logicalRow)) : ""
    readonly property string fileName: (active && isFile) ? (mb._rev, blockModel.mediaFileName(logicalRow)) : ""
    readonly property string filePath: (active && isFile) ? (mb._rev, blockModel.mediaLocalPath(logicalRow)) : ""
    readonly property string url: active ? (mb._rev, blockModel.mediaUrl(logicalRow)) : ""
    readonly property string localPath: (active && isVideo) ? (mb._rev, blockModel.mediaLocalPath(logicalRow)) : ""
    // Dims keyed on logicalRow (not active) so the displayed width is correct the
    // instant a delegate recycles onto a media row (no `active`-settle frame where
    // it reads 0). The frame HEIGHT is the model's authoritative mediaDisplayHeight
    // (set on the host from the cell), not implicitHeight — so this only governs
    // width/sourceSize. mediaW/H clamp invalid rows to 0, so the bare read is safe.
    readonly property int iw: logicalRow >= 0 ? (mb._rev, blockModel.mediaW(logicalRow)) : 0
    readonly property int ih: logicalRow >= 0 ? (mb._rev, blockModel.mediaH(logicalRow)) : 0
    readonly property real durMs: (active && isVideo) ? (mb._rev, blockModel.mediaDurationMs(logicalRow)) : 0
    // Effective display width from the model (per-block override or default) so a
    // resized image renders at its stored width; height comes from the cell.
    readonly property real dispW: logicalRow >= 0
        ? (blockModel.layoutRevision, mb._rev, blockModel.mediaDispWidth(logicalRow))
        : maxWidth

    implicitWidth:  dispW
    implicitHeight: (iw > 0 && ih > 0) ? Math.round(dispW * ih / iw) : Math.round(dispW * 0.5)

    Rectangle {   // resize placeholder: accent-bordered frame at the reserved size
        visible: mb.suspended && mb.active && !mb.isFile
        anchors.fill: parent
        color: "transparent"
        border.width: 1; border.color: Theme.colors.divider   // quiet grey frame
    }

    function _fmtDur(ms) {
        if (ms <= 0) return ""
        var s = Math.round(ms / 1000)
        var m = Math.floor(s / 60)
        return m + ":" + ((s % 60) < 10 ? "0" : "") + (s % 60)
    }
    // base64url(UTF-8 bytes of path) — matches the providers' decode
    // (QByteArray::fromBase64(.., Base64UrlEncoding) + QString::fromUtf8). Feeds
    // Qt.btoa a byte array (the deprecated string overload UTF-16-mangles non-ASCII
    // and warns); base64url keeps '/' and spaces URL-safe.
    function _b64urlPath(s) {
        var enc = encodeURIComponent(s), bytes = []
        for (var i = 0; i < enc.length; ++i) {
            if (enc[i] === '%') { bytes.push(parseInt(enc.substr(i + 1, 2), 16)); i += 2 }
            else bytes.push(enc.charCodeAt(i))
        }
        // Qt.btoa's array overload returns a QByteArray (not a JS String) — coerce
        // before the base64url char swaps.
        return ("" + Qt.btoa(bytes)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
    }
    // image://videoframe/<base64url(path)>@<frame>
    function _vframeSrc(path, frame) {
        if (path === "") return ""
        return "image://videoframe/" + _b64urlPath(path) + "@" + frame
    }
    // image://pdfpage/<base64url(path)>@<page>  — same scheme, cached per page.
    function _pdfSrc(path, page) {
        if (path === "") return ""
        return "image://pdfpage/" + _b64urlPath(path) + "@" + page
    }

    // --- File attachment chip (unsupported file: icon + name + path) ---
    Rectangle {
        visible: mb.isFile
        width: Math.min(360, mb.maxWidth); height: parent.height
        radius: Theme.dim.radius
        color: Theme.colors.surfaceHover
        border.width: 1; border.color: Theme.colors.border
        Row {
            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
            spacing: 10
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "file"; size: 24; color: Theme.colors.textMuted
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 44; spacing: 2
                Text {
                    width: parent.width; elide: Text.ElideMiddle
                    text: mb.fileName === "" ? "file" : mb.fileName
                    color: Theme.colors.text
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                }
                Text {
                    width: parent.width; elide: Text.ElideMiddle
                    text: mb.filePath
                    color: Theme.colors.textSubtle
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                }
            }
        }
    }

    // --- PDF branch: render the current page (fit width, white-backed) through
    // the "pdfpage" image provider. The provider URL carries the page, so
    // cache:true keeps the rendered page across delegate recycle (a PdfPageImage
    // tied to a churning PdfDocument blanks to white on scroll-away-and-back). ---
    Rectangle {
        id: pdfRect
        visible: mb.isPdf && !mb.suspended
        anchors.fill: parent
        color: "white"
        radius: Theme.dim.radius
        border.width: 1; border.color: Theme.colors.border
        clip: true
        onVisibleChanged: if (visible) pdfFade.restart()
        NumberAnimation { id: pdfFade; target: pdfRect; property: "opacity"
                          from: 0; to: 1; duration: 200; easing.type: Easing.OutCubic }
        Image {
            anchors.fill: parent
            source: (mb.isPdf && mb.pdfPath !== "") ? mb._pdfSrc(mb.pdfPath, mb.pdfPage) : ""
            asynchronous: true; cache: true
            fillMode: Image.PreserveAspectFit
            sourceSize.width: Math.round(mb.dispW * Screen.devicePixelRatio)
            smooth: true
        }
    }

    // --- Sketch branch: transparent canvas (ruling 2026-06-11), divider
    // border marking the extent; strokes render passively (disarmed canvas).
    // Editing lives in the full-frame sketch tab. ---
    Rectangle {
        visible: mb.isSketch && !mb.suspended
        anchors.fill: parent
        color: "transparent"
        border.width: 1; border.color: Theme.colors.divider
        SketchCanvas {
            id: inlineSketch
            anchors.fill: parent
            data: mb.sketchJson
            sourceWidth: mb.iw
        }
        Text {   // empty-canvas hint
            visible: inlineSketch.empty
            anchors.centerIn: parent
            text: "Sketch — open in tab to draw"
            color: Theme.colors.textSubtle
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }
    }

    // --- Image branch ---
    Image {
        id: img
        anchors.fill: parent
        visible: !mb.isVideo && !mb.isFile && !mb.isPdf && !mb.isSketch && !mb.suspended
        source: (mb.isVideo || mb.isFile || mb.isPdf || mb.isSketch) ? "" : mb.url
        asynchronous: true; cache: false
        fillMode: Image.PreserveAspectFit
        sourceSize.width: Math.round(mb.dispW * Screen.devicePixelRatio)
        smooth: true
        // Reveal fades; hide stays INSTANT (visible:false — the glitch never shows).
        onVisibleChanged: if (visible) imgFade.restart()
        NumberAnimation { id: imgFade; target: img; property: "opacity"
                          from: 0; to: 1; duration: 200; easing.type: Easing.OutCubic }
    }
    Rectangle {   // image loading / missing placeholder (shown until the image is Ready)
        anchors.fill: parent
        visible: mb.active && !mb.isVideo && !mb.isFile && !mb.isPdf && !mb.isSketch && !mb.suspended && img.status !== Image.Ready
        color: Theme.colors.surfaceRecess; radius: Theme.dim.radius   // image bed = inset well
        border.width: 1; border.color: Theme.colors.border
        Text {
            anchors.centerIn: parent
            text: (mb.url === "" || img.status === Image.Error) ? "image unavailable" : "loading…"
            color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
        }
    }

    // --- Video poster: a decoded frame thumbnail (no play overlay) ---
    Rectangle {   // backdrop while the frame decodes / if it fails. Page-colored so
                  // an alpha (ProRes 4444) poster composites its transparent regions
                  // over the page — exactly like the live video surface (whose
                  // fillColor is also the page surface). Keeps poster ↔ video alpha consistent.
        id: posterRect
        anchors.fill: parent
        visible: mb.isVideo && !mb.isActivePlayer && !mb.suspended
        color: Theme.colors.surface; radius: Theme.dim.radius
        clip: true
        onVisibleChanged: if (visible) posterFade.restart()
        NumberAnimation { id: posterFade; target: posterRect; property: "opacity"
                          from: 0; to: 1; duration: 200; easing.type: Easing.OutCubic }

        Image {
            id: poster
            anchors.fill: parent
            // The source URL carries the frame number, so cache:true is correct
            // (a new playhead = new URL) AND avoids re-decoding on scroll recycle.
            source: (mb.isVideo && !mb.isActivePlayer) ? mb._vframeSrc(mb.localPath, mb.posterFrame) : ""
            asynchronous: true; cache: true
            fillMode: Image.PreserveAspectFit
            sourceSize.width: Math.round(mb.dispW * Screen.devicePixelRatio)
            smooth: true
        }
        Rectangle {   // duration badge
            visible: mb.durMs > 0
            anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 8
            width: durText.width + 12; height: durText.height + 6; radius: 0
            color: Qt.rgba(0, 0, 0, 0.6)
            Text {
                id: durText
                anchors.centerIn: parent
                text: mb._fmtDur(mb.durMs)
                color: Theme.colors.textBright; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
        }
    }
}
