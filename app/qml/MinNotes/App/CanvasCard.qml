// One card on the canvas (B3): a run of blocks (a heading to the next heading, or to a closing
// divider) drawn at page width in world space. Phase-1 day-3 skeleton: the card's frame and its
// title; the real block delegates land next (CanvasEditorFacade + BlockView, B3/B4).
// Pooled like the page's delegates: `cardIndex` is -1 for an idle slot. VISIBILITY RULE (see
// BlockView.qml): an idle card rides opacity, never `visible`.
import QtQuick

Rectangle {
    id: card
    required property var editor
    property int cardIndex: -1
    readonly property bool active: cardIndex >= 0 && cardIndex < (blockModel.structureRevision, blockModel.cardCount())
    // The run and its world rect. layoutRevision: heights settle; structureRevision: the card set.
    readonly property int head: active ? (blockModel.structureRevision, blockModel.cardHeadAt(cardIndex)) : -1
    readonly property int last: active ? (blockModel.structureRevision, blockModel.cardLastAt(cardIndex)) : -1
    readonly property rect r: active ? (blockModel.layoutRevision, blockModel.structureRevision, blockModel.cardRect(cardIndex))
                                     : Qt.rect(0, 0, 0, 0)
    x: r.x; y: r.y; width: r.width; height: r.height
    opacity: active ? 1 : 0
    enabled: active
    color: Theme.colors.surface
    border.width: 1; border.color: Theme.colors.border
    radius: 0

    readonly property real inset: 24   // == BlockModel::kCardInset
    Text {   // the head block's text — the card's title until its blocks render
        x: card.inset; y: card.inset; width: card.width - 2 * card.inset
        text: card.head >= 0 ? (blockModel.contentRevision, blockModel.contentForRow(card.head)) : ""
        elide: Text.ElideRight; maximumLineCount: 1
        color: Theme.colors.text
        font.family: Theme.font.body; font.pixelSize: 20; font.bold: true
    }
    Text {   // row count, bottom right (skeleton diagnostics; the blocks replace it)
        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: card.inset
        text: card.active ? (card.last - card.head + 1) + (card.last - card.head === 0 ? " block" : " blocks") : ""
        color: Theme.colors.textSubtle
        font.family: Theme.font.body; font.pixelSize: Theme.font.sizeChrome
    }
}
