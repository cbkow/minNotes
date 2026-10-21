// A FOLDER element's card (2.0 phase A6): the folder's name, how many things it holds and a few of their
// names, and ALWAYS its full path (middle-elided; the whole path on hover) — a path a reader can copy is the
// baseline, the ufb link the extra (plan §2.10 "paths are first-class"). Squared, chrome tones, no accent.
import QtQuick

Rectangle {
    id: card
    required property var media          // the element's descriptor (json.media)
    required property string path        // its resolved local path ("" when the volume is unmapped)
    readonly property string kind: "folder"
    readonly property var pathText: pathLabel   // probe
    color: Theme.colors.surface
    border.width: 1; border.color: Theme.colors.border
    radius: Theme.dim.radius

    Row {
        anchors.fill: parent; anchors.margins: 12; spacing: 12
        Icon { anchors.verticalCenter: parent.verticalCenter; name: "folder"; size: 28; color: Theme.colors.textMuted }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 40; spacing: 3
            Text {
                width: parent.width; elide: Text.ElideRight
                text: card.media.name || qsTr("Folder")
                color: Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
            }
            Text {
                width: parent.width; elide: Text.ElideRight
                text: {
                    const n = card.media.count || 0, capped = !!card.media.countCapped
                    const items = (capped ? n.toLocaleString() + "+" : n.toLocaleString()) + (n === 1 && !capped ? qsTr(" item") : qsTr(" items"))
                    const names = (card.media.entries || []).join(", ")
                    return names ? items + " · " + names + (n > (card.media.entries || []).length ? ", …" : "") : items
                }
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
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
