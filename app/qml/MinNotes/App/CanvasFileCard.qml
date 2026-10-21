// A FILE CHIP element's card (2.0 phase A6): an unrecognised file dropped on the canvas — its name and
// ALWAYS its full path (middle-elided; the whole path on hover). Squared, chrome tones, no accent.
import QtQuick

Rectangle {
    id: card
    required property var media          // the element's descriptor (json.media)
    required property string path        // its resolved local path ("" when the volume is unmapped)
    readonly property string kind: "file"
    readonly property var pathText: pathLabel   // probe
    color: Theme.colors.surface
    border.width: 1; border.color: Theme.colors.border
    radius: Theme.dim.radius

    Row {
        anchors.fill: parent; anchors.margins: 10; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 10
        Icon { anchors.verticalCenter: parent.verticalCenter; name: "file"; size: 22; color: Theme.colors.textMuted }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 32; spacing: 2
            Text {
                width: parent.width; elide: Text.ElideMiddle
                text: card.media.name || qsTr("file")
                color: Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
            }
            Text {
                id: pathLabel
                width: parent.width; elide: Text.ElideMiddle
                text: card.path !== "" ? card.path : qsTr("(volume not mapped on this machine)")
                color: Theme.colors.textSubtle
                font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeMono
            }
        }
    }
    HoverHandler { id: hover }
    FlatToolTip { parent: card; x: 0; y: card.height + 6; visible: hover.hovered && card.path !== ""; text: card.path }
}
