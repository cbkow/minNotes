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

    readonly property int _rev: blockModel.contentRevision
    readonly property string kind: active ? (mb._rev, blockModel.mediaKind(logicalRow)) : ""
    readonly property bool   isVideo: kind === "video"
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
    readonly property real dispW: iw > 0 ? Math.min(maxWidth, iw) : maxWidth

    implicitWidth:  dispW
    implicitHeight: (iw > 0 && ih > 0) ? Math.round(dispW * ih / iw) : Math.round(dispW * 0.5)

    function _fmtDur(ms) {
        if (ms <= 0) return ""
        var s = Math.round(ms / 1000)
        var m = Math.floor(s / 60)
        return m + ":" + ((s % 60) < 10 ? "0" : "") + (s % 60)
    }
    // image://videoframe/<base64url(path)>@<frame>  (base64url so / and spaces survive)
    function _vframeSrc(path, frame) {
        if (path === "") return ""
        var b = Qt.btoa(path).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
        return "image://videoframe/" + b + "@" + frame
    }

    // --- Image branch ---
    Image {
        id: img
        anchors.fill: parent
        visible: !mb.isVideo
        source: mb.isVideo ? "" : mb.url
        asynchronous: true; cache: false
        fillMode: Image.PreserveAspectFit
        sourceSize.width: Math.round(mb.dispW * Screen.devicePixelRatio)
        smooth: true
    }
    Rectangle {   // image loading / missing placeholder (shown until the image is Ready)
        anchors.fill: parent
        visible: mb.active && !mb.isVideo && img.status !== Image.Ready
        color: Theme.colors.surfaceHover; radius: Theme.dim.radius
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
        anchors.fill: parent
        visible: mb.isVideo && !mb.isActivePlayer
        color: Theme.colors.surface; radius: Theme.dim.radius
        clip: true

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
            width: durText.width + 12; height: durText.height + 6; radius: 4
            color: Qt.rgba(0, 0, 0, 0.6)
            Text {
                id: durText
                anchors.centerIn: parent
                text: mb._fmtDur(mb.durMs)
                color: "#f2f2f2"; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
        }
    }
}
