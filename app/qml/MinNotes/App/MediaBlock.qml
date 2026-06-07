import QtQuick

// Inline render for a Media block. Images show a plain async Image; video shows
// a dark poster plate with a play affordance (v1 extracts no real frame). The
// intrinsic dims come from the model, so height is reserved BEFORE anything
// loads (no layout jump; the value also feeds the Fenwick index). Small images
// show at intrinsic width (never upscaled); larger ones cap at the measure.
//
// The video poster has NO MouseArea: it sits beneath the editor's central mouse
// layer, which routes a click on a video block to Editor.playVideo(). Once
// playing, the single root-owned VideoSurfaceItem overlays this rect exactly.
Item {
    id: mb
    property int  logicalRow: -1
    property bool active: false
    property real maxWidth: 760

    readonly property int _rev: blockModel.contentRevision
    readonly property string kind: active ? (mb._rev, blockModel.mediaKind(logicalRow)) : ""
    readonly property bool   isVideo: kind === "video"
    readonly property string url: active ? (mb._rev, blockModel.mediaUrl(logicalRow)) : ""
    readonly property int iw: active ? (mb._rev, blockModel.mediaW(logicalRow)) : 0
    readonly property int ih: active ? (mb._rev, blockModel.mediaH(logicalRow)) : 0
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

    // --- Video poster ---
    Rectangle {
        anchors.fill: parent
        visible: mb.isVideo
        color: "#0d0d0f"; radius: Theme.dim.radius
        border.width: 1; border.color: Theme.colors.border
        clip: true

        Rectangle {   // play button
            anchors.centerIn: parent
            width: 66; height: 66; radius: 33
            color: Qt.rgba(0, 0, 0, 0.40)
            border.width: 2; border.color: "#f2f2f2"
            Icon {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: 2   // optical-center the triangle
                name: "play"; weight: "fill"; size: 30; color: "#f2f2f2"
            }
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
