// A BOOKMARK element's card (2.0 phase A7): a dropped / pasted URL — favicon, the page's title (the host until
// the fetch lands), the host. Open opens it in the browser. Squared, chrome tones, no accent.
import QtQuick

Rectangle {
    id: card
    required property var media          // the element's descriptor (json.media)
    readonly property string kind: "bookmark"
    readonly property var hostText: hostLabel   // probe
    color: Theme.colors.surface
    border.width: 1; border.color: Theme.colors.border
    radius: 0

    Row {
        anchors.fill: parent; anchors.margins: 12; spacing: 12
        Item {
            anchors.verticalCenter: parent.verticalCenter
            width: 20; height: 20
            Image {
                id: favicon
                anchors.fill: parent
                source: card.media.icon ? canvas.resolveMediaUrl(card.media.icon) : ""
                asynchronous: true; cache: true; smooth: true
                fillMode: Image.PreserveAspectFit
                sourceSize.width: 40; sourceSize.height: 40
                visible: status === Image.Ready
            }
            Icon { anchors.centerIn: parent; name: "globe"; size: 18; color: Theme.colors.textMuted; visible: !favicon.visible }
        }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 32; spacing: 3
            Text {
                width: parent.width; elide: Text.ElideRight
                text: card.media.title || card.media.host || card.media.src || ""
                color: Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
            }
            Text {
                id: hostLabel
                width: parent.width; elide: Text.ElideMiddle
                text: card.media.host || ""
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
            }
        }
    }
    HoverHandler { id: hover }
    FlatToolTip { parent: card; x: 0; y: card.height + 6; visible: hover.hovered && !!card.media.src; text: card.media.src || "" }
}
