// The canvas (2.0): the FILE's elements on an infinite surface — pages (their real block delegates
// from their own models), and in the next sub-steps media, ink, shapes and connectors. screen =
// world · cam.zoom + cam.pan. Elements are culled to the viewport plus half a viewport of overscan
// and rendered from a stable slot pool (canvasSlots, a ViewportSlots keyed by ELEMENT index, so an
// element that stays visible keeps its delegate while the camera moves). The camera is the file's
// (canvas_meta on save); a canvas without one fits every element and keeps re-fitting on resize
// until the user takes the camera (wheel, drag, pinch, zoom keys).
import QtQuick

Rectangle {
    id: cv
    required property var editor
    color: Theme.colors.bgAlt      // the full-frame tabs' field tone
    clip: true                     // the world layer is unbounded: without this, elements paint over the rail and the tab strips
    property bool spaceHeld: false // the editor's key handler: Space held = hand (left-drag pans anyway)
    readonly property CanvasCamera cam: CanvasCamera { id: cam }

    // --- culling → pool ---------------------------------------------------------------------
    // Gated on `visible`: a hidden canvas holds NO elements (page cards carry live delegates).
    readonly property var visibleElements: (canvas.elementsRevision, canvas.geometryRevision, cam.rev, width, height, visible ? _cull() : [])
    function _cull() {
        if (width <= 0 || height <= 0 || !canvas.documentOpen) return []
        const tl = cam.screenToWorld(-width * 0.5, -height * 0.5), br = cam.screenToWorld(width * 1.5, height * 1.5)
        return canvas.visibleElementIndices(tl.x, tl.y, br.x, br.y)
    }
    readonly property int slotRev: canvasSlots.sync(visibleElements, elementPool.count)
    ListModel { id: elementPool }
    function sizePool() {
        const want = Math.min(Math.max(canvas.elementCount(), 1), Math.ceil((visibleElements.length + 4) / 4) * 4)
        while (elementPool.count < want) elementPool.append({ slot: elementPool.count })
    }
    onVisibleElementsChanged: sizePool()
    Component.onCompleted: sizePool()

    // The live stagger's budget (B4): ONE FrameAnimation hands out kRowsPerFrame delegate creations a
    // frame across every page that needs rows (round robin, at least one each).
    readonly property int kRowsPerFrame: 12
    function needyPages() { const out = []; for (let i = 0; i < elementPool.count; ++i) { const c = elementRepeater.itemAt(i); if (c && c.needsRows) out.push(c) } return out }
    property bool anyNeedsRows: false
    FrameAnimation {
        running: cv.visible && cv.anyNeedsRows
        onTriggered: {
            const pages = cv.needyPages()
            if (!pages.length) { cv.anyNeedsRows = false; return }
            const each = Math.max(1, Math.floor(cv.kRowsPerFrame / pages.length))
            for (const c of pages) c.feedRows(each)
        }
    }

    // Probe hooks.
    function elementItemAt(slot) { return elementRepeater.itemAt(slot) }
    readonly property int poolCount: elementPool.count

    // --- camera -----------------------------------------------------------------------------
    function fit() { if (width > 0 && height > 0) cam.fitRect(canvas.canvasBounds(), width, height, 48) }
    function zoomStep(dir) { cam.zoomAboutPoint(cam.zoom * Math.pow(Math.SQRT2, dir > 0 ? 1 : -1), width / 2, height / 2) }
    function zoomTo(v) { cam.zoomAboutPoint(v, width / 2, height / 2) }
    function captureCam() { return { x: cam.panX, y: cam.panY, z: cam.zoom, user: cam.userCam } }
    function restoreCam(c) { if (c && c.z !== undefined) cam.setCamera(c.x, c.y, c.z, c.user === true); else resetCam() }
    // No in-session state for this tab: the FILE's camera if it has one, else fit everything.
    function resetCam() {
        if (canvas.documentOpen && canvas.hasCamera) { const c = canvas.camera(); cam.setCamera(c.x, c.y, c.z, true); return }
        cam.setUserCam(false); if (visible) fit()
    }
    onWidthChanged: if (visible && !cam.userCam) fit()
    onHeightChanged: if (visible && !cam.userCam) fit()
    onVisibleChanged: if (visible && !cam.userCam) fit()
    Connections {   // elements come, go or grow under a camera the user hasn't taken yet
        target: canvas
        function onGeometryChanged() { if (cv.visible && !cam.userCam) cv.fit() }
        function onElementsChanged() { if (cv.visible && !cam.userCam) cv.fit() }
    }
    // The user's camera is the file's: remembered on save (canvas_meta), restored on open.
    Connections { target: cam; function onCameraChanged() { if (cam.userCam && cv.visible) camSave.restart() } }
    Timer { id: camSave; interval: 300; onTriggered: if (canvas.documentOpen && cam.userCam) canvas.setCamera(cam.panX, cam.panY, cam.zoom) }

    // Clicking dead space reclaims focus (the key swallow lives on the editor root).
    MouseArea { anchors.fill: parent; z: -1; onClicked: cv.editor.forceActiveFocus() }

    Item {
        id: canvasLayer
        transform: [
            Scale { xScale: cam.zoom; yScale: cam.zoom },
            Translate { x: cam.panX; y: cam.panY }
        ]
        Repeater {
            id: elementRepeater
            model: elementPool
            delegate: CanvasElement {
                required property int index
                editor: cv.editor
                cam: cv.cam
                elementIndex: (cv.slotRev, canvasSlots.rowForSlot(index))
                onNeedsRowsChanged: if (needsRows) cv.anyNeedsRows = true
            }
        }
    }

    // --- input: wheel + trackpad pan, ⌘-wheel / pinch zoom about the pointer, drag pans ---
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
    DragHandler {   // left-drag on the canvas pans (selection and moves come with the next sub-step); middle-drag always
        target: null
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        property real sx: 0
        property real sy: 0
        onActiveChanged: if (active) { sx = cam.panX; sy = cam.panY }
        onActiveTranslationChanged: if (active) cam.setPan(sx + activeTranslation.x, sy + activeTranslation.y)
    }
}
