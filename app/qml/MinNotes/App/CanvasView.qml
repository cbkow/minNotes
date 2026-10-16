// The canvas (B3): the document as cards on an infinite surface — a second VIEW over the same
// blocks, one document, read-only in phase 1. screen = world · cam.zoom + cam.pan. Cards are culled
// to the viewport plus half a viewport of overscan and rendered from a stable slot pool
// (canvasSlots, a second ViewportSlots keyed by CARD index, so a card that stays visible keeps its
// delegate while the camera moves). A fresh canvas fits the whole document and keeps re-fitting
// on resize until the user takes the camera (wheel, drag, pinch, zoom keys) — the sketch stage's rule.
import QtQuick

Rectangle {
    id: cv
    required property var editor
    color: Theme.colors.bgAlt      // the full-frame tabs' field tone
    clip: true                     // the world layer is unbounded: without this, cards paint over the rail and the tab strips
    property bool spaceHeld: false // the editor's key handler: Space held = hand (left-drag pans anyway in P1)
    readonly property CanvasCamera cam: CanvasCamera { id: cam }

    // --- culling → pool ---------------------------------------------------------------------
    // Gated on `visible`: a hidden canvas holds NO cards (its delegates key on the document-wide
    // contentRevision, so live cards behind the Document tab would put the old per-key fan-out back —
    // measured 2026-10-16: 338 → 3200 µs per key).
    readonly property var visibleCards: (blockModel.structureRevision, blockModel.layoutRevision, cam.rev, width, height, visible ? _cull() : [])
    function _cull() {
        if (width <= 0 || height <= 0) return []
        const tl = cam.screenToWorld(-width * 0.5, -height * 0.5), br = cam.screenToWorld(width * 1.5, height * 1.5)
        return blockModel.visibleCards(tl.x, tl.y, br.x, br.y)
    }
    readonly property int slotRev: canvasSlots.sync(visibleCards, cardPool.count)
    ListModel { id: cardPool }
    function sizePool() {
        const want = Math.min(Math.max(blockModel.cardCount(), 1), Math.ceil((visibleCards.length + 4) / 4) * 4)
        while (cardPool.count < want) cardPool.append({ slot: cardPool.count })
    }
    onVisibleCardsChanged: sizePool()
    Component.onCompleted: sizePool()

    // The live stagger's budget (B4): ONE FrameAnimation hands out kRowsPerFrame delegate creations a
    // frame across every card that needs rows (round robin, at least one each), so four cards entering
    // together cost the same frame time as one. A delegate is ~1 ms to create.
    readonly property int kRowsPerFrame: 12
    function needyCards() { const out = []; for (let i = 0; i < cardPool.count; ++i) { const c = cardRepeater.itemAt(i); if (c && c.needsRows) out.push(c) } return out }
    property bool anyNeedsRows: false
    function pollNeeds() { anyNeedsRows = needyCards().length > 0 }
    FrameAnimation {
        running: cv.visible && cv.anyNeedsRows
        onTriggered: {
            const cards = cv.needyCards()
            if (!cards.length) { cv.anyNeedsRows = false; return }
            const each = Math.max(1, Math.floor(cv.kRowsPerFrame / cards.length))
            for (const c of cards) c.feedRows(each)
        }
    }

    // Probe hooks (B10).
    function cardItemAt(slot) { return cardRepeater.itemAt(slot) }
    readonly property int poolCount: cardPool.count

    // --- camera -----------------------------------------------------------------------------
    function fit() { if (width > 0 && height > 0) cam.fitRect(blockModel.canvasBounds(), width, height, 48) }
    function zoomStep(dir) { cam.zoomAboutPoint(cam.zoom * Math.pow(Math.SQRT2, dir > 0 ? 1 : -1), width / 2, height / 2) }
    function zoomTo(v) { cam.zoomAboutPoint(v, width / 2, height / 2) }
    function captureCam() { return { x: cam.panX, y: cam.panY, z: cam.zoom, user: cam.userCam } }
    function restoreCam(c) { if (c && c.z !== undefined) cam.setCamera(c.x, c.y, c.z, c.user === true); else resetCam() }
    function resetCam() { cam.setUserCam(false); if (visible) fit() }
    onWidthChanged: if (visible && !cam.userCam) fit()
    onHeightChanged: if (visible && !cam.userCam) fit()
    onVisibleChanged: if (visible && !cam.userCam) fit()
    Connections {   // the document grows/shrinks under a camera the user hasn't taken yet
        target: blockModel
        function onLayoutChangedSpike() { if (cv.visible && !cam.userCam) cv.fit() }
        function onStructureChangedSpike() { if (cv.visible && !cam.userCam) cv.fit() }
    }

    // Clicking dead space reclaims focus (the key swallow lives on the editor root).
    MouseArea { anchors.fill: parent; z: -1; onClicked: cv.editor.forceActiveFocus() }

    Item {
        id: canvasLayer
        transform: [
            Scale { xScale: cam.zoom; yScale: cam.zoom },
            Translate { x: cam.panX; y: cam.panY }
        ]
        Repeater {
            id: cardRepeater
            model: cardPool
            delegate: CanvasCard {
                required property int index
                editor: cv.editor
                cam: cv.cam
                cardIndex: (cv.slotRev, canvasSlots.rowForSlot(index))
                onNeedsRowsChanged: if (needsRows) cv.anyNeedsRows = true
            }
        }
    }

    // --- input (P1): wheel + trackpad pan, ⌘-wheel / pinch zoom about the pointer, drag pans ---
    SmoothWheel {
        id: smoothWheel
        sink: cam
        onZoomRequested: (dir, x, y) => cam.zoomAboutPoint(cam.zoom * Math.pow(2, dir * 0.25), x, y)
    }
    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad   // see SmoothWheel: TouchPad is not optional
        onWheel: (event) => smoothWheel.handle(event)
    }
    PinchHandler {
        target: null
        property real startZoom: 1
        onActiveChanged: if (active) startZoom = cam.zoom
        onActiveScaleChanged: if (active) cam.zoomAboutPoint(startZoom * activeScale, centroid.position.x, centroid.position.y)
    }
    DragHandler {   // left-drag on the canvas pans in P1 (no card selection yet); middle-drag always
        target: null
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        property real sx: 0
        property real sy: 0
        onActiveChanged: if (active) { sx = cam.panX; sy = cam.panY }
        onActiveTranslationChanged: if (active) cam.setPan(sx + activeTranslation.x, sy + activeTranslation.y)
    }
}
