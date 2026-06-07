import QtQuick

// Inline render for a Media block (images in M1; video later). A plain async
// Image from the model's resolved file:// URL — Qt covers the v1 formats. The
// intrinsic dims come from the model, so the height is reserved BEFORE the image
// loads (no layout jump; the value also feeds the Fenwick index). Small images
// show at intrinsic width (never upscaled); larger ones cap at the measure.
Item {
    id: mb
    property int  logicalRow: -1
    property bool active: false
    property real maxWidth: 760

    readonly property int _rev: blockModel.contentRevision
    readonly property string url: active ? (mb._rev, blockModel.mediaUrl(logicalRow)) : ""
    readonly property int iw: active ? (mb._rev, blockModel.mediaW(logicalRow)) : 0
    readonly property int ih: active ? (mb._rev, blockModel.mediaH(logicalRow)) : 0
    readonly property real dispW: iw > 0 ? Math.min(maxWidth, iw) : maxWidth

    implicitWidth:  dispW
    implicitHeight: (iw > 0 && ih > 0) ? Math.round(dispW * ih / iw) : Math.round(dispW * 0.5)

    Image {
        id: img
        anchors.fill: parent
        source: mb.url
        asynchronous: true; cache: false
        fillMode: Image.PreserveAspectFit
        sourceSize.width: Math.round(mb.dispW * Screen.devicePixelRatio)
        smooth: true
    }
    Rectangle {   // loading / missing placeholder (shown until the image is Ready)
        anchors.fill: parent
        visible: mb.active && img.status !== Image.Ready
        color: Theme.colors.surfaceHover; radius: Theme.dim.radius
        border.width: 1; border.color: Theme.colors.border
        Text {
            anchors.centerIn: parent
            text: (mb.url === "" || img.status === Image.Error) ? "image unavailable" : "loading…"
            color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
        }
    }
}
