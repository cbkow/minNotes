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
    property bool spaceHeld: false // the editor's key handler: Space held = hand
    readonly property CanvasCamera cam: CanvasCamera { id: cam }

    // --- tools (the Inspector's draw tool): a stroke tool or the eraser arms the pen overlay; type
    // and select are the pointer (tap selects, drag moves, handles resize, empty-space drag pans) ---
    readonly property string tool: editor.inspector ? editor.inspector.drawTool : "type"
    readonly property bool drawing: !spaceHeld && (tool === "freehand" || tool === "rect" || tool === "oval" || tool === "arrow" || tool === "line" || tool === "eraser")
    readonly property bool textTool: !spaceHeld && tool === "text"

    // --- creating things on the canvas ---
    // Double-click on empty canvas (pointer tools): a new page there, opened for typing (ruling 6).
    function newPageAt(sx, sy) {
        const w = cam.screenToWorld(sx, sy)
        const id = canvas.addPageAt(w.x, w.y)
        if (id !== "") canvas.focusPage = id
        return id
    }
    // The text tool: a tap makes a text chip there and edits it; a double-tap on a chip edits it.
    function newTextAt(sx, sy) {
        const w = cam.screenToWorld(sx, sy)
        const size = editor.inspector && editor.inspector.drawTextSize ? editor.inspector.drawTextSize : 24
        const fill = editor.inspector ? editor.inspector.drawColor : "#ffffff"
        const id = canvas.addShapeElement(JSON.stringify({ shape: "text", text: "", fontSize: size, fill: "" + fill }), Qt.rect(w.x, w.y, Math.max(120, size * 6), size * 1.4 + Math.round(size * 0.6)))
        if (id === "") return ""
        select(id)
        textEditor.begin(id)
        return id
    }
    function editText(id) { if (canvas.elementKind(id) === "shape") { select(id); textEditor.begin(id) } }
    readonly property var textEditorItem: textEditor   // probe
    function emptyAt(sx, sy) { const w = cam.screenToWorld(sx, sy); return canvas.visibleElements(w.x, w.y, w.x + 0.01, w.y + 0.01).length === 0 }

    // --- selection ---
    property string selectedId: ""
    function select(id) { selectedId = id }
    function clearSelection() { selectedId = "" }
    readonly property string selectedKind: selectedId !== "" ? (canvas.elementsRevision, canvas.elementKind(selectedId)) : ""
    onSelectedKindChanged: if (selectedId !== "" && selectedKind === "") selectedId = ""   // the element went away
    Connections { target: canvas; function onDocumentChanged() { cv.clearSelection() } }
    // World-space moves and resizes of the selection (the handlers and the probe share these).
    function moveSelectedBy(dx, dy) {
        if (selectedId === "") return
        const r = canvas.elementRect(selectedId)
        canvas.setElementRect(selectedId, Qt.rect(r.x + dx, r.y + dy, r.width, r.height), "move:" + selectedId)
    }
    function resizeSelected(rect) { if (selectedId !== "") canvas.setElementRect(selectedId, rect, "resize:" + selectedId) }
    function edgeResize(start, dx, dy, ddx, ddy) {   // a handle's drag → the new world rect (min 8 px)
        let x = start.x, y = start.y, w = start.width, h = start.height
        if (dx < 0) { x = Math.min(start.x + ddx, start.x + start.width - 8); w = start.x + start.width - x }
        else if (dx > 0) w = Math.max(8, start.width + ddx)
        if (dy < 0) { y = Math.min(start.y + ddy, start.y + start.height - 8); h = start.y + start.height - y }
        else if (dy > 0) h = Math.max(8, start.height + ddy)
        return Qt.rect(x, y, w, h)
    }
    function deleteSelected() { if (selectedId !== "" && canvas.removeElement(selectedId)) selectedId = "" }
    function drillSelected() { if (selectedId !== "" && canvas.elementKind(selectedId) === "page") canvas.focusPage = selectedId }
    // The topmost ink element under a world point (the eraser).
    function inkAt(wx, wy) {
        const ids = canvas.visibleElements(wx, wy, wx + 0.01, wy + 0.01)
        for (let i = ids.length - 1; i >= 0; --i) if (canvas.elementKind(ids[i]) === "ink") return ids[i]
        return ""
    }

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
    onVisibleChanged: { if (!visible) clearSelection(); else if (!cam.userCam) fit() }
    Connections {   // elements come, go or grow under a camera the user hasn't taken yet
        target: canvas
        function onGeometryChanged() { if (cv.visible && !cam.userCam) cv.fit() }
        function onElementsChanged() { if (cv.visible && !cam.userCam) cv.fit() }
    }
    // The user's camera is the file's: remembered on save (canvas_meta), restored on open.
    Connections { target: cam; function onCameraChanged() { if (cam.userCam && cv.visible) camSave.restart() } }
    Timer { id: camSave; interval: 300; onTriggered: if (canvas.documentOpen && cam.userCam) canvas.setCamera(cam.panX, cam.panY, cam.zoom) }

    // Clicking dead space reclaims focus and clears the selection (the key swallow lives on the editor root).
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
    // Files dropped from outside the app land as elements at the drop point (a document imports
    // into a new page there — Main.qml answers canvas.importFileRequested). Several files stagger.
    DropArea {
        anchors.fill: parent; z: 5
        keys: ["text/uri-list"]
        onDropped: (drop) => {
            if (!drop.hasUrls || !canvas.documentOpen) return
            let last = ""
            for (let i = 0; i < drop.urls.length; ++i) {
                const w = cam.screenToWorld(drop.x, drop.y)
                const id = canvas.addMediaElementFromUrl(drop.urls[i].toString(), w.x + i * 32, w.y + i * 32)
                if (id !== "") last = id
            }
            if (last !== "") cv.select(last)
            drop.accept()
            cv.editor.forceActiveFocus()
        }
    }
    // Empty-canvas gestures: tap clears / makes a text chip (text tool); double-tap makes a page.
    TapHandler {
        enabled: !cv.drawing
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.DragThreshold
        onSingleTapped: (ev) => {
            if (!cv.emptyAt(ev.position.x, ev.position.y)) return
            if (cv.textTool) cv.newTextAt(ev.position.x, ev.position.y)
            else { textEditor.commit(); cv.clearSelection() }
        }
        onDoubleTapped: (ev) => { if (!cv.textTool && cv.emptyAt(ev.position.x, ev.position.y)) cv.newPageAt(ev.position.x, ev.position.y) }
    }
    // The selection frame (screen space), the text editor and the pen overlay sit above the world layer.
    CanvasSelectionFrame { id: selFrame; view: cv; z: 10 }
    CanvasTextEditor { id: textEditor; view: cv }
    CanvasInkCapture {
        id: capture
        anchors.fill: parent; z: 20
        active: cv.visible && cv.drawing
        tool: cv.tool
        color: cv.editor.inspector ? cv.editor.inspector.drawColor : "#ff0000"
        strokeWidth: cv.editor.inspector ? cv.editor.inspector.drawWidth : 6
        zoom: cam.zoom; panX: cam.panX; panY: cam.panY
        onStrokeCommitted: (json, rect) => { const id = canvas.addInkElement(json, rect); if (id !== "") cv.select(id) }
        onEraseAt: (wx, wy) => { const id = cv.inkAt(wx, wy); if (id !== "") { if (cv.selectedId === id) cv.clearSelection(); canvas.removeElement(id) } }
        InkNodeItem {   // the in-progress stroke, at this zoom
            anchors.fill: parent
            envelope: capture.liveEnvelope
            frameW: width; frameH: height; sourceW: width; sourceH: height
            pxScale: 1
        }
    }
    readonly property var inkCapture: capture   // probe
    readonly property var selectionFrame: selFrame

    DragHandler {   // left-drag on EMPTY canvas pans (an element's own handler moves it); middle-drag always
        target: null
        enabled: !cv.drawing
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        property real sx: 0
        property real sy: 0
        onActiveChanged: if (active) { sx = cam.panX; sy = cam.panY }
        onActiveTranslationChanged: if (active) cam.setPan(sx + activeTranslation.x, sy + activeTranslation.y)
    }
}
