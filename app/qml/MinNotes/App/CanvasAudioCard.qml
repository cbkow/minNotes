// An AUDIO element's card (2.0 phase A9): a dropped sound file — name, duration chip, ALWAYS its full path,
// and ▸ / ❚❚ on the shared player (one thing plays at a time: a video or this). Squared, chrome tones, no
// accent. The waveform is a later upgrade.
import QtQuick

Rectangle {
    id: card
    required property var editor         // the Editor root (the shared player)
    required property var media          // the element's descriptor (json.media)
    required property string path        // its resolved local path ("" when the volume is unmapped)
    required property string elementId
    required property string key         // the media's anchor path (editor.audioPlayingKey compares to it)
    readonly property string kind: "audio"
    readonly property var durText: durLabel   // probe
    readonly property bool mine: editor.audioPlayingKey !== "" && editor.audioPlayingKey === key
    color: Theme.colors.surface
    border.width: 1; border.color: Theme.colors.border
    radius: 0

    function fmtDur(ms) {
        if (!ms || ms <= 0) return "–:––"
        const s = Math.round(ms / 1000), m = Math.floor(s / 60)
        return m + ":" + ((s % 60) < 10 ? "0" : "") + (s % 60)
    }

    Row {
        anchors.fill: parent; anchors.margins: 10; anchors.leftMargin: 12; anchors.rightMargin: 10; spacing: 10
        FlatButton {
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 28; implicitHeight: 28; iconSize: 16; radius: 0
            iconName: card.mine && card.editor.audioIsPlaying ? "pause" : "play"
            tooltip: card.mine && card.editor.audioIsPlaying ? qsTr("Pause") : qsTr("Play"); tooltipSide: "top"
            onClicked: card.editor.playAudioElement(card.elementId)
        }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 28 - 10 - durChip.width - 10; spacing: 2
            Text {
                width: parent.width; elide: Text.ElideMiddle
                text: card.media.name || qsTr("audio")
                color: Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
            }
            Text {
                width: parent.width; elide: Text.ElideMiddle
                text: card.path !== "" ? card.path : qsTr("(volume not mapped on this machine)")
                color: Theme.colors.textSubtle
                font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeMono
            }
        }
        Rectangle {
            id: durChip
            anchors.verticalCenter: parent.verticalCenter
            width: durLabel.implicitWidth + 12; height: 20; radius: 0
            color: Theme.colors.surfaceRaised; border.width: 1; border.color: Theme.colors.border
            Text { id: durLabel; anchors.centerIn: parent; text: card.fmtDur(card.media.durMs)
                   color: Theme.colors.textMuted; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeMono }
        }
    }
    HoverHandler { id: hover }
    FlatToolTip { parent: card; x: 0; y: card.height + 6; visible: hover.hovered && card.path !== ""; text: card.path }
}
