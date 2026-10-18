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
    readonly property bool pageTool: !spaceHeld && tool === "page"
    readonly property bool handTool: spaceHeld || tool === "hand"
    readonly property bool pointerTool: !spaceHeld && (tool === "select" || tool === "type")

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

    // --- selection: a SET of ids; selectedId is the one id when exactly one is selected ---
    property var selectedIds: []
    readonly property string selectedId: selectedIds.length === 1 ? selectedIds[0] : ""
    readonly property int selectionCount: selectedIds.length
    property int elementDrags: 0   // probe diagnostics: element drags / view pans that activated
    property int pans: 0
    function select(id) { selectedIds = id !== "" ? [id] : [] }
    function selectMany(ids) { selectedIds = ids.slice() }
    function toggleSelect(id) { const i = selectedIds.indexOf(id); const a = selectedIds.slice(); if (i >= 0) a.splice(i, 1); else a.push(id); selectedIds = a }
    function isSelected(id) { return selectedIds.indexOf(id) >= 0 }
    function clearSelection() { if (selectedIds.length) selectedIds = [] }
    function selectionRect() {   // the union of the selected elements' world rects
        let r = null
        for (const id of selectedIds) {
            const e = canvas.elementRect(id)
            if (e.width <= 0 && e.height <= 0) continue
            r = r ? Qt.rect(Math.min(r.x, e.x), Math.min(r.y, e.y), Math.max(r.x + r.width, e.x + e.width) - Math.min(r.x, e.x), Math.max(r.y + r.height, e.y + e.height) - Math.min(r.y, e.y)) : e
        }
        return r || Qt.rect(0, 0, 0, 0)
    }
    Connections {   // elements that went away leave the selection
        target: canvas
        function onElementsChanged() { const keep = cv.selectedIds.filter(id => canvas.elementKind(id) !== ""); if (keep.length !== cv.selectedIds.length) cv.selectedIds = keep }
    }
    Connections { target: canvas; function onDocumentChanged() { cv.clearSelection() } }
    // World-space moves and resizes of the selection (the handlers and the probe share these). A move
    // of several elements is one history row (a batch); a drag opens the batch at its start.
    function moveSelectedBy(dx, dy) {
        if (!selectedIds.length) return
        canvas.beginBatch(qsTr("Move"))
        for (const id of selectedIds) {
            const r = canvas.elementRect(id)
            canvas.setElementRect(id, Qt.rect(r.x + dx, r.y + dy, r.width, r.height), "move:" + id)
        }
        canvas.endBatch()
    }
    // A drag: starts of every selected element, then absolute positions per step inside one batch.
    property var dragStarts: ({})
    function beginSelectionDrag() {
        dragStarts = {}
        for (const id of selectedIds) dragStarts[id] = canvas.elementRect(id)
        canvas.beginBatch(qsTr("Move"))
    }
    function dragSelectionTo(dx, dy) {
        for (const id in dragStarts) { const r = dragStarts[id]; canvas.setElementRect(id, Qt.rect(r.x + dx, r.y + dy, r.width, r.height), "move:" + id) }
    }
    function endSelectionDrag() { canvas.endBatch(); dragStarts = {} }
    function resizeSelected(rect) { if (selectedId !== "") canvas.setElementRect(selectedId, rect, "resize:" + selectedId) }
    function edgeResize(start, dx, dy, ddx, ddy, lockAspect) {   // a handle's drag → the new world rect (min 8 px)
        let x = start.x, y = start.y, w = start.width, h = start.height
        if (dx < 0) { x = Math.min(start.x + ddx, start.x + start.width - 8); w = start.x + start.width - x }
        else if (dx > 0) w = Math.max(8, start.width + ddx)
        if (dy < 0) { y = Math.min(start.y + ddy, start.y + start.height - 8); h = start.y + start.height - y }
        else if (dy > 0) h = Math.max(8, start.height + ddy)
        if (lockAspect && start.width > 0 && start.height > 0) {   // a video element keeps its picture's aspect (walk 7c)
            const ratio = start.width / start.height
            if (dx !== 0 && (dy === 0 || Math.abs(ddx) >= Math.abs(ddy) * ratio)) h = Math.max(8, w / ratio)
            else w = Math.max(8, h * ratio)
            if (dx < 0) x = start.x + start.width - w     // the moved edges stay under the pointer's side
            if (dy < 0) y = start.y + start.height - h
        }
        return Qt.rect(x, y, w, h)
    }
    function deleteSelected() {
        if (!selectedIds.length) return
        canvas.beginBatch(qsTr("Delete"))
        for (const id of selectedIds.slice()) canvas.removeElement(id)
        canvas.endBatch()
        selectedIds = selectedIds.filter(id => canvas.elementKind(id) !== "")   // (a last page refuses)
    }
    function drillSelected() { if (selectedId !== "" && canvas.elementKind(selectedId) === "page") canvas.focusPage = selectedId }
    // OPEN (2.0 phase A4) — one action behind the open glyph, the context menu's row and Shift-double-click: a
    // page pops out to the page view, a video / PDF to its full-frame tab, a text chip to its editor, a bookmark
    // to the browser, any other file to the platform's default app. (Double-click and Return = goLive: in place.)
    function canOpen(id) { const k = canvas.elementKind(id); return k === "page" || k === "shape" || k === "media" }
    function openLabel(id) {
        const k = canvas.elementKind(id)
        if (k === "page") return qsTr("Open page")
        if (k === "shape") return qsTr("Edit text")
        const mk = canvas.mediaElementKind(id)
        return mk === "video" ? qsTr("Open in studio") : mk === "pdf" ? qsTr("Open PDF") : mk === "bookmark" ? qsTr("Open link") : mk === "folder" ? qsTr("Open folder") : mk === "audio" ? qsTr("Play") : qsTr("Open")
    }
    // A selected PDF element pages (phase A5): the page lives in the editor's path-keyed map, so the
    // same PDF inline in a page and as an element show the same page.
    function mediaOf(id) { try { return JSON.parse(canvas.elementJson(id)).media || {} } catch (e) { return {} } }
    function pdfKeyOf(id) { return canvas.resolveMediaPath(mediaOf(id).src) }
    function pdfPagesOf(id) { return mediaOf(id).pages || 0 }
    readonly property bool selectedIsPdf: selectedId !== "" && canvas.mediaElementKind(selectedId) === "pdf"
    readonly property int pdfPageOfSelected: (editor.pdfPageRev, selectedIsPdf ? editor.pdfPageForKey(pdfKeyOf(selectedId)) : 0)
    function pdfStepSelected(d) { if (selectedIsPdf) editor.pdfStepKey(pdfKeyOf(selectedId), d, pdfPagesOf(selectedId)) }
    // Dropped / pasted TEXT (phase A7): one http(s) line → a remote image (image extension) or a bookmark; anything
    // else → a text chip sized to its text (body 14). Screen coordinates in, the element id out.
    function dropUrl(u, wx, wy) {
        return /\.(png|jpe?g|gif|webp|bmp|svg|avif)(\?.*)?$/i.test(u) ? canvas.addRemoteImageElement(u, wx, wy) : canvas.addBookmarkElement(u, wx, wy)
    }
    function dropText(text, sx, sy) {
        const t = ("" + text).trim()
        if (t === "") return ""
        const w = cam.screenToWorld(sx, sy)
        if (/^https?:\/\/\S+$/i.test(t)) return dropUrl(t, w.x, w.y)
        const size = 14, pad = Math.round(size * 0.3)
        measureText.text = t
        const tw = Math.min(480, Math.ceil(measureText.contentWidth) + 2 * pad + 4), th = Math.ceil(measureText.contentHeight) + 2 * pad
        return canvas.addShapeElement(JSON.stringify({ shape: "text", text: t, fontSize: size, fill: "" + Theme.colors.text }), Qt.rect(w.x, w.y, Math.max(60, tw), Math.max(size * 1.4, th)))
    }
    Text { id: measureText; visible: false; width: 480 - 12; wrapMode: Text.Wrap; font.family: Theme.font.body; font.pixelSize: 14 }
    // ⌘V on the canvas (phase A7): files → elements (a grid), an image → an image element, a bare remote image →
    // a remote image, text → dropText. The viewport's centre is the drop point.
    function pasteAt(sx, sy) {
        if (!canvas.documentOpen) return
        const w = cam.screenToWorld(sx, sy)
        const urls = clipboard.readUrls()
        let ids = []
        if (urls.length) {
            const local = urls.filter(u => !/^https?:/i.test(u)), remote = urls.filter(u => /^https?:/i.test(u))
            ids = local.length ? canvas.addElementsFromUrls(local, w.x, w.y) : []
            for (const u of remote) { const id = dropUrl(u, w.x, w.y); if (id !== "") ids.push(id) }
        } else if (clipboard.hasImage()) {
            const id = canvas.addClipboardImageElement(w.x, w.y); if (id !== "") ids = [id]
        } else if (clipboard.hasHtml() && canvas.bareRemoteImageSrc(clipboard.readHtml()) !== "") {
            const id = canvas.addRemoteImageElement(canvas.bareRemoteImageSrc(clipboard.readHtml()), w.x, w.y); if (id !== "") ids = [id]
        } else {
            const id = dropText(clipboard.readText(), sx, sy); if (id !== "") ids = [id]
        }
        if (ids.length) selectMany(ids)
    }
    readonly property bool selectedIsFolder: selectedId !== "" && canvas.mediaElementKind(selectedId) === "folder"
    function spreadSelected() { if (selectedIsFolder) { const ids = canvas.spreadFolder(selectedId); if (ids.length) selectMany(ids) } }
    property int opens: 0                // probe diagnostics
    function openSelected() {
        const id = selectedId
        ++opens
        if (id === "") return
        const k = canvas.elementKind(id)
        if (k === "page") { canvas.focusPage = id; return }
        if (k === "shape") { editText(id); return }
        if (k !== "media") return
        const mk = canvas.mediaElementKind(id)
        if (mk === "video" || mk === "pdf") { editor.setActiveTab(id); return }
        if (mk === "audio") { editor.playAudioElement(id); return }
        if (mk === "bookmark") { try { Qt.openUrlExternally(JSON.parse(canvas.elementJson(id)).media.src) } catch (e) {} return }
        canvas.openElement(id)
    }
    function duplicateSelected() {
        const made = []
        canvas.beginBatch(qsTr("Duplicate"))
        for (const id of selectedIds) { const n = canvas.duplicateElement(id); if (n !== "") made.push(n) }
        canvas.endBatch()
        if (made.length) selectedIds = made
    }
    function frontSelected() { canvas.beginBatch(qsTr("Bring to front")); for (const id of selectedIds) canvas.bringToFront(id); canvas.endBatch() }
    function backSelected() { canvas.beginBatch(qsTr("Send to back")); for (const id of selectedIds.slice().reverse()) canvas.sendToBack(id); canvas.endBatch() }
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
    onVisibleChanged: { if (!visible) { endLive(false); clearSelection() } else if (!cam.userCam) fit() }

    // --- LIVE CARD (2.0 SPIKE S4): a page edited IN PLACE. canvas.livePage makes the page the active model
    // (the manager re-points blockModel); the ONE Editor is reparented from its home slot into the card's
    // liveSlot and back. Keys and wheel go to it; the middle button and ⌥-drag pan the canvas; Escape ends it. ---
    // ONE state machine for "live" (phase C3): a page (canvas.livePage, the Editor in its card) or a text chip
    // (textEditor.editingId). `liveId` names whichever is live; `liveKind` which kind.
    readonly property string liveId: canvas.livePage !== "" ? canvas.livePage : textEditor.editingId
    readonly property string liveKind: canvas.livePage !== "" ? "page" : textEditor.editingId !== "" ? "shape" : ""
    // The camera animates to a readable zoom and brings the element into view before a page goes live (phase C2):
    // 180 ms, OutCubic, then `camThen` runs. Skipped when already ≥ 100 % and in view.
    property real camT: 0
    property var camFrom: null
    property var camTo: null
    property var camThen: null
    readonly property bool camAnimating: camAnim.running
    NumberAnimation { id: camAnim; target: cv; property: "camT"; from: 0; to: 1; duration: 180; easing.type: Easing.OutCubic
                      onFinished: { const f = cv.camThen; cv.camThen = null; cv.camFrom = cv.camTo = null; if (f) f() } }
    onCamTChanged: if (camFrom && camTo) cam.setCamera(camFrom.x + (camTo.x - camFrom.x) * camT, camFrom.y + (camTo.y - camFrom.y) * camT, camFrom.z + (camTo.z - camFrom.z) * camT, true)
    function animateCameraTo(panX, panY, zoom, then) {
        camAnim.stop()
        camFrom = { x: cam.panX, y: cam.panY, z: cam.zoom }; camTo = { x: panX, y: panY, z: zoom }; camThen = then
        camT = 0; camAnim.restart()
    }
    // The camera that shows `r` at zoom ≥ 1 with the least movement: zoom about the rect's centre, then shift
    // just enough to bring it inside the viewport (a rect wider / taller than the view aligns top-left).
    function readableCameraFor(r) {
        const z = Math.max(1, cam.zoom), pad = 24
        const c = cam.worldToScreen(r.x + r.width / 2, r.y + r.height / 2)            // the centre stays put through the zoom
        let px = c.x - (r.x + r.width / 2) * z, py = c.y - (r.y + r.height / 2) * z
        // pan ≥ lo keeps the rect's leading edge inside the padding; pan ≤ hi its trailing edge. Infeasible (the
        // rect is bigger than the view) → align the leading edge.
        const loX = pad - r.x * z, hiX = width - pad - (r.x + r.width) * z
        const loY = pad - r.y * z, hiY = height - pad - (r.y + r.height) * z
        px = loX <= hiX ? Math.max(loX, Math.min(hiX, px)) : loX
        py = loY <= hiY ? Math.max(loY, Math.min(hiY, py)) : loY
        return { x: px, y: py, z: z, moves: Math.abs(px - cam.panX) > 0.5 || Math.abs(py - cam.panY) > 0.5 || Math.abs(z - cam.zoom) > 0.001 }
    }
    // Liveness can end from ANY path — a drill-in, a page removal, a close, a tab switch — not only Escape
    // here: whenever it does, the Editor goes home (a stranded Editor inside a card swallows every gesture).
    onLiveIdChanged: if (liveId === "" && editor && editor.homeSlot && editor.parent !== editor.homeSlot) {
        editor.parent = editor.homeSlot
        editor.commitPageWidth(0)
        editor.forceActiveFocus()
    }
    function pageItemFor(id) {
        for (let i = 0; i < elementRepeater.count; ++i) { const it = elementRepeater.itemAt(i); if (it && it.elementId === id) return it }
        return null
    }
    property int lives: 0   // probe diagnostics
    property int mediaFocuses: 0   // probe
    function goLive(id) {
        const kind = canvas.elementKind(id)
        if (kind === "shape") { if (liveId !== "" && liveId !== id) endLive(false); editText(id); return }   // a text chip's live form is its editor
        if (kind === "media") {   // FOCUS (walk 7c, the ladder's rung for media): the camera to readable, selected; a video plays with its bar
            if (liveId !== "" && liveId !== id) endLive(false)
            select(id); ++mediaFocuses
            const target = readableCameraFor(canvas.elementRect(id))
            const then = function() {
                if (selectedId !== id || !editor) return
                const h = editor.canvasVideoHost
                if (canvas.mediaElementKind(id) === "video" && h && h.valid && !editor.canvasHostPlaying) editor.playFromHost(h)
            }
            if (target.moves) animateCameraTo(target.x, target.y, target.z, then); else then()
            if (editor) editor.forceActiveFocus()
            return
        }
        if (kind !== "page" || !editor || !editor.homeSlot) return
        if (liveId !== "" && liveId !== id) endLive(false)
        select(id)
        const target = readableCameraFor(canvas.elementRect(id))
        const mount = function() {
            if (selectedId !== id || canvas.livePage !== "") return   // the user moved on during the animation
            canvas.livePage = id          // → blockModel is this page; the card's preview unloads
            ++lives
            Qt.callLater(function() {     // mount after the press that asked for it has fully released
                const p = pageItemFor(id)
                if (!p || !p.page || canvas.livePage !== id) { canvas.livePage = ""; return }
                editor.parent = p.page.liveSlot
                editor.commitPageWidth(0)   // the card's measure (== what the preview told the model)
                editor.restoreScrollY(canvas.pageScrollY(id))
                editor.forceActiveFocus()
            })
        }
        if (target.moves) animateCameraTo(target.x, target.y, target.z, mount); else mount()
    }
    function endLive(selectIt) {
        if (camAnim.running) { camAnim.stop(); camThen = null }
        if (textEditor.editingId !== "") textEditor.commit()
        const id = canvas.livePage
        if (id === "") { if (selectIt && selectedId === "") {} return }
        editor.flushScrollSave()
        if (editor.homeSlot) editor.parent = editor.homeSlot
        canvas.livePage = ""
        editor.commitPageWidth(0)
        if (selectIt) select(id)
        editor.forceActiveFocus()
    }
    function liveSelected() { if (selectedId !== "") goLive(selectedId) }
    // Below a readable zoom a live card is unusable: the camera dropping under 75 % ends liveness (phase C8).
    Connections { target: cam; function onCameraChanged() { if (canvas.livePage !== "" && !camAnim.running && cam.zoom < 0.75) cv.endLive(true) } }
    Connections { target: cv.editor; function onLiveExitRequested() { cv.endLive(true) } }
    Connections {   // elements come, go or grow under a camera the user hasn't taken yet
        target: canvas
        function onGeometryChanged() { if (cv.visible && !cam.userCam) cv.fit() }
        function onElementsChanged() { if (cv.visible && !cam.userCam) cv.fit() }
    }
    // The user's camera is the file's: remembered on save (canvas_meta), restored on open.
    Connections { target: cam; function onCameraChanged() { if (cam.userCam && cv.visible) camSave.restart() } }
    Timer { id: camSave; interval: 300; onTriggered: if (canvas.documentOpen && cam.userCam) canvas.setCamera(cam.panX, cam.panY, cam.zoom) }

    // Which handler a press belongs to is decided AT THE PRESS (a passive point handler sees it
    // before any drag can activate): over an element or the selection frame the element's / the
    // handles' drag handlers own it, over empty canvas the view's pan does — two drag handlers
    // racing for the same press let the pan win and the element stayed put (gesture probe, 2026-10-17).
    property bool pressOnElement: false
    property int taps: 0                 // probe diagnostics
    property int doubleTaps: 0
    function frameHit(p) { const f = selFrame; return f.shown && ((p.x >= f.x - 8 && p.x <= f.x + f.width + 8 && p.y >= f.y - 8 && p.y <= f.y + f.height + 8) || f.glyphHit(p)) }
    PointHandler {
        acceptedButtons: Qt.LeftButton
        onActiveChanged: cv.pressOnElement = active ? (!cv.emptyAt(point.position.x, point.position.y) || cv.frameHit(point.position)) : false
    }

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
        keys: ["text/uri-list", "text/plain", "text/html"]
        onDropped: (drop) => {
            if (!canvas.documentOpen) return
            const w = cam.screenToWorld(drop.x, drop.y)
            if (drop.hasUrls && drop.urls.length) {
                const urls = []
                for (let i = 0; i < drop.urls.length; ++i) urls.push(drop.urls[i].toString())
                const local = urls.filter(u => !/^https?:/i.test(u)), remote = urls.filter(u => /^https?:/i.test(u))
                const ids = local.length ? canvas.addElementsFromUrls(local, w.x, w.y) : []   // one → in place; many → a grid, ONE history row (phase A6)
                for (const u of remote) { const id = cv.dropUrl(u, w.x, w.y); if (id !== "") ids.push(id) }
                if (ids.length) cv.selectMany(ids)
            } else if (drop.hasHtml && canvas.bareRemoteImageSrc(drop.html) !== "") {
                const id = canvas.addRemoteImageElement(canvas.bareRemoteImageSrc(drop.html), w.x, w.y)
                if (id !== "") cv.select(id)
            } else if (drop.hasText) {
                const id = cv.dropText(drop.text, drop.x, drop.y)
                if (id !== "") cv.select(id)
            }
            drop.accept()
            cv.editor.forceActiveFocus()
        }
    }
    // Empty-canvas gestures: tap clears / makes a text chip (text tool) / places a page (page tool);
    // double-tap with the pointer makes a page too (the hidden shortcut; the page tool is the shown one).
    TapHandler {
        enabled: !cv.drawing && !cv.handTool
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.DragThreshold
        onSingleTapped: (ev) => {
            ++cv.taps
            if (cv.frameHit(ev.position)) return   // the selection frame's chrome (the transport, the pager, the glyph) sits OUTSIDE the element's rect: never "empty canvas"
            if (!cv.emptyAt(ev.position.x, ev.position.y)) return
            cv.endLive(false)   // a click elsewhere ends a live card (and commits a chip's edit)
            if (cv.textTool) cv.newTextAt(ev.position.x, ev.position.y)
            else if (cv.pageTool) { const w = cam.screenToWorld(ev.position.x, ev.position.y); const id = canvas.addPageAt(w.x, w.y); if (id !== "") cv.select(id) }
            else { cv.clearSelection(); cv.editor.forceActiveFocus() }
        }
        onDoubleTapped: (ev) => { ++cv.doubleTaps; if (cv.pointerTool && !cv.frameHit(ev.position) && cv.emptyAt(ev.position.x, ev.position.y)) cv.newPageAt(ev.position.x, ev.position.y) }
    }
    // Right-click: the context menu for the element under the pointer (joins the selection) or for empty canvas.
    property int rightClicks: 0   // probe diagnostics
    TapHandler {
        acceptedButtons: Qt.RightButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: (ev) => {
            ++cv.rightClicks
            const w = cam.screenToWorld(ev.position.x, ev.position.y)
            const ids = canvas.visibleElements(w.x, w.y, w.x + 0.01, w.y + 0.01)
            const hit = ids.length ? ids[ids.length - 1] : ""
            if (hit !== "" && !cv.isSelected(hit)) cv.select(hit)
            else if (hit === "") cv.clearSelection()
            ctxMenu.worldX = w.x; ctxMenu.worldY = w.y
            ctxMenu.popupAt(ev.position.x, ev.position.y)
        }
    }
    CanvasContextMenu { id: ctxMenu; view: cv }
    readonly property var contextMenu: ctxMenu   // probe
    // The marquee (pointer tool, empty canvas): a screen-space box; the elements it crosses get selected.
    property rect marqueeRect: Qt.rect(0, 0, 0, 0)
    property bool marqueeActive: false
    DragHandler {
        id: marquee
        target: null
        acceptedButtons: Qt.LeftButton
        enabled: cv.pointerTool && !cv.pressOnElement
        // (It must keep the default CanTakeOverFromItems: on empty canvas the press falls through to the Editor's
        // background MouseArea underneath, so the marquee always starts by taking over from an item. The frame's
        // chrome protects itself instead — the compact transport's scrub area is preventStealing.)
        onActiveChanged: {
            if (active) { cv.marqueeActive = true; cv.marqueeRect = Qt.rect(centroid.pressPosition.x, centroid.pressPosition.y, 0, 0) }
            else if (cv.marqueeActive) {
                cv.marqueeActive = false
                const r = cv.marqueeRect
                if (r.width > 2 || r.height > 2) {
                    const a = cam.screenToWorld(r.x, r.y), b = cam.screenToWorld(r.x + r.width, r.y + r.height)
                    cv.selectMany(canvas.visibleElements(a.x, a.y, b.x, b.y))
                }
                cv.editor.forceActiveFocus()   // every selecting gesture hands the keys to the editor
            }
        }
        onCentroidChanged: {
            if (!active) return
            const p0 = centroid.pressPosition, p1 = centroid.position
            cv.marqueeRect = Qt.rect(Math.min(p0.x, p1.x), Math.min(p0.y, p1.y), Math.abs(p1.x - p0.x), Math.abs(p1.y - p0.y))
        }
    }
    Rectangle {
        visible: cv.marqueeActive
        x: cv.marqueeRect.x; y: cv.marqueeRect.y; width: cv.marqueeRect.width; height: cv.marqueeRect.height
        z: 9
        color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.08)
        border.width: 1; border.color: Theme.colors.accent
    }
    // The page tool's drag: a box whose width becomes the new page's measure.
    property rect pageDragRect: Qt.rect(0, 0, 0, 0)
    property bool pageDragActive: false
    DragHandler {
        target: null
        acceptedButtons: Qt.LeftButton
        enabled: cv.pageTool
        onActiveChanged: {
            if (active) { cv.pageDragActive = true; cv.pageDragRect = Qt.rect(centroid.pressPosition.x, centroid.pressPosition.y, 0, 0) }
            else if (cv.pageDragActive) {
                cv.pageDragActive = false
                const r = cv.pageDragRect
                if (r.width > 8) {
                    const w = cam.screenToWorld(r.x, r.y)
                    const id = canvas.addPageAt(w.x, w.y, r.width / cam.zoom - 2 * 24)
                    if (id !== "") cv.select(id)
                }
            }
        }
        onCentroidChanged: {
            if (!active) return
            const p0 = centroid.pressPosition, p1 = centroid.position
            cv.pageDragRect = Qt.rect(Math.min(p0.x, p1.x), Math.min(p0.y, p1.y), Math.abs(p1.x - p0.x), Math.abs(p1.y - p0.y))
        }
    }
    Rectangle {
        visible: cv.pageDragActive
        x: cv.pageDragRect.x; y: cv.pageDragRect.y; width: cv.pageDragRect.width; height: Math.max(cv.pageDragRect.height, 400 * cam.zoom)
        z: 9
        color: Qt.rgba(Theme.colors.surface.r, Theme.colors.surface.g, Theme.colors.surface.b, 0.5)
        border.width: 1; border.color: Theme.colors.accent
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
        onEraseAt: (wx, wy) => { const id = cv.inkAt(wx, wy); if (id !== "") { if (cv.isSelected(id)) cv.toggleSelect(id); canvas.removeElement(id) } }
        InkNodeItem {   // the in-progress stroke, at this zoom
            anchors.fill: parent
            envelope: capture.liveEnvelope
            frameW: width; frameH: height; sourceW: width; sourceH: height
            pxScale: 1
        }
    }
    readonly property var inkCapture: capture   // probe
    readonly property var selectionFrame: selFrame

    DragHandler {   // middle-drag ALWAYS pans — selection, tool or element under the pointer notwithstanding
        target: null
        acceptedButtons: Qt.MiddleButton
        property real sx: 0
        property real sy: 0
        onActiveChanged: if (active) { sx = cam.panX; sy = cam.panY; ++cv.pans }
        onActiveTranslationChanged: if (active) cam.setPan(sx + activeTranslation.x, sy + activeTranslation.y)
    }
    DragHandler {   // ⌥-drag pans while a page is LIVE (ruling 2026-10-18: Space types there) — takes over from the card's editor past the threshold
        target: null
        enabled: cv.liveId !== ""
        acceptedButtons: Qt.LeftButton
        acceptedModifiers: Qt.AltModifier
        property real sx: 0
        property real sy: 0
        onActiveChanged: if (active) { sx = cam.panX; sy = cam.panY; ++cv.pans }
        onActiveTranslationChanged: if (active) cam.setPan(sx + activeTranslation.x, sy + activeTranslation.y)
    }
    DragHandler {   // left-drag pans with the hand tool or Space held
        target: null
        enabled: cv.handTool
        acceptedButtons: Qt.LeftButton
        property real sx: 0
        property real sy: 0
        onActiveChanged: if (active) { sx = cam.panX; sy = cam.panY; ++cv.pans }
        onActiveTranslationChanged: if (active) cam.setPan(sx + activeTranslation.x, sy + activeTranslation.y)
    }
}
