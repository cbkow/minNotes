// One card on the canvas (B3): a run of blocks (a heading to the next heading, or to a closing
// divider) drawn at page width in world space, with the page's REAL block delegates (BlockView)
// driven by a CanvasEditorFacade instead of the editor. Pooled like the page's delegates:
// `cardIndex` is -1 for an idle slot. VISIBILITY RULE (see BlockView.qml): an idle card rides
// opacity, never `visible`.
//
// LOD (B4), three tiers held with hysteresis in `tier` (set from the camera, never a pure binding):
//   0 ghost  (zoom < 0.10):  the frame and the title.
//   1 greek  (0.10 – 0.40):  the title and one bar per block from the model's own row geometry —
//                             a faithful silhouette at a few px per line for ONE item per row (a live
//                             delegate is ~30 items), no texture memory, no text layout. Replaces the
//                             planned raster-layer tier for phase 1: text under 0.4 is unreadable
//                             anyway and the heading stays real.
//   2 live   (≥ 0.40):        the delegates, created a few rows per frame (the stagger) so a zoom
//                             across the threshold never lands a 600-delegate hitch on one frame.
// Live holds down to 0.32, greek down to 0.08 (the dead bands).
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
    readonly property int kGreekMaxRows: 400
    readonly property int kRowsPerFrame: 8            // the live stagger
    // The first block sits at the inset: BlockView.y = yForRow(row) − originY.
    readonly property real originY: head >= 0 ? (blockModel.layoutRevision, blockModel.yForRow(head)) - inset : 0

    // --- LOD tier with hysteresis ---
    property int tier: 0
    function retier() {
        const z = cam.zoom
        let t = tier
        if (z >= 0.40) t = 2
        else if (z >= 0.10) t = (t === 2 && z > 0.32) ? 2 : 1
        else t = (t >= 1 && z > 0.08) ? 1 : 0
        if (t !== tier) tier = t
    }
    Component.onCompleted: retier()
    Connections { target: card.cam; function onCameraChanged() { card.retier() } }

    // --- live: the real delegates, staggered ---
    readonly property int wantRows: active && tier === 2 ? Math.min(kCardMaxRows, runLength) : 0
    property int liveRows: 0
    onWantRowsChanged: { if (wantRows === 0) liveRows = 0; else if (liveRows > wantRows) liveRows = wantRows }
    FrameAnimation {
        running: card.liveRows < card.wantRows
        onTriggered: card.liveRows = Math.min(card.wantRows, card.liveRows + card.kRowsPerFrame)
    }
    readonly property CanvasEditorFacade facade: CanvasEditorFacade { editor: card.editor; inset: card.inset }
    Repeater {
        model: card.liveRows
        delegate: BlockView {
            required property int index
            editor: card.facade
            logicalRow: card.head + index
            originY: card.originY
            measures: true
        }
    }

    // --- greek: one bar per block (also under a live ramp's not-yet-created rows) ---
    Repeater {
        model: card.active && card.tier === 1 ? Math.min(card.kGreekMaxRows, card.runLength) : 0
        delegate: Rectangle {
            required property int index
            readonly property int row: card.head + index
            readonly property int t: (blockModel.structureRevision, blockModel.typeForRow(row))
            readonly property var lane: (blockModel.structureRevision, card.editor.laneOf(row))
            readonly property int chars: t === 3 || t === 6 || t === 10 ? 0 : (blockModel.contentRevision, blockModel.contentForRow(row).length)
            readonly property real rowH: (blockModel.layoutRevision, blockModel.heightForRow(row))
            x: card.inset + lane.x
            y: (blockModel.layoutRevision, blockModel.yForRow(row)) - card.originY + (t === 6 ? rowH / 2 : 6)
            height: t === 10 ? 0 : t === 6 ? 1 : Math.max(2, rowH - 12)
            width: t === 10 ? 0 : (t === 2 || t === 3 || t === 6) ? lane.w : Math.max(24, Math.min(lane.w, chars * 7.2))   // ~7 px a glyph at body size
            radius: 0
            color: t === 1 ? Theme.colors.textMuted : t === 2 ? Theme.colors.codeBg : t === 3 ? Theme.colors.surfaceHover
                 : t === 6 ? Theme.colors.divider : Theme.colors.divider
        }
    }

    Text {   // ghost and greek: the head block's text as the title
        x: card.inset; y: card.inset; width: card.width - 2 * card.inset
        opacity: card.active && card.tier < 2 ? 1 : 0
        text: card.head >= 0 && card.tier < 2 ? (blockModel.contentRevision, blockModel.contentForRow(card.head)) : ""
        elide: Text.ElideRight; maximumLineCount: 1
        color: Theme.colors.text
        font.family: Theme.font.body; font.pixelSize: card.tier === 1 ? 28 : 20; font.bold: true
    }
    Text {   // the row cap's footer
        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: card.inset
        opacity: card.active && card.tier === 2 && card.runLength > card.kCardMaxRows ? 1 : 0
        text: card.runLength > card.kCardMaxRows ? (card.runLength - card.kCardMaxRows) + " more blocks" : ""
        color: Theme.colors.textSubtle
        font.family: Theme.font.body; font.pixelSize: Theme.font.sizeChrome
    }
}
