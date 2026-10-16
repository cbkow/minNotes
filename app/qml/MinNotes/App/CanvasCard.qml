// One card on the canvas (B3): a run of blocks (a heading to the next heading, or to a closing
// divider) drawn at page width in world space, with the page's REAL block delegates (BlockView)
// driven by a CanvasEditorFacade instead of the editor. Pooled like the page's delegates:
// `cardIndex` is -1 for an idle slot. VISIBILITY RULE (see BlockView.qml): an idle card rides
// opacity, never `visible`.
// LOD (B4, first cut): below kGhostZoom the card is a frame with its title and no delegates;
// above it the delegates are live. (The raster tier lands with the budgets.)
import QtQuick

Rectangle {
    id: card
    required property var editor
    required property var cam
    property int cardIndex: -1
    readonly property bool active: cardIndex >= 0 && cardIndex < (blockModel.structureRevision, blockModel.cardCount())
    // The run and its world rect. layoutRevision: heights settle; structureRevision: the card set.
    readonly property int head: active ? (blockModel.structureRevision, blockModel.cardHeadAt(cardIndex)) : -1
    readonly property int last: active ? (blockModel.structureRevision, blockModel.cardLastAt(cardIndex)) : -1
    readonly property int runLength: active ? last - head + 1 : 0
    readonly property rect r: active ? (blockModel.layoutRevision, blockModel.structureRevision, blockModel.cardRect(cardIndex))
                                     : Qt.rect(0, 0, 0, 0)
    x: r.x; y: r.y; width: r.width; height: r.height
    opacity: active ? 1 : 0
    enabled: active
    clip: true
    color: Theme.colors.surface
    border.width: 1; border.color: Theme.colors.border
    radius: 0

    readonly property real inset: 24                 // == BlockModel::kCardInset
    readonly property int kCardMaxRows: 120           // a giant run shows this many blocks and a footer
    readonly property real kGhostZoom: 0.10
    readonly property bool ghost: cam.zoom < kGhostZoom
    // The first block sits at the inset: BlockView.y = yForRow(row) − originY.
    readonly property real originY: head >= 0 ? (blockModel.layoutRevision, blockModel.yForRow(head)) - inset : 0

    readonly property CanvasEditorFacade facade: CanvasEditorFacade { editor: card.editor; inset: card.inset }

    Repeater {
        model: card.active && !card.ghost ? Math.min(card.kCardMaxRows, card.runLength) : 0
        delegate: BlockView {
            required property int index
            editor: card.facade
            logicalRow: card.head + index
            originY: card.originY
            measures: true
        }
    }
    Text {   // ghost: the head block's text as the title
        x: card.inset; y: card.inset; width: card.width - 2 * card.inset
        opacity: card.ghost && card.active ? 1 : 0
        text: card.head >= 0 ? (blockModel.contentRevision, blockModel.contentForRow(card.head)) : ""
        elide: Text.ElideRight; maximumLineCount: 1
        color: Theme.colors.text
        font.family: Theme.font.body; font.pixelSize: 20; font.bold: true
    }
    Text {   // the row cap's footer
        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: card.inset
        opacity: card.active && !card.ghost && card.runLength > card.kCardMaxRows ? 1 : 0
        text: card.runLength > card.kCardMaxRows ? (card.runLength - card.kCardMaxRows) + " more blocks" : ""
        color: Theme.colors.textSubtle
        font.family: Theme.font.body; font.pixelSize: Theme.font.sizeChrome
    }
}
