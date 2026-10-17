// A PAGE element on the canvas (2.0 stage 2 step 7): the page's REAL block delegates (BlockView)
// rendered from its OWN model inside a PageContextLoader (the child context's `blockModel` is the
// page's model, its `spell` a stub), driven by a CanvasEditorFacade instead of the editor. The
// page lays out at the ELEMENT's measure (its rect less the insets) while it shows here; the page
// view re-tells its own width when it takes the page (two widths, one layout index — plan §2.3).
// Double-click (CanvasElement's handler) drills in (canvas.focusPage = this page); Esc on the page comes back.
//
// LOD (B4), three tiers held with hysteresis in `tier` (set from the camera, never a pure binding):
//   0 ghost  (zoom < 0.10):  the frame and the title.
//   1 greek  (0.10 – 0.40):  the title and one bar per block from the model's own row geometry.
//   2 live   (≥ 0.40):        the delegates, created a few rows per frame (the stagger) so a zoom
//                             across the threshold never lands a 600-delegate hitch on one frame.
// Live holds down to 0.32, greek down to 0.08 (the dead bands).
import QtQuick

Rectangle {
    id: page
    required property var editor
    required property var cam
    property string elementId: ""
    readonly property bool active: elementId !== ""
    readonly property var pageModel: active ? canvas.pageModel(elementId) : null
    anchors.fill: parent
    clip: true
    color: Theme.colors.surface
    border.width: 1; border.color: Theme.colors.border
    radius: 0

    readonly property real inset: 24                        // == CanvasDocument::kPageInset
    readonly property real measure: Math.max(320, width - 2 * inset)
    readonly property real originY: -inset                  // BlockView.y = yForRow(row) − originY: the first block sits at the inset
    // The page's model lays out at this card's measure (the file's measure for the page) — once the
    // slot is really sized (an idle slot's rect is empty; its width must never reach a model).
    // Deferred (Qt.callLater) so every binding has settled: when a slot idles, the width-derived chain
    // can fire before the element-id chain, and a stale `active` would let an idle slot's 320 through.
    // The measure comes from the ELEMENT's rect, the truth, not the item's width.
    function tellWidth() {
        if (!active || !pageModel || !canvas.documentOpen) return
        const w = canvas.elementRect(elementId).width - 2 * inset
        if (w >= 320) pageModel.setContentWidth(w)
    }
    function tellWidthLater() { Qt.callLater(tellWidth) }
    onMeasureChanged: tellWidthLater()
    onPageModelChanged: tellWidthLater()
    onActiveChanged: tellWidthLater()
    // The element's height follows the laid-out model (a cache for culling / exporters; no undo).
    Connections {
        target: page.pageModel
        function onLayoutChangedSpike() { heightSync.restart() }
        function onStructureChangedSpike() { heightSync.restart() }
    }
    Timer { id: heightSync; interval: 200; onTriggered: if (page.active && page.pageModel) canvas.cachePageHeight(page.elementId, page.pageModel.totalHeight + 2 * page.inset) }

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
    Connections { target: page.cam; function onCameraChanged() { page.retier() } }

    // Forwards for the view's row budget and the probe.
    readonly property var body: loader.item
    readonly property bool needsRows: body ? body.needsRows : false
    function feedRows(n) { if (body) body.feedRows(n) }
    readonly property int liveRows: body ? body.liveRows : 0
    readonly property int wantRows: body ? body.wantRows : 0
    readonly property var facade: body ? body.facade : null
    function delegateAt(i) { return body ? body.delegateAt(i) : null }

    QtObject {   // a background page never shows the FOCUSED page's spelling issues
        id: spellStub
        readonly property bool checkSpelling: false
        readonly property bool checkGrammar: false
        readonly property int revision: 0
        function issuesForRow(row) { return [] }
    }
    PageContextLoader {
        id: loader
        anchors.fill: parent
        active: page.active && page.tier > 0
        pageModel: page.pageModel
        extraContext: ({ spell: spellStub })
        sourceComponent: bodyComponent
    }
    Component {
        id: bodyComponent
        Item {   // `blockModel` here is THE PAGE's model (the loader's child context)
            id: body
            anchors.fill: parent
            readonly property int runLength: blockModel.count
            readonly property int kCardMaxRows: 120           // a giant page shows this many blocks and a footer
            readonly property int kGreekMaxRows: 400
            // --- live: the real delegates, staggered through a ListModel (appends touch only the new rows) ---
            readonly property int wantRows: page.tier === 2 ? Math.min(kCardMaxRows, runLength) : 0
            readonly property int liveRows: liveModel.count
            readonly property bool needsRows: liveModel.count < wantRows
            ListModel { id: liveModel }
            onWantRowsChanged: if (wantRows < liveModel.count) liveModel.clear()
            function feedRows(n) { const want = Math.min(wantRows, liveModel.count + n); while (liveModel.count < want) liveModel.append({ idx: liveModel.count }) }
            readonly property CanvasEditorFacade facade: CanvasEditorFacade { editor: page.editor; inset: page.inset; measure: page.measure }
            function delegateAt(i) { return blockRepeater.itemAt(i) }
            Repeater {
                id: blockRepeater
                model: liveModel
                delegate: BlockView {
                    required property int idx
                    editor: body.facade
                    logicalRow: idx
                    originY: page.originY
                    measures: true
                }
            }
            // --- greek: one bar per block (also under a live ramp's not-yet-created rows) ---
            Repeater {
                model: page.tier === 1 ? Math.min(body.kGreekMaxRows, body.runLength) : 0
                delegate: Rectangle {
                    required property int index
                    readonly property int row: index
                    readonly property int t: (blockModel.structureRevision, blockModel.typeForRow(row))
                    readonly property var lane: (blockModel.structureRevision, body.facade.laneOf(row))
                    readonly property int chars: t === 3 || t === 6 || t === 10 ? 0 : (blockModel.contentRevision, blockModel.contentForRow(row).length)
                    readonly property real rowH: (blockModel.layoutRevision, blockModel.heightForRow(row))
                    x: page.inset + lane.x
                    y: (blockModel.layoutRevision, blockModel.yForRow(row)) - page.originY + (t === 6 ? rowH / 2 : 6)
                    height: t === 10 ? 0 : t === 6 ? 1 : Math.max(2, rowH - 12)
                    width: t === 10 ? 0 : (t === 2 || t === 3 || t === 6) ? lane.w : Math.max(24, Math.min(lane.w, chars * 7.2))   // ~7 px a glyph at body size
                    radius: 0
                    color: t === 1 ? Theme.colors.textMuted : t === 2 ? Theme.colors.codeBg : t === 3 ? Theme.colors.surfaceHover : Theme.colors.divider
                }
            }
            Text {   // the row cap's footer
                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: page.inset
                opacity: page.tier === 2 && body.runLength > body.kCardMaxRows ? 1 : 0
                text: body.runLength > body.kCardMaxRows ? (body.runLength - body.kCardMaxRows) + " more blocks — open the page" : ""
                color: Theme.colors.textSubtle
                font.family: Theme.font.body; font.pixelSize: Theme.font.sizeChrome
            }
        }
    }

    Text {   // ghost and greek: the page's title
        x: page.inset; y: page.inset; width: page.width - 2 * page.inset
        opacity: page.active && page.tier < 2 ? 1 : 0
        text: page.active && page.tier < 2 ? (canvas.pagesRevision, page.pageModel ? page.pageModel.contentRevision : 0, canvas.pageTitle(page.elementId)) : ""
        elide: Text.ElideRight; maximumLineCount: 1
        color: Theme.colors.text
        font.family: Theme.font.body; font.pixelSize: page.tier === 1 ? 28 : 20; font.bold: true
    }
}
