import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Pdf
import QtCore

// Arm C — the "better surface" prototype. Blocks are PASSIVE (read-only) text;
// they never take focus and never own a cursor or selection. A single central
// key handler (this FocusScope) holds focus for the whole document. The model's
// logical cursor (anchor/focus as row,col) is the ONE source of truth; the caret
// and selection are drawn as overlays computed from it. This removes the
// focus-vs-logical desync that the per-TextEdit arm (B) fought.
//
// Read-only TextEdits are used only as a text-LAYOUT engine (positionToRectangle
// / positionAt) — not as editors.
FocusScope {
    id: root
    focus: true
    Component.onCompleted: { forceActiveFocus(); cursor.setCaret(0, 0); _recomputeVideoRows(); _recomputePdfRows(); blockModel.setContentWidth(pageWidth); sizePool() }
    // Closing the LAST tab unloads the whole Editor (the docContent Loader goes
    // inactive) — stop the shared decoder/audio deliberately rather than letting
    // child destruction race the decode/audio threads mid-playback.
    Component.onDestruction: stopVideo()
    // Single 760 reading measure shared by ALL blocks — ALWAYS the full
    // measure (user ruling 2026-07-12): a narrow window scrolls the page
    // horizontally instead of squeezing the column. That keeps the frame
    // margin ink was drawn against stable in EVERY mode, so annotations
    // never need to hide on shrink (the old inkSqueezed machinery is gone).
    // The page is LEFT-ANCHORED at a fixed margin, matching the HTML
    // export's layout language; the space to the right is where wide
    // tables grow.
    // PER-DOCUMENT width (v3, PLAN-page-width): the model owns the measure —
    // 760 for pre-v3 docs, detents up to 1600 for image-board documents.
    // Guarded so a transient 0 during document switches never reaches layout.
    // previewWidth (the PageRuler drives it mid-drag) overrides WITHOUT
    // committing; release commits via blockModel.setPageWidth then clears it.
    property real previewWidth: 0
    property bool widthDragging: false   // the ruler's drag → gutter tints below
    property real pageWidth: previewWidth > 0 ? previewWidth
                           : (blockModel.pageWidth > 0 ? blockModel.pageWidth
                                                       : Theme.dim.columnWidth)
    readonly property real viewContentX: flick.contentX   // the ruler rides the pan
    // Media is known-geometry: tell the model the width it derives media heights
    // from, and re-tell it on resize so reserved height stays exact (no jump).
    onPageWidthChanged: blockModel.setContentWidth(pageWidth)
    // (The resize-suspension machinery is GONE: it existed because the page
    // SQUEEZED on window resize, reflowing media live. The measure is a fixed
    // 760 now — a window resize changes only the viewport, media geometry
    // never moves, so there is nothing to suspend. 2026-07-12.)
    // Horizontal reach past the page for margin ink (each side, ink mode only).
    readonly property real inkGutter: 120
    // The document's CONTENT width: the viewport, grown to hold the sheet
    // (page or the widest measured block — wide tables — plus the trailing
    // gutter) so the PAGE scrolls horizontally when the window is narrower
    // than the sheet. In ink mode the same number: both gutters are part of
    // the sheet, so the Flickable pans natively (the kanban board's 2D-pan
    // pattern) exactly when they don't fit.
    readonly property real contentSpan: Math.max(flick.width, sheetRight)
    // The SHEET = page + one ink gutter each side. CENTRED in the viewport
    // when there is room (PLAN-centred-page, 2026-09-16 — overturns the
    // "fixed left margin" half of the 2026-08-18 ruling; the gutter itself
    // stays: margin annotations stay visible in writing mode and nothing
    // jumps when annotation mode toggles). Narrower windows hug the left
    // gutter as before and pan. Keyed off the COMMITTED page width, never
    // the ruler's live preview: a scrub grows the page rightward from its
    // current edge and the sheet recentres on release (no feedback loop
    // through the ruler's own origin). `/ zoom` is written in for a future
    // view zoom (1 today) so centring composes with it untouched.
    readonly property real zoom: 1
    readonly property real centredEdge: Math.max(inkGutter, Math.floor((flick.width / zoom
                                                 - (blockModel.pageWidth > 0 ? blockModel.pageWidth : Theme.dim.columnWidth)) / 2))
    // The gesture latch: the edge may only move BETWEEN gestures. Anything
    // that cached a content x at press (block-drag auto-scroll re-aim, the
    // pull arm point, image resize deltas, the 4 px click thresholds, an ink
    // stroke's press origin) would drift if a window resize or an Inspector
    // toggle moved the sheet mid-drag.
    readonly property bool gestureActive: mouse.pressed || widthDragging || blockDragging
                                          || dragging || pulling || dividerDragging || imageResizing
                                          || tableGripPressed || inkCanvas.drawing
    property real gestureEdge: -1
    onGestureActiveChanged: gestureEdge = gestureActive ? centredEdge : -1
    readonly property real leftEdge: gestureEdge >= 0 ? gestureEdge : centredEdge
    // The sheet's extent in content x — never narrower than the widest
    // content plus the trailing gutter (user ruling 2026-08-20: an uncapped
    // wide table extends the sheet rather than being cropped by the desk
    // tone at the page boundary). A wide table grows RIGHTWARD from the
    // page's left edge (ruling 2026-09-16: sheets left-align; centring on
    // the widest content would feed measured widths back into layout).
    // A table WIDER than the page centres under it (ruling 2026-09-16): half its overhang
    // shifts left, but never past the gutter — so in a narrow window it slides back to the
    // page's left edge and overflows right as before. Continuous, prose never moves. View
    // geometry only: the model's table x stays 0-based; `tableX(head)` is the content x of
    // a table's left edge and every table site adds THAT, not leftEdge.
    function tableShiftFor(tw) {
        return -Math.min(Math.round(Math.max(0, (tw - pageWidth) / 2)), Math.max(0, leftEdge - inkGutter))
    }
    function tableShift(head) { return head < 0 ? 0 : tableShiftFor((blockModel.layoutRevision, blockModel.tableWidth(head))) }
    function tableX(head) { return leftEdge + tableShift(head) }
    // Content x → PAGE x (0 = the page's left edge) for the block under (cx, cy): a table row's
    // x is read against the table's shifted left edge, so the model's 0-based columns resolve.
    function pageXAt(cx, cy) {
        const rec = blockModel.rowForY(Math.max(0, cy))
        const head = rec >= 0 ? blockModel.tableHeadOf(rec) : -1
        return cx - leftEdge - (head >= 0 ? tableShift(head) : 0)
    }
    readonly property real sheetLeft: leftEdge + tableShiftFor(blockModel.maxTableWidth) - inkGutter
    readonly property real sheetRight:
        leftEdge + Math.max(pageWidth, blockModel.maxBlockWidth,
                            blockModel.maxTableWidth + tableShiftFor(blockModel.maxTableWidth)) + inkGutter
    function measureForType(t) { return pageWidth }
    function measureForRow(row) { return laneOf(row).w }   // a lane block measures its lane
    // An image's "fit" width: its lane, minus a table cell's two 8 px insets (BlockView's colLeft).
    function fitWidthForRow(row) { return Math.round(measureForRow(row) - (blockModel.tableColumnOf(row) >= 0 ? 16 : 0)) }
    // Lane `lane` of the split row whose record is `record`, page-relative (SR-3 D3):
    // its share of the page less the gaps between lanes. Computed from pageWidth and
    // the row's ratios (not read from the model) so a page-width change can't race
    // setContentWidth; the model's own geometry uses the same formula. Reads
    // contentRevision so every binding calling it follows structure changes.
    function laneSpan(record, lane) {
        const rev = blockModel.contentRevision
        const head = blockModel.tableHeadOf(record)
        if (head >= 0) {       // SR-4: table columns are px from the head's spec (C++), past the page too
            const lrev = blockModel.layoutRevision
            return { x: tableShift(head) + blockModel.tableColumnLeft(head, lane), w: blockModel.tableColumnWidth(head, lane) }
        }
        const ratios = blockModel.splitRatios(record)
        if (lane < 0 || lane >= ratios.length) return { x: 0, w: pageWidth }
        const gap = blockModel.laneGap
        const avail = Math.max(0, pageWidth - gap * (ratios.length - 1))
        let x = 0
        for (let k = 0; k < lane; ++k) x += avail * ratios[k] + gap
        return { x: x, w: avail * ratios[lane] }
    }
    // A block's lane: the page for a top-level block.
    function laneOf(row) {
        const rev = blockModel.contentRevision
        const lane = blockModel.laneForRow(row)
        return lane < 0 ? { x: 0, w: pageWidth } : laneSpan(blockModel.splitRowOf(row), lane)
    }
    // The block's column x in CONTENT coordinates (the page edge plus its lane).
    function columnX(row) { return leftEdge + laneOf(row).x }
    // Whether a block's box meets the viewport (± a margin). Lane-safe, unlike comparing
    // flat row numbers to the first/last visible top-level rows: a lane block's number
    // can sit far past the next top-level row's.
    function rowInView(row) {
        const y = (blockModel.layoutRevision, blockModel.yForRow(row))
        return y + blockModel.heightForRow(row) >= flick.contentY - 100
            && y <= flick.contentY + flick.height + 100
    }

    // The right Inspector panel (set from Main.qml) — the studio's drawing
    // tool/color/width live there (its Draw target).
    property var inspector: null

    // --- Annotation (ink) mode: draw block-pinned margin ink over the whole
    // document. DERIVED, never stored (2026-08-19 redesign): the armed tool IS
    // the mode. Any non-Type tool in the Document view engages the ink surface;
    // Type — the resting tool — is regular editing. Page locks to 760 +
    // horizontal pan (see pageWidth/contentSpan above); the Inspector FLOATS
    // instead of pushing the column (Main.qml); the ink canvas takes the mouse.
    // Full-frame tabs are their own worlds: sketch/video interpret the tools
    // locally, table/PDF disarm to Type (setActiveTab).
    readonly property bool inkMode: activeFrameId === "" && !!inspector
                                    && inspector.drawTool !== "type"
    // Show/hide the ink layer (data untouched — a reading-mode switch,
    // mirroring the video studio's annotations toggle). Persisted app-wide;
    // entering annotation mode forces it visible (drawing on a hidden layer
    // would be baffling).
    property alias inkLayerVisible: inkPrefs.layerVisible
    Settings { id: inkPrefs; category: "annotations"; property bool layerVisible: true }
    readonly property int inkStrokeCount: inkCanvas.strokeCount   // rail toggle enablement
    // Mode-edge side effects (the old setInkMode body, minus the tool writes —
    // the tool now drives, so writing it from here would loop).
    onInkModeChanged: {
        inkTextSession.commit()   // an open chip session never straddles the mode edge
        if (inkMode) {
            inkLayerVisible = true
        } else {
            inkCanvas.clearSelection()
            flick.contentX = 0    // the pan gutter is an ink-mode affordance
        }
    }

    // A derived table's tab (SR-4 S9): its head record's id. Until S9b lands the grid frame the
    // tab shows the board; a table with no grouping column stays in the document.
    property string activeGridId: ""
    readonly property int activeGridHead: (blockModel.layoutRevision, blockModel.contentRevision,
        activeGridId === "" ? -1 : blockModel.rowForId(activeGridId))
    onActiveGridHeadChanged: if (activeGridId !== "" && (activeGridHead < 0 || blockModel.headerCount(activeGridHead) <= 0)) activeGridId = ""
    // The grid frame (S9b): the document view restricted to the tab's table — blocks outside it hidden,
    // the scroll range clamped to the table's extent through the Flickable's margins (negative margins
    // move the min / max offsets inward), the pool and every gesture untouched.
    readonly property int frameLo: activeGridHead >= 0 && !boardMode ? activeGridHead : -1
    readonly property int frameHiRec: {
        if (frameLo < 0) return -1
        const dep = blockModel.contentRevision
        const recs = blockModel.tableRecords(frameLo)
        return recs.length ? recs[recs.length - 1] : frameLo
    }
    readonly property int frameHi: frameLo >= 0 ? (blockModel.contentRevision, blockModel.splitRowLast(frameHiRec)) : -1
    readonly property real frameTop: frameLo >= 0 ? (blockModel.layoutRevision, blockModel.yForRow(frameLo)) : 0
    readonly property real frameBottom: frameLo >= 0
        ? (blockModel.layoutRevision, blockModel.yForRow(frameHiRec) + blockModel.heightForRow(frameHiRec)) : 0
    onFrameLoChanged: if (frameLo >= 0) { flick.contentX = 0; flick.contentY = frameTop - Theme.dim.toolStripHeight }
    // T4 (S9c): the grid frame's view-only row filter. Lives with the frame: a tab switch clears it,
    // and it never touches the document (copy / export / undo see every row). Re-applied on edits
    // only through the fold set — a row you're typing in stays put even when it stops matching.
    property string tableFilter: ""
    readonly property int tableHiddenCount: activeGridHead >= 0 ? (blockModel.layoutRevision, blockModel.hiddenRecordCount(activeGridHead)) : 0
    function applyGridFilter(text) {
        tableFilter = text
        if (activeGridHead < 0) { blockModel.clearHiddenRecords(); return }
        blockModel.setHiddenRecords(blockModel.tableFilterRecords(activeGridHead, text))
        // The caret can't sit in a folded row: park it in the first row still showing.
        if (cursor.row >= 0 && blockModel.rowHidden(cursor.row)) {
            const recs = blockModel.tableRecords(activeGridHead)
            for (let i = blockModel.headerCount(activeGridHead); i < recs.length; ++i)
                if (!blockModel.rowHidden(recs[i])) { landInCell(activeGridHead, i, 0); return }
            landInCell(activeGridHead, 0, 0)
        }
    }
    onActiveGridIdChanged: if (tableFilter !== "") applyGridFilter("")   // by id: the head ROW shifts on edits above the table
    function firstGroupColOf(head) {
        for (var c = 0; c < blockModel.tableColumnCount(head); ++c) {
            var k = blockModel.tableColumnKind(head, c)
            if (k === 1 || k === 2) return c
        }
        return -1
    }
    // The board's grouping column kind / option moves / editor.
    function boardKind() {
        return boardCol >= 0 && activeGridHead >= 0 ? blockModel.tableColumnKind(activeGridHead, boardCol) : 0
    }
    function moveBoardOption(key, toIndex) {
        if (activeGridHead >= 0) blockModel.tableMoveOption(activeGridHead, boardCol, key, toIndex)
    }
    function openBoardOptions() {
        if (activeGridHead >= 0) choiceEditor.open2Grid(activeGridHead, boardCol)
    }
    // Active PDF tab (full-page scroll view); "" = not in a PDF tab. Tables and
    // PDFs are mutually exclusive full-frame modes — setActiveTab keeps one set.
    property string activePdfId: ""
    readonly property int activePdfRow: (blockModel.layoutRevision, blockModel.contentRevision,
        activePdfId === "" ? -1 : blockModel.rowForId(activePdfId))
    onActivePdfRowChanged: {
        if (activePdfId !== "" && activePdfRow < 0) activePdfId = ""
        if (activePdfRow < 0) pdfSpaceHeld = false   // never leave the hand stuck across a tab switch
    }
    // PDF tab zoom (2026-08-20): 1.0 = fit-width; the ListView grows a
    // horizontal content axis when zoomed past it (space-hand pans both).
    // Reset per tab — keyed on the ID, not the row (row shifts on inserts).
    property real pdfZoom: 1.0
    onActivePdfIdChanged: pdfZoom = 1.0
    property real pdfFitPageW: 0            // pushed by the list (viewport-derived)
    function pdfZoomTo(z) { pdfZoom = Math.max(0.25, Math.min(4, z)) }
    function pdfZoomStep(dir) { pdfZoomTo(pdfZoom * (dir > 0 ? Math.SQRT2 : 1 / Math.SQRT2)) }
    function pdfZoomFit() { pdfZoomTo(1.0) }
    function pdfZoom100() {   // page at its POINT size (1 unit per pt)
        if (pdfFitPageW > 0 && blockModel.mediaW(activePdfRow) > 0)
            pdfZoomTo(blockModel.mediaW(activePdfRow) / pdfFitPageW)
    }
    // Space-hand panning in the PDF tab (the sketch-tab convention): while
    // held, the page canvases refuse mouse so drags fall through to the
    // ListView and scroll it. With a tool armed, a plain drag DRAWS — the
    // canvas keeps the grab (SketchCanvas::mousePressEvent) so the list can
    // never steal a stroke; wheel scrolling works throughout.
    property bool pdfSpaceHeld: false
    // The PDF tab's page canvas that currently owns Esc/Delete (one SketchCanvas
    // per page delegate — the last one to draw or select wins; switching pages
    // clears the previous page's selection so two pages never both show one).
    property var pdfActiveInk: null
    function _setPdfActiveInk(c) {
        if (pdfActiveInk && pdfActiveInk !== c) pdfActiveInk.clearSelection()
        pdfActiveInk = c
    }
    // Active video tab (the studio: surface + transport + notes panel); "" = not
    // in a video tab. Opening it activates the video PAUSED at its remembered
    // playhead so the studio comes up showing a frame, not a void.
    property string activeVideoId: ""
    readonly property int activeVideoRow: (blockModel.layoutRevision, blockModel.contentRevision,
        activeVideoId === "" ? -1 : blockModel.rowForId(activeVideoId))
    onActiveVideoRowChanged: if (activeVideoId !== "" && activeVideoRow < 0) activeVideoId = ""
    onActiveVideoIdChanged: {
        forceActiveFocus()
        if (activeVideoId === "") {
            // Leaving the studio with the block off-viewport: videoVisible was
            // false and STAYS false, so its change handler never runs — tear
            // down here or the decoder (and audio) would run on invisibly.
            if (!videoVisible && videoPlayingRow >= 0) stopVideo()
            return
        }
        var r = blockModel.rowForId(activeVideoId)
        if (r >= 0) _activateVideo(r)
    }
    // Active sketch tab (full-frame canvas editing); "" = not in a sketch tab.
    property string activeSketchId: ""
    readonly property int activeSketchRow: (blockModel.layoutRevision, blockModel.contentRevision,
        activeSketchId === "" ? -1 : blockModel.rowForId(activeSketchId))
    onActiveSketchRowChanged: if (activeSketchId !== "" && activeSketchRow < 0) activeSketchId = ""
    onActiveSketchIdChanged: forceActiveFocus()
    // The block id shown full-frame ("" = Document view) — drives the tab strip's
    // active state across table, PDF, video and sketch tabs.
    readonly property string activeFrameId: activeGridId !== "" ? activeGridId
                                          : activePdfId !== "" ? activePdfId
                                          : activeVideoId !== "" ? activeVideoId : activeSketchId
    function setActiveTab(id) {
        boardMode = false; boardCol = -1
        // Tool rules on view changes (2026-08-19, tightened 2026-08-20):
        // landing on the Document view always lands TYPING (a leftover armed
        // tool would silently re-engage ink mode); table tabs have no ink
        // surface AND select there is the cell cursor's job, so EVERY
        // non-Type tool drops to Type; sketch/video/PDF tabs keep the tool
        // and interpret it locally (PDF pages take per-page ink AND text
        // chips since 2026-08-20). The text-chip tool alone drops to Select
        // in video tabs (text boxes are not part of the QCView flow).
        var t = inspector ? inspector.drawTool : "type"
        if (id === "") {
            if (inspector && t !== "type") inspector.drawTool = "type"
            activeGridId = ""; activePdfId = ""; activeVideoId = ""; activeSketchId = ""; return
        }
        var r = blockModel.rowForId(id)
        if (blockModel.headerCount(r) > 0 && blockModel.tableHeadOf(r) === r) {   // a table (S9)
            // Any non-Type tool drops here (2026-08-20): tables have no ink surface.
            if (t !== "type") inspector.drawTool = "type"
            activePdfId = ""; activeVideoId = ""; activeSketchId = ""; activeGridId = id
            var gc = boardPref(id)               // this table's remembered view: the board, else the grid frame
            if (gc >= 0 && blockModel.tableColumnKind(r, gc) !== 1 && blockModel.tableColumnKind(r, gc) !== 2) gc = -1
            if (gc >= 0) { boardCol = gc; boardMode = true }
        }
        else if (blockModel.mediaKind(r) === "video") {
            if (t === "text") inspector.drawTool = "select"
            activeGridId = ""; activePdfId = ""; activeSketchId = ""; activeVideoId = id
        }
        else if (blockModel.mediaKind(r) === "sketch") { activeGridId = ""; activePdfId = ""; activeVideoId = ""; activeSketchId = id }
        else { activeGridId = ""; activeVideoId = ""; activeSketchId = ""; activePdfId = id }
    }
    // Switching documents (new/open/save-as) resets the model; drop all per-doc
    // UI state so nothing points at the old doc's blocks (closes frame tabs /
    // studio / board, parks the caret at the top).
    Connections {
        target: blockModel
        function onDocumentChanged() {
            root.setActiveTab("")
            cursor.clearMarks()
            cursor.setCaret(0, 0)
            root.ensureVisible(0)
        }
    }

    // --- Per-tab view state (multi-document tabs). One Editor is shared across
    // all open documents; `blockModel` re-points to the active tab's model on
    // switch. These snapshot / restore the QML-side view state (scroll, caret,
    // active sub-tab + board) so each tab comes back exactly where you left it.
    // The blob is held by the DocumentManager, keyed per tab; Main.qml drives
    // capture-before / restore-after around docs.setActive.
    function captureViewState() {
        return {
            scrollY:       flick.contentY,
            focusRow:      cursor.focusRow,  focusCol:  cursor.focusCol,
            anchorRow:     cursor.anchorRow, anchorCol: cursor.anchorCol,
            activeFrameId: root.activeFrameId,
            boardMode:     root.boardMode,   boardCol:  root.boardCol
        }
    }
    function restoreViewState(m) {
        // No saved state (a freshly-opened tab) → the standard fresh-doc reset.
        if (!m || m.focusRow === undefined) {
            root.setActiveTab("")
            cursor.clearMarks(); cursor.setCaret(0, 0); root.ensureVisible(0)
            return
        }
        cursor.clearMarks()
        root.setActiveTab(m.activeFrameId)              // restores the frame tab (table/pdf/video/sketch)
        if (m.boardMode && m.boardCol >= 0) { root.boardMode = true; root.boardCol = m.boardCol }
        // Restore the caret (and any selection), then the scroll offset.
        cursor.anchorRow = m.anchorRow; cursor.anchorCol = m.anchorCol
        cursor.focusRow  = m.focusRow;  cursor.focusCol  = m.focusCol
        cursor.sync()
        flick.contentY = m.scrollY
    }

    // Inspector hook: after a model-side Fit-to-ink, frame the new canvas.
    function sketchRefitCamera() { if (activeSketchRow >= 0) sketchStage.fitCamera() }

    // --- Inspector Size-slider chip targeting: when a text chip is SELECTED
    // on either surface, the slider edits that chip (value in page/source px;
    // ink frame chips convert via the placement scale). -1 = no target, the
    // slider falls back to the new-chip default. ---
    readonly property real selChipSize: {
        if (activeSketchRow >= 0 && sketchEditCanvas.selectedTextIndex >= 0) {
            var t = (blockModel.contentRevision,
                     sketchEditCanvas.textElementAt(sketchEditCanvas.selectedTextIndex))
            return t.size !== undefined ? t.size : -1
        }
        if (inkMode && inkCanvas.selectedTextIndex >= 0) {
            var r = inkCanvas.selectedTextRow
            var u = (blockModel.inkRevision,
                     inkCanvas.inkTextAt(r, inkCanvas.selectedTextIndex))
            return u.size !== undefined ? u.size * inkCanvas.inkTextSizeScale(r) : -1
        }
        if (activePdfRow >= 0 && pdfActiveInk && pdfActiveInk.selectedTextIndex >= 0) {
            var p = (blockModel.contentRevision,
                     pdfActiveInk.textElementAt(pdfActiveInk.selectedTextIndex))
            return p.size !== undefined ? p.size : -1
        }
        return -1
    }
    function applyChipSize(v) {
        if (v <= 0) return
        if (activeSketchRow >= 0 && sketchEditCanvas.selectedTextIndex >= 0) {
            var i = sketchEditCanvas.selectedTextIndex
            var t = sketchEditCanvas.textElementAt(i)
            if (t.size !== undefined && Math.abs(t.size - v) > 0.01)
                blockModel.sketchSetTextBox(activeSketchRow, i, t.x, t.y, t.w, v)
        } else if (inkMode && inkCanvas.selectedTextIndex >= 0) {
            var r = inkCanvas.selectedTextRow, j = inkCanvas.selectedTextIndex
            var u = inkCanvas.inkTextAt(r, j)
            var sc = inkCanvas.inkTextSizeScale(r)
            var lv = sc > 0 ? v / sc : v
            if (u.size !== undefined && Math.abs(u.size - lv) > 0.01)
                inkCanvas.inkSetTextBox(r, j, u.x, u.y, u.w, lv)
        } else if (activePdfRow >= 0 && pdfActiveInk && pdfActiveInk.selectedTextIndex >= 0) {
            var pi = pdfActiveInk.selectedTextIndex
            var pt = pdfActiveInk.textElementAt(pi)
            if (pt.size !== undefined && Math.abs(pt.size - v) > 0.01)
                blockModel.pdfSetPageTextBox(activePdfRow, pdfActiveInk.pdfPage,
                                             pi, pt.x, pt.y, pt.w, v)
        }
    }

    function insertSketchAt(row) {
        blockModel.commitMarkdown(cursor.focusRow)   // leaving the edited block → consume its inline md
        var r = blockModel.insertSketch(row)
        // Open the new sketch's tab immediately — an empty inline canvas
        // invites nothing; the tab is where drawing lives.
        if (r >= 0) { cursor.setCaret(r, 0); setActiveTab(blockModel.idForRow(r)) }
    }
    function insertSketchAtCaret() { insertSketchAt(cursor.focusRow) }
    // Per-table board-view memory (app-level Settings, keyed by block id —
    // VIEW state, so it stays out of the document and out of undo).
    Settings {
        id: boardPrefs
        category: "boards"
        property string map: "{}"
    }
    function boardPref(id) {
        try { var m = JSON.parse(boardPrefs.map); return m[id] === undefined ? -1 : m[id] } catch (e) { return -1 }
    }
    function saveBoardPref(id, col) {
        if (id === "") return
        var m = {}
        try { m = JSON.parse(boardPrefs.map) } catch (e) {}
        if (col >= 0) m[id] = col; else delete m[id]
        boardPrefs.map = JSON.stringify(m)
    }
    // Explicitly leaving the board (toolbar / Esc / opening a card / the column
    // reverting) remembers "grid" for this table; plain tab switches don't.
    function showGridView() {
        boardMode = false
        saveBoardPref(activeGridId, -1)
    }
    // Leaving the board: the tab shows its grid (a derived table's grid frame, S9b).
    function leaveBoard() { showGridView() }
    // Kanban board: the active table tab rendered as a board grouped by a
    // choice/check column. View state only (not persisted, not undoable).
    property bool boardMode: false
    property int  boardCol: -1
    function openBoard(row, c) {
        var id = blockModel.idForRow(row)
        setActiveTab(id)
        boardCol = c
        boardMode = true
        saveBoardPref(id, c)
    }
    // The first choice/check column of the active table (−1 none) — the default
    // grouping for the grid view's "Board view" toggle.
    readonly property int firstGroupCol: {
        var rev = blockModel.contentRevision
        return activeGridHead >= 0 ? firstGroupColOf(activeGridHead) : -1
    }
    readonly property int overscan: 6
    property Item focusBlockItem: null    // the read-only TextEdit of the focus row
    property bool caretOn: true

    // Mouse drag-select state. dragX is content-x; dragViewY is viewport-y (so
    // edge auto-scroll keeps extending under the held cursor as content moves).
    property bool dragging: false
    property real dragX: 0
    property real dragViewY: 0

    // Block drag-reorder state. The persistent content-level `mouse` MouseArea
    // (not a recycled cell) owns the grab, so this survives scroll/recycle and
    // auto-scroll. The dragged block is tracked by index; a floating ghost +
    // a drop-indicator line follow the cursor; on release → blockModel.moveBlock.
    property bool blockDragging: false
    property int  blockDragRow: -1       // logical row being dragged
    property real blockDragViewY: 0       // viewport y of the cursor
    property int  dropGap: -1            // insertion gap 0..count (line at its top)
    property int  blockDragCount: 1      // run length: the selected range when its number was grabbed
    // Drops into lanes (SR-3 S7c): the lane a gap sits in (-1 = the top level), or a
    // side-edge target — the block the drop lands beside and on which side (0 right, 1 left).
    property int  dropLane: -1
    property int  dropBesideRow: -1
    property int  dropBesideSide: -1
    property real blockDragX: 0          // content x of the pointer (the auto-scroll ticker re-aims with it)
    property int  imageDropLane: -1
    property int  imageDropBesideRow: -1
    property int  imageDropBesideSide: -1
    property int  hoverRow: -1           // row whose grip is lit
    readonly property real gutterX: leftEdge   // drag-feedback overlays (drop line / ghost) align here

    // Block context-menu state: the right-clicked row and where the menu opened
    // (viewport coords; reused to anchor the language picker).
    property int  menuRow: -1
    property real menuX: 0
    property real menuY: 0
    property real choiceX: 0       // anchor for the choice-cell option picker
    property real choiceY: 0
    property string menuLinkUrl: ""  // link URL under the right-click (for "Open …")
    property var  menuIssue: null      // spell/grammar issue under the right-click ({s,e,kind,…}) or null
    // Link-hover tooltip: the URL under the pointer + where to anchor the pill.
    property string hoverLinkUrl: ""
    property real   hoverLinkX: 0
    property real   hoverLinkViewY: 0
    // Context-menu target highlight: the scope of the hovered menu item ("" none,
    // "block" whole block, "column"/"row" within a table) + danger (red) tint.
    property string menuHiScope: ""
    property bool   menuHiDanger: false
    // The derived-table cell under the block menu {head, r, c} (null elsewhere): what the table,
    // column, row and set scopes wash (SR-4 S7b).
    readonly property var menuGrid: {
        const dep = blockModel.contentRevision
        const h = menuRow >= 0 && menuRow < blockModel.count ? blockModel.tableHeadOf(menuRow) : -1
        return h < 0 ? null : { head: h, r: blockModel.tableRowOf(menuRow), c: blockModel.tableColumnOf(menuRow) }
    }

    // Lane gestures (SR-3 S7b). ALL state on root (delegates are pooled). Previews never
    // touch the model: release commits one undo step, Escape cancels.
    // Divider drag: hover a split row's lane gap, drag the divider (⌥ = this row only).
    property int  dividerHoverRecord: -1
    property int  dividerHoverIndex: -1
    property bool dividerDragging: false
    property int  dividerDragRecord: -1
    property int  dividerDragIndex: -1
    property bool dividerDragAlone: false
    property real dividerPreviewX: 0          // page-relative
    property var  dividerDragChain: []        // [record, divider, …] moving together
    // Pull from the boundary: the hot band just inside a block's column edge drags out a lane.
    property int  pullHoverRow: -1
    property int  pullHoverSide: -1           // 0 = the right edge (a new lane on the right), 1 = the left
    property bool pulling: false
    property int  pullRow: -1
    property int  pullSide: 0
    property int  pullLo: -1                  // the run being wrapped (one block unless a run is selected)
    property int  pullHi: -1
    property real pullPressX: 0               // page-relative
    property real pullPreviewX: 0             // page-relative
    readonly property real laneHotBand: 8
    // A press on the pull strip waits for its first move: sideways pulls a lane, anything else is a click
    // or a selection drag from the press point (Shift presses never arm).
    property bool pullArmed: false
    property int  pullArmRow: -1
    property int  pullArmSide: -1
    property int  pullArmMods: 0
    property real pullArmX: 0                 // content coords
    property real pullArmY: 0
    // Table grips (SR-4 S7b, SR-0 §4.12): hover bands outside the grid — beside each row in the left
    // margin, above the first row in its pocket. A click picks a row / column set (Shift spans, ⌘
    // toggles); past the 4 px threshold a row grip starts the rail's drag (a header row carries its
    // table) and a column grip reorders columns table-wide.
    property int    tableGripHead: -1          // hover
    property string tableGripKind: ""          // "" | "row" | "col"
    property int    tableGripIndex: -1
    property bool   tableGripPressed: false    // pressed, not yet a drag
    property int    tableGripPressHead: -1
    property string tableGripPressKind: ""
    property int    tableGripPressIndex: -1
    property int    tableGripPressMods: 0
    property real   tableGripPressX: 0
    property real   tableGripPressY: 0
    property bool   tableColDragging: false
    property int    tableColGap: -1            // 0..columns
    // A grip-picked set {head, kind "row" | "col", items (sorted), last, rev}; valid while the caret
    // stays put and the content revision is `rev` (tableSetLive).
    property var    tableSet: null

    // Image resize: the hovered image row shows corner affordances; dragging the
    // bottom-right handle previews a target size (a ghost frame — the document does
    // NOT reflow during the drag) and commits the new per-block width on release.
    property int  imgHandleRow: -1         // image row whose hover handles are shown
    property bool imageResizing: false
    property int  imageResizeRow: -1
    property real imageResizeW: 0          // live preview width (px)
    property real imageResizeAspect: 1     // h/w, captured on press (for the ghost height)
    property real _imgResizePressX: 0
    property real _imgResizeStartW: 0
    property int  _imgResizeSign: 1        // +1 right-corner drag, -1 left-corner drag (outward = bigger)
    function _isImageRow(r) {
        return r >= 0 && blockModel.typeForRow(r) === 3 && blockModel.mediaKind(r) === "image"
    }
    // Rows that take the inline resize affordance: images and sketches (both carry
    // a display-width override; sketch strokes are normalized so they scale crisp).
    function _isResizableMediaRow(r) {
        if (r < 0 || blockModel.typeForRow(r) !== 3) return false
        var k = blockModel.mediaKind(r)
        return k === "image" || k === "sketch"
    }
    // Insertion gap (0..count) for a content-y: before/after the row by its midpoint.
    function gapForY(cy) {
        var n = blockModel.count
        if (cy <= 0) return 0
        var row = blockModel.rowForY(cy)          // a top entry — a split row answers with its record
        var mid = blockModel.yForRow(row) + blockModel.heightForRow(row) / 2
        if (cy < mid) return row
        const last = blockModel.splitRowLast(row) // below a split row means after its last block, never inside it
        return (last >= 0 ? last : row) + 1
    }
    // Tab merge snaps below a run of adjacent split rows (R-I6 6c): a merge never lands
    // between two split rows or inside one.
    function mergeGapForY(cy) {
        return blockModel.mergeGapFor(gapForY(cy))
    }
    // Where a drag (of blocks or files) would land at content point (cx, cy), SR-3 S7c:
    // the side edge of a block (→ a new lane beside it), a gap inside the lane under the
    // pointer, or a top-level gap — always top-level when the pointer is off the page.
    // excludeLo/excludeCount = the dragged run, which can't be its own target.
    // A top-level gap kept out of a table's inside (SR-4 S7b, §4.12): a whole table — or anything
    // that isn't a split row — never lands between a table's rows (it snaps to the nearer edge); a
    // table row never lands among a table's header rows (it lands under them, in the body).
    function tableSafeGap(gap, cy, wholeTable) {
        if (gap <= 0 || gap >= blockModel.count) return gap
        const head = blockModel.tableHeadOf(gap)
        if (head < 0 || head === gap) return gap
        const recs = blockModel.tableRecords(head), lastRec = recs[recs.length - 1]
        const end = blockModel.splitRowLast(lastRec) + 1
        if (wholeTable) {
            const mid = (blockModel.yForRow(head) + blockModel.yForRow(lastRec) + blockModel.heightForRow(lastRec)) / 2
            return cy < mid ? head : end
        }
        const r = recs.indexOf(gap), hc = blockModel.headerCount(head)
        return r > 0 && r < hc ? (hc < recs.length ? recs[hc] : end) : gap
    }
    function laneDropAim(cx, cy, excludeLo, excludeCount) {
        const top = { gap: tableSafeGap(gapForY(cy), cy, true), lane: -1, besideRow: -1, besideSide: -1 }
        const pageX = pageXAt(cx, cy)
        if (pageX < 0) return top
        const hit = blockModel.blockAt(pageX, Math.max(0, cy))
        const head = hit >= 0 ? blockModel.tableHeadOf(hit) : -1
        if (pageX > pageWidth && head < 0) return top             // a table may run past the page
        if (hit < 0 || (hit >= excludeLo && hit < excludeLo + excludeCount)) return top
        const t = blockModel.typeForRow(hit)
        // A typed body cell holds one chip: nothing joins it.
        if (head >= 0 && t !== 10 && !blockModel.isHeaderRow(hit)
            && blockModel.tableColumnKind(head, blockModel.tableColumnOf(hit)) !== 0) return top
        if (t !== 10 && head < 0) {                               // table cells take no side drops
            const x0 = columnX(hit), w = laneOf(hit).w, band = Math.min(24, w / 6)
            if (cx < x0 + band) return { gap: -1, lane: -1, besideRow: hit, besideSide: 1 }
            if (cx > x0 + w - band) return { gap: -1, lane: -1, besideRow: hit, besideSide: 0 }
        }
        const lane = blockModel.laneForRow(hit)
        if (lane < 0) return top
        const mid = blockModel.yForRow(hit) + blockModel.heightForRow(hit) / 2
        return { gap: cy < mid ? hit : hit + 1, lane: lane, besideRow: -1, besideSide: -1 }
    }
    function aimBlockDrag(cx, cy) {
        let carries = false, wholeTable = false    // split rows move whole, between top-level rows only
        for (let k = blockDragRow; k < blockDragRow + blockDragCount; ++k) {
            if (blockModel.typeForRow(k) === 10) carries = true
            if (blockModel.headerCount(k) > 0) wholeTable = true
        }
        const aim = carries ? { gap: tableSafeGap(gapForY(cy), cy, wholeTable), lane: -1, besideRow: -1, besideSide: -1 }
                            : laneDropAim(cx, cy, blockDragRow, blockDragCount)
        dropGap = aim.gap; dropLane = aim.lane; dropBesideRow = aim.besideRow; dropBesideSide = aim.besideSide
    }
    // A drop line for gap `gap` in `lane` (content coords): {x, w, y}; w = -1 for a top-level line.
    function dropLineGeom(gap, lane) {
        if (gap < 0 || lane < 0) return { x: gutterX, w: -1, y: gap >= 0 ? gapY(gap) : 0 }
        const below = gap < blockModel.count && blockModel.laneForRow(gap) === lane
        const ref = below ? gap : gap - 1
        const g = laneOf(ref)
        return { x: leftEdge + g.x, w: g.w,
                 y: below ? blockModel.yForRow(gap) : blockModel.yForRow(ref) + blockModel.heightForRow(ref) }
    }
    // Content-y of a gap's drop line (top of that block, or doc end).
    function gapY(gap) {
        var n = blockModel.count
        return (gap >= n) ? blockModel.yForRow(n - 1) + blockModel.heightForRow(n - 1)
                          : blockModel.yForRow(gap)
    }
    // Gaps inside the dragged run (from..from+count) are no-ops; past it the
    // destination index shifts by the run's length.
    // In place is a no-op only when the run keeps its lane.
    function dropGapIsNoop(gap) {
        return gap >= blockDragRow && gap <= blockDragRow + blockDragCount
            && dropLane === blockModel.laneForRow(blockDragRow)
    }
    function commitBlockDrag() {
        if (blockDragRow >= 0 && dropBesideRow >= 0) {
            // Beside a block's edge: a new lane for the run (SR-3 S7c), grouped with the
            // markdown commit so the gesture is one undo step.
            const b = blockModel.splitRowsBand(Math.min(blockDragRow, dropBesideRow, cursor.focusRow),
                                               Math.max(blockDragRow + blockDragCount - 1, dropBesideRow, cursor.focusRow))
            blockModel.beginGroup(b[0], b[1])
            blockModel.commitMarkdown(cursor.focusRow)
            const land = blockModel.moveBeside(blockDragRow, blockDragCount, dropBesideRow, dropBesideSide)
            blockModel.endGroup()
            if (land >= 0) { cursor.setCaret(land, 0); root.ensureVisible(land) }
        } else if (blockDragRow >= 0 && dropGap >= 0 && !dropGapIsNoop(dropGap)) {
            var to = (dropGap > blockDragRow) ? dropGap - blockDragCount : dropGap
            root.moveRun(blockDragRow, blockDragCount, to, dropLane)
        }
        blockDragging = false; blockDragRow = -1; dropGap = -1; blockDragCount = 1
        dropLane = -1; dropBesideRow = -1; dropBesideSide = -1
    }
    // Move the run [from, from+count) so it starts at final index `to` — ONE
    // undo step (the focused row's inline markdown commits inside it), then
    // the caret and selection follow their blocks (the old code left the caret
    // on whatever slid into the vacated index).
    // lane: omitted = join the lane above the gap; -1 = the top level; k = lane k.
    function moveRun(from, count, to, lane) {
        const targetLane = lane === undefined ? -2 : lane
        if (count < 1 || to < 0 || to > blockModel.count - count) return
        if (to === from && (targetLane === -2 || targetLane === blockModel.laneForRow(from))) return
        // The group covers whole split rows: a lane the run leaves may collapse.
        const band = blockModel.splitRowsBand(Math.min(from, to, cursor.focusRow),
                                              Math.max(from, to, cursor.focusRow) + count - 1)
        blockModel.beginGroup(band[0], band[1])
        blockModel.commitMarkdown(cursor.focusRow)
        blockModel.moveBlocks(from, count, to, targetLane)
        blockModel.endGroup()                          // BEFORE the caret write
        cursor.anchorRow = blockModel.rowAfterMove(cursor.anchorRow, from, count, to)
        cursor.focusRow  = blockModel.rowAfterMove(cursor.focusRow,  from, count, to)
        cursor.goalX = -1
        cursor.sync()
        root.ensureVisible(cursor.focusRow)
    }
    // Context-menu Move up/down: the selected run when the menu row lies in
    // it, else the menu row alone.
    function moveMenuRow(d) {
        var r = root.menuRow
        if (cursor.hasSel && r >= cursor.loRow && r <= cursor.hiRow)
            root.moveRun(cursor.loRow, cursor.hiRow - cursor.loRow + 1, cursor.loRow + d)
        else
            root.moveRun(r, 1, r + d)
    }

    // The whole editor field is ONE tone now (user ruling 2026-07-12): the
    // page shares the desk grey and the desk RULES alone carry structure.
    Rectangle { anchors.fill: parent; color: Theme.colors.bgAlt }
    // AMENDED 2026-08-20 (user ruling): the SHEET — the page plus EQUAL
    // left/right margins (an ink gutter each side), stretched by wide content
    // (sheetRight) — keeps the field tone; the area beyond it drops to the
    // window-shell tone, so the document reads as a constrained shape that
    // follows the width setting. Both sides since the sheet centres
    // (2026-09-16). Tracks the pan and the ruler's live width preview
    // (pageWidth includes previewWidth).
    Rectangle {
        readonly property real sheetRight: root.sheetRight - flick.contentX
        visible: flick.visible && width > 0
        x: sheetRight
        width: Math.max(0, parent.width - sheetRight)
        height: parent.height
        color: Theme.colors.bg
    }
    Rectangle {
        visible: flick.visible && width > 0
        x: 0
        width: Math.max(0, root.sheetLeft - flick.contentX)
        height: parent.height
        color: Theme.colors.bg
    }
    MouseArea { anchors.fill: parent; onClicked: root.forceActiveFocus() }  // reclaim focus on bg click

    // --- Inline video player. ONE decoder, root-owned (a pooled MediaBlock
    // can't host a live decoder — it recycles mid-scroll). The surface + a
    // dedicated transport toolbar BELOW it are overlaid on the playing block,
    // which reserves videoTransportH extra height while it's the active player
    // (see the overlay below). A single shared decoder gives "one video at a
    // time" for free; scrolling the playing block out of view tears it down.
    // Transport logic is ported from ufb's VideoPreview. ---
    property int  videoPlayingRow: -1
    // T5 (S9c): a video whose column is narrower than this can't host the transport; its bar hides and
    // a click on the frame opens the review view instead.
    readonly property real transportMinW: 300
    // The active video's file path, captured at activation. Playhead banking keys
    // off THIS, not blockModel.mediaLocalPath(videoPlayingRow), so teardown stays
    // correct even when blockModel has already re-pointed to another note (a note
    // switch re-points it before our onActiveChanged handler runs).
    property string _videoPlayingPath: ""
    property bool videoLoop: false
    // ONE annotation-visibility switch for the whole app (inline player +
    // studio): when true the on-video stroke overlay and the note caption
    // hide so the clip can be watched clean. Note ticks and the studio
    // filmstrip stay — they're navigation, not overlay.
    property bool annotationsHidden: false
    // The screen-owning clip's QCView notes for ticks + caption (and the
    // studio filmstrip via vnotes directly). The revision read must be
    // LOAD-BEARING (ternary, not comma-tuple — reactivity rule 1e).
    readonly property var videoNoteArr: vnotes.revision >= 0 ? vnotes.noteList() : []
    // Review-speed playback (QCView parity: R cycles 0.5→0.75→1→1.25→1.5→2,
    // Shift+R resets, transport readout when ≠ 1x). Applied to BOTH the video
    // pacing divisor and the audio TempoStage; the re-anchoring seek right
    // after a change drops old-tempo ring residue so the sync servo restarts
    // clean (without it the servo fights stale audio). Persists across clips.
    property real videoSpeed: 1.0
    function setVideoSpeed(s) {
        videoSpeed = s
        videoDec.setPlaybackSpeed(s)
        videoAudio.setPlaybackTempo(s)
        if (videoDec.fps > 0) videoAudio.seek(_vidIntendedFrame() / videoDec.fps)
    }
    function cycleVideoSpeed() {
        var steps = [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]
        var nearest = 0, best = 1e9
        for (var i = 0; i < steps.length; ++i) {
            var d = Math.abs(steps[i] - videoSpeed)
            if (d < best) { best = d; nearest = i }
        }
        setVideoSpeed(steps[(nearest + 1) % steps.length])
    }
    // False from activation until the active video paints its first frame, so the
    // single shared surface never flashes the PREVIOUS video's stale frame — the
    // (correct) poster stays up until the new frame is ready.
    property bool _videoSurfaceReady: false
    readonly property real videoTransportH: 40
    readonly property real pdfNavH: 40          // reserved under an inline PDF page for the nav strip (matches kPdfNav)
    readonly property bool videoVisible: videoPlayingRow >= 0
        && activePdfRow < 0   // not in a full-frame tab
        && activeVideoRow < 0 && activeSketchRow < 0   // the studio has its own surface; sketch tab hides the doc
        && videoPlayingRow >= firstVisible && videoPlayingRow <= lastVisible
    // Scrolled away / entered a table or PDF tab → tear the player down. NOT
    // when the studio owns the decoder (its surface replaces the inline one).
    // Deferred (2026-08-20): stopVideo mutates videoPlayingRow, which this
    // binding reads — tearing down INSIDE the change handler re-entered the
    // binding and logged the (benign) videoVisible binding-loop warning.
    // callLater re-checks the condition at fire time, so a state that
    // changed back in the same tick doesn't tear down a live player.
    onVideoVisibleChanged: if (!videoVisible) Qt.callLater(root._maybeStopVideo)
    function _maybeStopVideo() {
        if (!videoVisible && videoPlayingRow >= 0 && activeVideoRow < 0) stopVideo()
    }

    // Per-video playhead memory (keyed by file path) so a torn-down video shows
    // its last frame as the poster and resumes there. videoPlayheadRev makes the
    // poster bindings re-evaluate when a playhead is recorded.
    property var videoPlayheads: ({})
    property int videoPlayheadRev: 0
    function videoPlayheadFor(row) {
        // Anchor-path key: pure computation (no IO), stable whether the clip
        // streams from a package or lives on disk.
        var key = blockModel.mediaAnchorPath(row)
        return (key !== "" && videoPlayheads[key] !== undefined) ? videoPlayheads[key] : 0
    }
    function _rememberVideoPlayhead() {   // bank the last-accessed frame
        if (videoPlayingRow < 0) return
        var key = videoPlayingRow >= 0 ? blockModel.mediaAnchorPath(videoPlayingRow) : ""
        // _vidIntendedFrame: the scrubbed-to frame if mid-scrub, else the live
        // playhead — scrubToFrame leaves currentFrame at the old streaming spot,
        // so reading currentFrame here would lose the scrub position.
        if (key !== "") { videoPlayheads[key] = _vidIntendedFrame(); videoPlayheadRev++ }
    }

    // Per-PDF current page, keyed by file path (stable across row shifts). The
    // pdfPageRev bump re-evaluates the page bindings when the nav changes it.
    property var pdfPages: ({})
    property int pdfPageRev: 0
    function pdfPageFor(row) {
        var key = blockModel.mediaViewPath(row)   // non-blocking (delegate bindings)
        return (key !== "" && pdfPages[key] !== undefined) ? pdfPages[key] : 0
    }
    function setPdfPage(row, page) {
        var n = blockModel.mediaPdfPages(row)
        var p = Math.max(0, Math.min(page, n - 1))
        var key = blockModel.mediaViewPath(row)
        if (key !== "") { pdfPages[key] = p; pdfPageRev++ }
    }
    function pdfStep(row, d) { setPdfPage(row, pdfPageFor(row) + d) }

    // -1 = not scrubbing. Otherwise the frame scrubbed to but not yet resumed
    // from (the streaming decoder is repositioned lazily on resume).
    property int _vidScrubTarget: -1
    function _vidIntendedFrame() { return _vidScrubTarget >= 0 ? _vidScrubTarget : videoDec.currentFrame }
    function _vidScrubTo(f) {
        f = Math.max(0, Math.min(videoDec.frameCount - 1, f))
        _vidScrubTarget = f
        videoDec.scrubToFrame(f)
        // While a shuttle gesture owns audio (drag-scrub or FF/RW grains),
        // skip the per-tick decoder re-seek — the old "seek storm". The
        // commit seek happens at gesture release (_vidSyncForResume).
        if (!videoAudio.shuttleActive() && videoDec.fps > 0)
            videoAudio.seek(f / videoDec.fps)
    }
    function _vidSyncForResume() {
        if (_vidScrubTarget < 0) return
        videoDec.seekToFrame(_vidScrubTarget)
        if (videoDec.fps > 0) videoAudio.seek(_vidScrubTarget / videoDec.fps)
        _vidScrubTarget = -1
    }

    // Make `row` the active video, opened PAUSED at its remembered playhead.
    // Does NOT start playback — so scrubbing/stepping a not-yet-playing video
    // shows frames without triggering play (ufb behaviour). Returns success.
    function _activateVideo(row) {
        if (videoPlayingRow === row) return true
        _rememberVideoPlayhead()              // bank the outgoing video's frame
        _videoSurfaceReady = false            // hide the surface until THIS video paints
        // A teardown can land mid-gesture (note switch during a drag/FF hold);
        // AudioPlayer.close() does NOT end the shuttle, and a live grain thread
        // would keep reading the OLD clip after the swap.
        if (videoAudio.shuttleActive()) videoAudio.endShuttle()
        _scrubAudioActive = false
        videoDec.close(); videoAudio.close()
        _vidScrubTarget = -1
        // Playback source: disk path, or a subfile spec streaming a packaged
        // clip straight from the archive — play starts without extraction.
        var p = blockModel.mediaPlaybackSource(row)
        if (p === "" || !videoDec.open(p)) { videoPlayingRow = -1; _videoPlayingPath = ""; return false }
        videoPlayingRow = row
        _videoPlayingPath = p
        videoAudio.initialize()   // idempotent; open() no-ops without it
        videoAudio.open(p)
        if (videoSpeed !== 1.0) {   // review speed persists across clips
            videoDec.setPlaybackSpeed(videoSpeed)
            videoAudio.setPlaybackTempo(videoSpeed)
        }
        var resume = videoPlayheadFor(row)    // pick up where we left off
        if (resume > 0) {
            videoDec.seekToFrame(resume)      // streaming decoder parks here (not a scrub)
            if (videoDec.fps > 0) videoAudio.seek(resume / videoDec.fps)
        }
        return true
    }
    function ensureVideoActive(row) { return _activateVideo(row) }
    function playVideo(row) {
        if (videoPlayingRow === row) { toggleVideo(); return }
        if (_activateVideo(row)) {
            videoDec.play()
            if (videoAudio.hasAudio) videoAudio.play()
        }
    }
    function toggleVideo() {
        if (videoPlayingRow < 0) return
        if (videoDec.isPlaying) { videoDec.pause(); videoAudio.pause() }
        else {
            if (videoDec.state === VideoDecoder.EndOfStream) { videoDec.seekToFrame(0); videoAudio.seek(0); _vidScrubTarget = -1 }
            else _vidSyncForResume()
            videoDec.play()
            if (videoAudio.hasAudio) videoAudio.play()
        }
    }
    function stopVideo() {
        _rememberVideoPlayhead()
        if (videoAudio.shuttleActive()) videoAudio.endShuttle()   // see _activateVideo
        _scrubAudioActive = false
        videoDec.close(); videoAudio.close()
        videoPlayingRow = -1; _videoPlayingPath = ""; _videoSurfaceReady = false
        _vidScrubTarget = -1; _vidFastSeekDir = 0; videoFastSeekTimer.stop()
    }
    // Frame-accurate step — implies review, so pause first.
    function stepVideoFrames(n) { videoDec.pause(); videoAudio.pause(); _vidScrubTo(_vidIntendedFrame() + n) }
    function seekVideoStart() { videoDec.pause(); videoAudio.pause(); _vidScrubTo(0) }
    function seekVideoEnd()   { videoDec.pause(); videoAudio.pause(); _vidScrubTo(videoDec.frameCount - 1) }
    // Note-card click: park exactly on the note's frame (review → pause first).
    function seekVideoFrame(f) { videoDec.pause(); videoAudio.pause(); _vidScrubTo(f) }
    function toggleVideoMute(){ if (videoAudio.hasAudio) videoAudio.setMuted(!videoAudio.muted) }
    function toggleVideoLoop(){ videoLoop = !videoLoop }

    // Accelerating fast-seek shuttle (held rewind/ff): a 33 ms timer advances a
    // position at 2x→32x (doubles/sec) and scrubs to it each tick.
    property int  _vidFastSeekDir: 0
    property real _vidFastSeekSpeed: 2.0
    property real _vidFastSeekElapsed: 0
    property real _vidFastSeekPos: 0
    function startVideoFastSeek(dir) {
        dir = dir > 0 ? 1 : -1
        if (_vidFastSeekDir === dir) return
        videoDec.pause(); videoAudio.pause()
        // Shuttle and review speed are mutually exclusive transports —
        // entering the gesture snaps the rate back to 1x (QCView rule).
        if (videoSpeed !== 1.0) setVideoSpeed(1.0)
        _vidFastSeekDir = dir; _vidFastSeekSpeed = 2.0; _vidFastSeekElapsed = 0
        _vidFastSeekPos = (videoDec.fps > 0) ? _vidIntendedFrame() / videoDec.fps : 0
        // Deck-style grain audio follows the fast-seek position (engine-side
        // no-op when the clip has no audio).
        if (videoAudio.hasAudio && _videoPlayingPath !== "")
            videoAudio.beginShuttle(_videoPlayingPath, _vidFastSeekPos,
                                    2.0 * dir, videoAudio.routingMode())
        videoFastSeekTimer.start()
    }
    function stopVideoFastSeek() {
        _vidFastSeekDir = 0; videoFastSeekTimer.stop()
        if (videoAudio.shuttleActive()) videoAudio.endShuttle()
    }

    // ---- Drag-scrub audio (transport-slider gesture) ----
    // Skim-style grains through the shuttle engine (natural pitch above 1x,
    // varispeed below), speed ESTIMATED from drag velocity: signed
    // source-seconds per wall second, EMA-smoothed with a dt-scaled alpha
    // (tau ≈ 40 ms) so mouse jitter doesn't warble the grain pitch. Tau was
    // 80 ms when above-1x jitter was audible; with the engine's 1x pitch cap
    // only sub-1x drags hear the estimate, so a faster tracker wins. The
    // engine's hold detection silences a stationary mouse.
    property bool _scrubAudioActive: false
    property real _scrubVelLastMs: 0
    property real _scrubVelLastSec: 0
    property real _scrubVelEma: 0
    function beginScrubAudio(sec) {
        if (!videoAudio.hasAudio || _videoPlayingPath === "") { _scrubAudioActive = false; return }
        if (videoSpeed !== 1.0) setVideoSpeed(1.0)   // mutually exclusive transports
        _scrubAudioActive = true
        _scrubVelLastMs = Date.now(); _scrubVelLastSec = sec; _scrubVelEma = 0
        videoAudio.beginShuttle(_videoPlayingPath, sec, 0.0, videoAudio.routingMode())
    }
    function scrubAudioMove(sec) {
        if (!_scrubAudioActive) return
        var now = Date.now()
        var dt = (now - _scrubVelLastMs) * 1e-3
        if (dt > 0.0005) {
            var v = Math.max(-32, Math.min(32, (sec - _scrubVelLastSec) / dt))
            var alpha = 1.0 - Math.exp(-dt / 0.040)
            _scrubVelEma += alpha * (v - _scrubVelEma)
            _scrubVelLastMs = now; _scrubVelLastSec = sec
        }
        videoAudio.shuttleTarget(_videoPlayingPath, sec, _scrubVelEma)
    }
    function endScrubAudio() {
        if (!_scrubAudioActive) return
        _scrubAudioActive = false
        if (videoAudio.shuttleActive()) videoAudio.endShuttle()
    }

    VideoDecoder { id: videoDec }
    AudioPlayer  { id: videoAudio }
    // The note store: QCView's sidecar (.qcview/<media>/notes.json), loaded
    // for whichever video owns the screen — the studio's row when a video tab
    // is open (poster case included), else the inline player's. Root-owned so
    // the inline overlay + transports and the studio share ONE copy (two
    // models on the same notes.json would only sync through the file
    // watcher). Lives OUTSIDE the document + undo — it travels with the
    // video, shared with QCView.
    VideoNotesModel {
        id: vnotes
        readonly property int noteRow: root.activeVideoRow >= 0 ? root.activeVideoRow
                                                                : root.videoPlayingRow
        mediaPath: noteRow >= 0 ? blockModel.mediaAnchorPath(noteRow) : ""
        fps: noteRow >= 0 ? blockModel.mediaFps(noteRow) : 0
    }
    // Keep audio aligned to the video playhead while playing (~30 Hz).
    Timer {
        interval: 33; repeat: true
        running: videoDec.isPlaying && videoAudio.hasAudio
        onTriggered: videoAudio.update(videoDec.fps > 0 ? videoDec.currentFrame / videoDec.fps : 0)
    }
    Timer {
        id: videoFastSeekTimer
        interval: 33; repeat: true; running: false
        onTriggered: {
            if (root._vidFastSeekDir === 0 || videoDec.fps <= 0 || videoDec.frameCount <= 0) return
            var dt = interval / 1000
            root._vidFastSeekElapsed += dt
            root._vidFastSeekSpeed = Math.min(32.0, 2.0 * Math.pow(2.0, root._vidFastSeekElapsed))
            root._vidFastSeekPos += dt * root._vidFastSeekSpeed * root._vidFastSeekDir
            var maxSec = (videoDec.frameCount - 1) / videoDec.fps
            root._vidFastSeekPos = Math.max(0, Math.min(maxSec, root._vidFastSeekPos))
            root._vidScrubTo(Math.round(root._vidFastSeekPos * videoDec.fps))
            // Grain audio follows the integrated position + signed ramp speed
            // (no-op unless beginShuttle ran at gesture start).
            if (videoAudio.shuttleActive())
                videoAudio.shuttleTarget(root._videoPlayingPath, root._vidFastSeekPos,
                                         root._vidFastSeekSpeed * root._vidFastSeekDir)
        }
    }
    Connections {
        target: videoDec
        function onStateChanged() {
            if (videoDec.state === VideoDecoder.EndOfStream) {
                if (root.videoLoop) {
                    root._vidScrubTarget = -1
                    videoDec.seekToFrame(0); videoAudio.seek(0)
                    videoDec.play(); if (videoAudio.hasAudio) videoAudio.play()
                } else {
                    videoAudio.pause()
                }
            }
        }
        // First frame of the active video has been published → safe to reveal the
        // surface (it now holds THIS video, not the previous one).
        function onFrameAvailable() { if (!root._videoSurfaceReady) root._videoSurfaceReady = true }
    }

    // Drag-drop image files in → media blocks at the snapped insertion gap. Tracks
    // the drag so the overlay below can show exactly where it'll land. (Only handles
    // external drags; doesn't touch the normal mouse interaction.)
    property bool imageDropActive: false
    property int  imageDropGap: -1
    // Tab-merge drag (0.4.0): driven from Main.qml's strip drag. Shares the
    // image-drop pulsing indicator (the dropIndicator* pair below) and the
    // edge auto-scroll timer with the other drags.
    property bool mergeDragActive: false
    property int  mergeDropGap: -1
    property real mergeDragViewY: 0
    readonly property bool dropIndicatorActive: imageDropActive || mergeDragActive
    readonly property int  dropIndicatorGap: imageDropActive ? imageDropGap : mergeDropGap
    // Aim the merge drag at an editor-local point. Full-frame tabs (table/
    // pdf/video/sketch) have no document surface — no gap there, so a
    // release over them is a no-op rather than a blind insert.
    function aimMergeDrop(ex, ey) {
        if (activeFrameId !== "" || ex < 0 || ex > width || ey < 0 || ey > height) {
            mergeDragActive = false; mergeDropGap = -1; return
        }
        mergeDragActive = true
        mergeDragViewY = ey
        mergeDropGap = mergeGapForY(ey + flick.contentY)
    }
    function endMergeDrop() { mergeDragActive = false; mergeDropGap = -1 }
    // A table's cell under the files (SR-4 S7b); −1 = none. When set, the
    // block-insertion gap line is suppressed and the cell is highlighted.
    property int dropGridHead: -1
    property int dropGridR: -1
    property int dropGridC: -1
    function clearDropState() {
        imageDropGap = -1
        imageDropLane = -1; imageDropBesideRow = -1; imageDropBesideSide = -1
        dropGridHead = -1; dropGridR = -1; dropGridC = -1
    }
    // The derived-table cell under content point (cx, cy) — inside the grid, between the pockets —
    // as {head, r, c, accepts}, or null. A typed body cell doesn't take files (it holds one chip).
    function tableCellAtPoint(cx, cy) {
        const rec = blockModel.rowForY(Math.max(0, cy))
        if (rec < 0 || blockModel.typeForRow(rec) !== 10) return null
        const head = blockModel.tableHeadOf(rec)
        if (head < 0) return null
        const y0 = blockModel.yForRow(rec)
        if (cy < y0 + blockModel.tablePadTop(rec) || cy >= y0 + blockModel.heightForRow(rec) - blockModel.tablePadBottom(rec))
            return null
        const c = tableColumnAtX(head, cx - leftEdge)
        if (c < 0) return null
        const r = blockModel.tableRowOf(rec)
        return { head: head, r: r, c: c, accepts: r < blockModel.headerCount(head) || blockModel.tableColumnKind(head, c) === 0 }
    }
    // Aim a drag at a content point: a table cell wins (→ media into the cell), else a
    // block-insertion gap. Mutually exclusive, so the affordances don't both show.
    function aimDrop(cx, cy) {
        imageDropLane = -1; imageDropBesideRow = -1; imageDropBesideSide = -1
        dropGridHead = -1; dropGridR = -1; dropGridC = -1
        const gc = root.tableCellAtPoint(cx, cy)                 // a table's cell (a typed one: no target)
        if (gc) {
            if (gc.accepts) { dropGridHead = gc.head; dropGridR = gc.r; dropGridC = gc.c }
            imageDropGap = -1
            return
        }
        const aim = root.laneDropAim(cx, cy, -1, 0)   // a lane gap, a side edge, or a top-level gap
        imageDropGap = aim.gap; imageDropLane = aim.lane
        imageDropBesideRow = aim.besideRow; imageDropBesideSide = aim.besideSide
    }
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onEntered: (drag) => {
            if (root.activeFrameId !== "") return   // frame tab: no doc indicator
            root.imageDropActive = true
            root.aimDrop(drag.x + flick.contentX, drag.y + flick.contentY)
        }
        onPositionChanged: (drag) => {
            if (root.activeFrameId !== "") return
            root.aimDrop(drag.x + flick.contentX, drag.y + flick.contentY)
        }
        onExited: { root.imageDropActive = false; root.clearDropState() }
        onDropped: (drop) => {
            root.imageDropActive = false
            if (!drop.hasUrls) { root.clearDropState(); return }
            // Full-frame tabs cover the document (regression fix 2026-08-21:
            // drops used to fall THROUGH to the hidden document). A sketch
            // tab takes the image(s) onto its canvas; the other frame tabs
            // ignore the drop (the table tab's cells have their own
            // dedicated DropArea).
            if (root.activeSketchRow >= 0) {
                for (var si = 0; si < drop.urls.length; ++si)
                    blockModel.sketchAddImageFromUrl(root.activeSketchRow, drop.urls[si].toString(), false)
                root.clearDropState()
                drop.accept()
                return
            }
            if (root.activeFrameId !== "") { root.clearDropState(); return }
            // A drop lands the caret on the new media → leaving the edited text block, so
            // consume its inline md first (commit doesn't move rows, so the drop-target
            // indices below stay valid).
            blockModel.commitMarkdown(cursor.focusRow)
            if (root.dropGridHead >= 0) {             // over a table's cell → media blocks in it (S7b)
                const urls = []
                for (let j = 0; j < drop.urls.length; ++j) urls.push(drop.urls[j].toString())
                const land = blockModel.tableInsertMedia(root.dropGridHead, root.dropGridR, root.dropGridC, urls)
                root.clearDropState()
                if (land >= 0) { cursor.setCaret(land, 0); root.ensureVisible(land) }
                drop.accept()
                return
            }
            var afterRow = root.imageDropGap - 1      // insert AT the gap (= after gap-1)
            var lane = root.imageDropLane             // the lane the gap sits in (-1 = top level)
            var any = false
            for (var i = 0; i < drop.urls.length; ++i) {
                // Inserts return the ACTUAL new row (an empty-paragraph anchor
                // is consumed, shifting placement) — chain from it, never +1.
                // On a block's edge the first file makes a lane beside it; the rest
                // follow it down that lane.
                var u = drop.urls[i].toString(), nr = -1
                if (!any && root.imageDropBesideRow >= 0)
                    nr = blockModel.insertMediaBeside(root.imageDropBesideRow, root.imageDropBesideSide, u)
                else if (afterRow >= -1)
                    nr = blockModel.insertMediaAt(afterRow + 1, lane, u)
                if (nr >= 0) { afterRow = nr; lane = blockModel.laneForRow(nr); any = true }
            }
            root.clearDropState()
            if (any) { cursor.setCaret(Math.max(0, afterRow), 0); root.ensureVisible(afterRow) }
            drop.accept()
        }
    }

    // Rich paste outcome (the paster applies synchronously unless assets are
    // being copied across documents, when this lands after the worker).
    Connections {
        target: paster
        function onPasteFinished(ok, cr, cc, error) {
            if (!ok) {
                if (error.length > 0)
                    Toasts.show(error === "Cancelled" ? qsTr("Paste cancelled")
                                                      : qsTr("Paste failed — ") + error, 2)
                return
            }
            if (cr >= 0) { cursor.setCaret(cr, Math.max(0, cc)); root.ensureVisible(cr) }
            if (blockModel.lastPasteRelocated())
                Toasts.show(qsTr("Pasted below the split row — split rows and tables can't go inside a lane"))
        }
    }

    // --- Logical cursor + editing ops. Sole owner of caret/selection/content.
    QtObject {
        id: cursor
        property int focusRow: 0
        property int focusCol: 0
        property int anchorRow: 0
        property int anchorCol: 0
        readonly property bool anchorFirst: anchorRow < focusRow
                                            || (anchorRow === focusRow && anchorCol <= focusCol)
        readonly property int _lo: anchorFirst ? anchorRow : focusRow
        readonly property int _loC: anchorFirst ? anchorCol : focusCol
        readonly property int _hi: anchorFirst ? focusRow : anchorRow
        readonly property int _hiC: anchorFirst ? focusCol : anchorCol
        // SR-0 §4.9 row ranges: ends in different top-level rows (not one table) take the split rows and
        // tables they reach into whole — [lo, hi] widened, or null. Every reader of the selection (washes,
        // copy, delete, the run menu, the rail drag) sees the widened range.
        readonly property var _span: (blockModel.contentRevision, root.rowRangeSpan(_lo, _hi))
        readonly property int loRow: _span ? _span[0] : _lo
        readonly property int loCol: _span && _span[0] !== _lo ? 0 : _loC
        readonly property int hiRow: _span ? _span[1] : _hi
        readonly property int hiCol: _span && _span[1] !== _hi ? blockModel.contentForRow(_span[1]).length : _hiC
        readonly property bool hasSel: loRow !== hiRow || loCol !== hiCol

        // Sticky goal-x for vertical nav: the x the caret aims for across a RUN
        // of up/down presses, so it doesn't drift toward shorter lines. te-local
        // (distance from text start), so it's consistent across blocks. -1 = unset;
        // any horizontal move / edit / click resets it (see resetGoalX callers).
        property real goalX: -1
        function resetGoalX() { goalX = -1 }

        // Word-style armed typing attributes (active when nothing is selected):
        // bold=1, italic=2, code=4. Applied to typed text; cleared on caret nav.
        property int activeMarks: 0
        // Persistent "type colour" / "highlight" pens: set by picking in the
        // palette (or the highlight toggle); typed text gets them. "" = none.
        // Cleared on caret nav (like the armed marks): survive a run, not nav.
        property string armedFg: ""
        property string armedBg: ""
        function toggleMark(kind) {
            var bit = kind === "bold" ? 1 : kind === "italic" ? 2 : kind === "code" ? 4
                    : kind === "strike" ? 8 : kind === "underline" ? 16 : 0
            if (bit) activeMarks ^= bit
        }
        function clearMarks() { activeMarks = 0; armedFg = ""; armedBg = "" }

        // The caret stays hidden until the user first interacts (click / key /
        // type), so the app opens with nothing active. sync() is the chokepoint
        // for every caret change, so flag it active there.
        property bool active: false

        // Mirror the caret into the model so undo transactions can snapshot it
        // (and stamp a just-pushed entry's caret-after). Called after any change.
        // sync() is the chokepoint for every caret change — leaving a block is
        // the checker's "settled" hook (flushRow bypasses its debounce).
        property int lastSyncRow: -1
        function sync() {
            active = true
            blockModel.noteCaret(focusRow, focusCol, anchorRow, anchorCol)
            if (lastSyncRow >= 0 && lastSyncRow !== focusRow) spell.flushRow(lastSyncRow)
            lastSyncRow = focusRow
        }

        function setCaret(r, col) { anchorRow = r; anchorCol = col; focusRow = r; focusCol = col; goalX = -1; sync() }
        function move(r, col, extend) {
            // Leaving a block with a collapsed caret consumes its markdown → spans.
            if (!extend && r !== focusRow && !root.dragging) blockModel.commitMarkdown(focusRow)
            focusRow = r; focusCol = col
            if (!extend) { anchorRow = r; anchorCol = col }
            root.ensureVisible(r)
            sync()
        }
        // Block-grain selection (2026-09-09): opaque rows (table/media/divider)
        // are whole-in or whole-out. An opaque LOW end starts at col 0, an
        // opaque HIGH end ends at its content length. The model's
        // deleteSelectionRange applies the same rule on delete.
        function effectiveRange() {
            var lR = loRow, lC = loCol, hR = hiRow, hC = hiCol
            if (opaque(lR)) lC = 0
            if (opaque(hR)) hC = blockModel.contentForRow(hR).length
            return { lR: lR, lC: lC, hR: hR, hC: hC }
        }
        function deleteSelection() {
            if (!hasSel) return
            if (root.selObjectValid()) { root.deleteSelectedObject(); return }   // a table row / table picked by Escape
            var e = effectiveRange()
            var land = blockModel.deleteSelectionRange(e.lR, e.lC, e.hR, e.hC)
            var r = (land && land.length === 2) ? land[0] : e.lR
            var c = (land && land.length === 2) ? land[1] : 0
            anchorRow = r; anchorCol = c; focusRow = r; focusCol = c
            goalX = -1
            root.ensureVisible(r)
            sync()
        }
        // Opaque blocks (media/divider) hold non-prose content — a cross-block text
        // merge would spill it. Never merge across one.
        function opaque(r) { var t = blockModel.typeForRow(r); return t === 3 || t === 6 }
        // Is the CARET on a media/divider? These blocks have no text caret, so text
        // ops must not edit them.
        function opaqueHere() { var t = blockModel.typeForRow(focusRow); return t === 3 || t === 6 }
        // `repeat` = key auto-repeat: it never makes a structural change (SR-0 P6) — a held
        // Backspace/Delete won't delete a selected or focused media/divider, remove a block
        // beside a split row, or collapse a lane. Text merges still repeat.
        function backspace(repeat) {
            if (hasSel) { if (!repeat) deleteSelection(); return }
            if (opaqueHere()) { if (!repeat) root.deleteBlock(focusRow); return }   // caret on media/divider → delete it
            if (focusCol > 0) {
                // A choice chip is atomic (DT-2): a backspace touching ANY of
                // its text removes the WHOLE chip, label and all.
                var cbr = blockModel.choiceRangeAt(focusRow, focusCol - 1)
                if (cbr.length === 2) {
                    blockModel.removeChoiceAt(focusRow, cbr[0])
                    setCaret(focusRow, cbr[0])
                    return
                }
                blockModel.deleteRange(focusRow, focusCol - 1, focusRow, focusCol)
                setCaret(focusRow, focusCol - 1)
            } else root.backspaceAtStart(repeat)
        }
        function forwardDelete(repeat) {
            if (hasSel) { if (!repeat) deleteSelection(); return }
            if (opaqueHere()) { if (!repeat) root.deleteBlock(focusRow); return }   // caret on media/divider → delete it
            var len = blockModel.contentForRow(focusRow).length
            if (focusCol < len) {
                // Chip atomicity, forward direction: deleting at its left
                // edge (or inside) removes the whole chip.
                var cfr = blockModel.choiceRangeAt(focusRow, focusCol)
                if (cfr.length === 2) {
                    blockModel.removeChoiceAt(focusRow, cfr[0])
                    setCaret(focusRow, cfr[0])
                    return
                }
                blockModel.deleteRange(focusRow, focusCol, focusRow, focusCol + 1)
                setCaret(focusRow, focusCol)
            } else root.deleteAtEnd(repeat)
        }
        function insertChar(ch) {
            if (hasSel) deleteSelection()                 // FIRST — the caret may land on an opaque row
            if (opaqueHere()) {                           // typing next to a media/divider → fresh paragraph after it
                blockModel.insertBlock(focusRow + 1); setCaret(focusRow + 1, 0)
            }
            {   // never type INTO a chip: a caret strictly inside hops to its end
                var cir = blockModel.choiceRangeAt(focusRow, focusCol)
                if (cir.length === 2 && focusCol > cir[0]) setCaret(focusRow, cir[1])
            }
            blockModel.insertText(focusRow, focusCol, ch, activeMarks, armedFg, armedBg)   // armed attrs + pens → span the run
            setCaret(focusRow, focusCol + ch.length)
            // Markdown autoformat fires on the space that completes a prefix
            // (e.g. "## "): the prefix is consumed, so pull the caret back.
            if (ch === " ") {
                var strip = blockModel.applyMarkdownTrigger(focusRow)
                if (strip > 0) setCaret(focusRow, Math.max(0, focusCol - strip))
            }
        }
        function splitLine(shift, repeat) {
            if (hasSel) deleteSelection()
            if (opaqueHere()) {   // Enter on a media/divider → a fresh paragraph below it
                blockModel.insertBlock(focusRow + 1); setCaret(focusRow + 1, 0); root.ensureVisible(focusRow + 1); return
            }
            // "```" / "```lang" + Enter → an (empty) code block; caret stays inside.
            if (blockModel.makeCodeBlockIfFence(focusRow)) { setCaret(focusRow, 0); return }
            // Inside a code block, Enter adds a newline; pressing it on an empty
            // trailing line exits to a fresh paragraph below. Shift+Enter is
            // ALWAYS a newline (user ruling 2026-08-21) — the deliberate way
            // to add trailing blank lines without tripping the exit rule.
            if (blockModel.typeForRow(focusRow) === 2) {
                var c = blockModel.contentForRow(focusRow)
                var atEnd = focusCol >= c.length
                if (!shift && atEnd && (c.length === 0 || c.charAt(c.length - 1) === "\n")) {
                    if (c.length > 0) blockModel.deleteRange(focusRow, c.length - 1, focusRow, c.length)
                    blockModel.splitBlock(focusRow, blockModel.contentForRow(focusRow).length)
                    setCaret(focusRow + 1, 0); root.ensureVisible(focusRow + 1)
                    return
                }
                blockModel.insertText(focusRow, focusCol, "\n", 0)
                setCaret(focusRow, focusCol + 1)
                return
            }
            // "---"/"***"/"___" + Enter → divider, then a fresh paragraph below.
            if (blockModel.makeDividerIfMarker(focusRow)) {
                blockModel.insertBlock(focusRow + 1)
                setCaret(focusRow + 1, 0)
                root.ensureVisible(focusRow + 1)
                return
            }
            // Enter on an EMPTY list item exits the list instead of continuing
            // it: outdent one level first if nested, else back to a paragraph.
            // (splitBlock continues non-empty items at the same type/depth.)
            var lt = blockModel.typeForRow(focusRow)
            if ((lt === 5 || lt === 8 || lt === 9)
                && blockModel.contentForRow(focusRow).length === 0) {
                if (blockModel.depthForRow(focusRow) > 0)
                    blockModel.indentBlocks(focusRow, focusRow, -1)
                else
                    blockModel.setBlockType(focusRow, 0)
                setCaret(focusRow, 0)
                return
            }
            // SR-4 A1: Enter in a table row navigates — the same column in the next row; the last
            // row appends one (copying its divisions); an empty last body row exits the table as a
            // paragraph. Shift+Enter splits the block inside its cell.
            if (!shift && blockModel.tableHeadOf(focusRow) >= 0 && (lt === 0 || lt === 1 || lt === 4)) {
                root.tableEnter(repeat === true)
                return
            }
            // A lane's double Enter (2026-09-14 walk): Enter on an empty last block of a layout lane
            // leaves the split row — the block goes (unless it's the lane's only one) and the caret
            // lands in a paragraph below. Shift+Enter still adds a block; a held key never exits.
            if (!shift && repeat !== true && lt === 0 && blockModel.laneForRow(focusRow) >= 0
                && blockModel.tableHeadOf(focusRow) < 0 && blockModel.contentForRow(focusRow).length === 0) {
                const out = blockModel.exitLane(focusRow)
                if (out >= 0) { setCaret(out, 0); root.ensureVisible(out); return }
            }
            var leftRow = focusRow
            blockModel.splitBlock(focusRow, focusCol)
            setCaret(focusRow + 1, 0)
            // Enter finishes the line being left → consume its inline markdown
            // (*italic*, **bold**, `code`) into spans, same as moving the caret
            // off a block does (see cursor.move). The caret is already on the new
            // row, so converting the left row never shifts it.
            blockModel.commitMarkdown(leftRow)
            root.ensureVisible(focusRow + 1)
        }
    }

    // Undo/redo restore the caret (and selection) the model snapshotted.
    Connections {
        target: blockModel
        function onCaretRestoreRequested(r, c, ar, ac) {
            var n = blockModel.count
            r = Math.max(0, Math.min(r, n - 1))
            ar = Math.max(0, Math.min(ar, n - 1))
            cursor.anchorRow = ar; cursor.anchorCol = Math.max(0, Math.min(ac, blockModel.contentForRow(ar).length))
            cursor.focusRow = r;   cursor.focusCol = Math.max(0, Math.min(c, blockModel.contentForRow(r).length))
            cursor.goalX = -1
            // Full-frame tabs (PDF/video/sketch/table) hide the document —
            // scrolling it for a caret nobody can see just moves the hidden
            // view (undoing page ink from a PDF tab, the banked nit). The
            // caret state itself still restores for when the doc returns.
            if (flick.visible) root.ensureVisible(r)
            cursor.sync()
        }
    }

    // Selection rects for a [sp,ep) range within one block, ONE PER VISUAL LINE
    // so a wrapped selection highlights correctly (first line → right edge,
    // full middle lines, last line → end). This is what makes selection "see
    // lines, not just blocks."
    function selectionRects(te, sp, ep) {
        var out = []
        if (sp >= ep) return out
        var rs = te.positionToRectangle(sp)
        var re = te.positionToRectangle(ep)
        var lh = rs.height > 0 ? rs.height : 18
        if (Math.abs(rs.y - re.y) < lh * 0.5) {
            out.push(Qt.rect(rs.x, rs.y, Math.max(2, re.x - rs.x), lh))
        } else {
            out.push(Qt.rect(rs.x, rs.y, Math.max(2, te.width - rs.x), lh))
            for (var y = rs.y + lh; y < re.y - lh * 0.5; y += lh)
                out.push(Qt.rect(0, y, te.width, lh))
            out.push(Qt.rect(0, re.y, Math.max(2, re.x), lh))
        }
        return out
    }

    function ensureVisible(rowIdx) {
        var y = blockModel.yForRow(rowIdx)
        var h = blockModel.heightForRow(rowIdx)
        var top = flick.contentY, bot = flick.contentY + flick.height
        if (h >= flick.height) {
            // Block taller than the viewport (a big image/video): forcing its top
            // OR bottom into view oscillates during a drag-select — each event flips
            // between the two, which reads as the scroll getting stuck/stuttering.
            // So only move if it's ENTIRELY off-screen; once any part shows, leave
            // the scroll alone (wheel/trackpad still scrolls through it freely).
            if (y + h <= top) flick.contentY = y + h - flick.height       // entirely above → its bottom
            else if (y >= bot) flick.contentY = y                          // entirely below → its top
            return
        }
        if (y < top) flick.contentY = y
        else if (y + h > bot)
            flick.contentY = Math.min(flick.contentHeight - flick.height, y + h - flick.height)
    }

    // --- Mouse hit-testing. The passive-surface architecture makes this the
    // clean path to cross-block selection: blockAt() finds the block, then that
    // block's own TextEdit maps pixels → column via positionAt().
    function cellForRow(r) {
        const s = (root.slotRev, viewSlots.slotForRow(r))
        const c = s >= 0 ? pool.itemAt(s) : null
        return (c && c.active && c.logicalRow === r) ? c : null
    }
    // (cx, cy) in CONTENT coordinates → {row, col}.
    function hitTest(cx, cy) {
        var row = blockModel.blockAt(root.pageXAt(cx, cy), Math.max(0, cy))
        var cell = cellForRow(row)
        if (!cell || cell.isMedia) return { row: row, col: 0 }
        var te = cell.teItem
        var col = te.positionAt(cx - cell.x - te.x, cy - cell.y - te.y)
        return { row: row, col: col }
    }
    // (cx, cy) in CONTENT coordinates → the row index if the click is over a task
    // item's checkbox glyph (the left decoration column, first line), else -1. Its
    // delegate can't own a MouseArea (the document mouse layer sits above it), so the
    // central handler hit-tests the glyph zone here.
    function taskCheckboxAt(cx, cy) {
        var row = blockModel.blockAt(root.pageXAt(cx, cy), Math.max(0, cy))
        if (blockModel.typeForRow(row) !== 8) return -1
        var cell = cellForRow(row)
        if (!cell) return -1
        var te = cell.teItem
        var lx = cx - cell.x - cell.colLeft  // x within the cell's content column
        var ly = cy - cell.y - te.y          // y relative to the text top
        if (lx >= 0 && lx <= 20 && ly >= -2 && ly <= te.lineH) return row
        return -1
    }
    // (cx, cy) in CONTENT coordinates → the row index if the click lands on a
    // code block's language chip (top-right pill), else -1. Same central-layer
    // hit-testing as the task checkbox — the delegate can't own a MouseArea.
    property int codeChipHoverRow: -1
    function codeLangChipAt(cx, cy) {
        var row = blockModel.blockAt(root.pageXAt(cx, cy), Math.max(0, cy))
        if (blockModel.typeForRow(row) !== 2) return -1
        var cell = cellForRow(row)
        if (!cell || !cell.langChip || !cell.langChip.visible) return -1
        var p = cell.langChip.mapFromItem(mouse, cx, cy)
        var pad = 2
        if (p.x >= -pad && p.x <= cell.langChip.width + pad
            && p.y >= -pad && p.y <= cell.langChip.height + pad) return row
        return -1
    }
    // --- Central navigation. Uses the focus block's text layout for vertical
    // moves; crosses boundaries at the text edges. Single focus holder → the
    // caret the user sees and the row the keys act on can never diverge.
    // An arrow run off the document's end (or start) inside a split row or table would trap the
    // caret (2026-09-14 walk): a paragraph appears below (or above) the row and the caret moves in.
    function leaveSplitRowAtEdge(down, shift) {
        if (shift || blockModel.splitRowOf(cursor.focusRow) < 0) return
        const p = down ? blockModel.insertParagraphBelow(cursor.focusRow) : blockModel.insertParagraphAbove(cursor.focusRow)
        if (p < 0) return
        cursor.setCaret(p, 0)
        root.ensureVisible(p)
    }
    function navRight(shift) {
        cursor.resetGoalX(); cursor.clearMarks()
        var fb = root.focusBlockItem, n = blockModel.count
        if (fb && cursor.focusCol < fb.length) {
            // Chips are atomic (DT-2): stepping INTO one hops to its far edge.
            var cr = blockModel.choiceRangeAt(cursor.focusRow, cursor.focusCol)
            cursor.move(cursor.focusRow,
                        cr.length === 2 ? cr[1] : cursor.focusCol + 1, shift)
        }
        else {                               // the next block in reading order (records skipped)
            const nx = blockModel.nextLeaf(cursor.focusRow)
            if (nx < 0) { root.leaveSplitRowAtEdge(true, shift); return }
            cursor.move(nx, 0, shift)
        }
    }
    function navLeft(shift) {
        cursor.resetGoalX(); cursor.clearMarks()
        if (cursor.focusCol > 0) {
            var cl = blockModel.choiceRangeAt(cursor.focusRow, cursor.focusCol - 1)
            cursor.move(cursor.focusRow,
                        cl.length === 2 ? cl[0] : cursor.focusCol - 1, shift)
        }
        else {                               // the previous block in reading order (records skipped)
            const pv = blockModel.prevLeaf(cursor.focusRow)
            if (pv < 0) { root.leaveSplitRowAtEdge(false, shift); return }
            cursor.move(pv, blockModel.contentForRow(pv).length, shift)
        }
    }
    // Backspace at a block's start with no selection (SR-0 §4.2). Blocks merge only within
    // their container (a lane, or the top level); a media/divider block above is selected
    // first — the caret moves onto it and the next press deletes it.
    function backspaceAtStart(repeat) {
        const row = cursor.focusRow
        const lane = blockModel.laneForRow(row)
        const prev = (row > 0 && blockModel.laneForRow(row - 1) === lane
                      && blockModel.typeForRow(row - 1) !== 10) ? row - 1 : -1
        if (prev < 0) {
            if (lane >= 0) {                      // case 4: the first block of a lane
                if (repeat) return
                const land = blockModel.collapseEmptyLane(row, false)   // [] unless it's the lane's only, empty block
                if (land.length === 2) { cursor.setCaret(land[0], land[1]); root.ensureVisible(land[0]) }
                return
            }
            const pv = blockModel.prevLeaf(row)   // case 3: a split row above (case 5: document start)
            if (pv < 0) return
            if (!repeat && blockModel.typeForRow(row) === 0 && blockModel.contentForRow(row).length === 0
                && blockModel.count > 1)
                blockModel.removeBlock(row)       // the break-removal gesture: nothing merges
            const pvt = blockModel.typeForRow(pv)
            cursor.setCaret(pv, (pvt === 3 || pvt === 6) ? 0 : blockModel.contentForRow(pv).length)
            root.ensureVisible(pv)
            return
        }
        const pt = blockModel.typeForRow(prev)   // case 2: the previous block in this container
        if (pt === 3 || pt === 6) { cursor.setCaret(prev, 0); root.ensureVisible(prev); return }   // select first
        const pl = blockModel.contentForRow(prev).length
        blockModel.deleteRange(prev, pl, row, 0)
        cursor.setCaret(prev, pl)
        root.ensureVisible(prev)
    }
    // Delete at a block's end with no selection (SR-0 §4.3) — the mirror of backspaceAtStart.
    function deleteAtEnd(repeat) {
        const row = cursor.focusRow
        const lane = blockModel.laneForRow(row)
        const next = (row + 1 < blockModel.count && blockModel.laneForRow(row + 1) === lane
                      && blockModel.typeForRow(row + 1) !== 10) ? row + 1 : -1
        if (next < 0) {
            if (lane >= 0) {                      // the last block of a lane
                if (repeat) return
                const land = blockModel.collapseEmptyLane(row, true)
                if (land.length === 2) { cursor.setCaret(land[0], land[1]); root.ensureVisible(land[0]) }
                return
            }
            const nx = blockModel.nextLeaf(row)   // a split row below (or the document's end)
            if (nx < 0) return
            let land = nx
            if (!repeat && blockModel.typeForRow(row) === 0 && blockModel.contentForRow(row).length === 0
                && blockModel.count > 1) {
                blockModel.removeBlock(row)
                land = nx - 1
            }
            cursor.setCaret(land, 0)
            root.ensureVisible(land)
            return
        }
        const nt = blockModel.typeForRow(next)
        if (nt === 3 || nt === 6) { cursor.setCaret(next, 0); root.ensureVisible(next); return } // select first
        const col = cursor.focusCol
        blockModel.deleteRange(row, blockModel.contentForRow(row).length, next, 0)   // pull the next block up
        cursor.setCaret(row, col)
    }
    // SR-0 §4.9: a selection from row lo to row hi whose ends sit in different top-level rows (and not in
    // one table) is a row range — an end inside a split row widens to the whole split row, an end inside a
    // table to the whole table; an end in a top-level block stays where it is. → [lo, hi], or null when
    // nothing widens (one block, one cell, one split row, one table: the finer grains).
    function rowRangeSpan(lo, hi) {
        if (lo < 0 || hi < 0 || lo >= hi || hi >= blockModel.count) return null
        const sl = blockModel.splitRowOf(lo), sh = blockModel.splitRowOf(hi)
        const tl = sl >= 0 ? sl : lo, th = sh >= 0 ? sh : hi
        if (tl === th) return null
        const hl = blockModel.tableHeadOf(tl), hh = blockModel.tableHeadOf(th)
        if (hl >= 0 && hl === hh) return null
        let a = lo, b = hi
        if (sl >= 0 || blockModel.typeForRow(lo) === 10) a = hl >= 0 ? hl : tl
        if (sh >= 0 || blockModel.typeForRow(hi) === 10) {
            const recs = hh >= 0 ? blockModel.tableRecords(hh) : [th]
            b = blockModel.splitRowLast(recs[recs.length - 1])
        }
        return a === lo && b === hi ? null : [a, b]
    }
    // The caret work of a press, for a press on the pull strip that turned out not to be a pull.
    function edgePressCaret(px, py, mods, drag) {
        const h = hitTest(px, py)
        if (mods & Qt.ShiftModifier) cursor.move(h.row, h.col, true)
        else {
            if (h.row !== cursor.focusRow) blockModel.commitMarkdown(cursor.focusRow)
            cursor.setCaret(h.row, h.col)
        }
        if (drag) { dragging = true; dragX = px; dragViewY = py - flick.contentY }
    }
    // --- Lane gestures (SR-3 S7b) ---
    // The lane gap under a page-relative x in the split row holding `row`: {record, index}, or null.
    function dividerAt(row, pageX) {
        const rec = blockModel.splitRowOf(row)
        if (rec < 0) return null
        const head = blockModel.tableHeadOf(rec)
        if (head >= 0) {   // SR-4 A6: a table column's right border (the last column's too) → its px width
            pageX -= tableShift(head)
            const cols = blockModel.tableColumnCount(head)
            for (let k = 0; k < cols; ++k)
                if (Math.abs(pageX - blockModel.tableColumnLeft(head, k) - blockModel.tableColumnWidth(head, k)) <= 4)
                    return { record: rec, index: k }
            return null
        }
        const n = blockModel.laneCount(rec)
        for (let k = 0; k + 1 < n; ++k)
            if (Math.abs(pageX - blockModel.dividerX(rec, k)) <= blockModel.laneGap / 2) return { record: rec, index: k }
        return null
    }
    // 1 = the hot band inside the block's left column edge, 0 = its right edge, -1 = neither.
    // Hidden while a draw tool is armed or a frame tab is open; tables and records don't pull.
    function pullSideAt(row, cx) {
        if (row < 0 || root.inkMode || root.activeFrameId !== "") return -1
        const t = blockModel.typeForRow(row)
        if (t === 10) return -1
        const x0 = columnX(row), w = laneOf(row).w
        if (cx >= x0 && cx < x0 + laneHotBand) return 1
        if (cx > x0 + w - laneHotBand && cx <= x0 + w) return 0
        return -1
    }
    // Soft snaps at ¼ ⅓ ½ ⅔ ¾ of [left, left + width].
    function snapToFractions(x, left, width) {
        const fr = [0.25, 1 / 3, 0.5, 2 / 3, 0.75]
        for (let i = 0; i < fr.length; ++i)
            if (Math.abs(x - (left + width * fr[i])) <= 8) return left + width * fr[i]
        return x
    }
    function beginDividerDrag(rec, idx, pageX, alone) {
        dividerDragRecord = rec; dividerDragIndex = idx; dividerDragAlone = alone
        const head = blockModel.tableHeadOf(rec)
        if (head >= 0) {   // a table column border: the preview spans the table
            const recs = blockModel.tableRecords(head)
            dividerDragChain = [recs[0], idx, recs[recs.length - 1], idx]
            dividerPreviewX = blockModel.tableColumnLeft(head, idx) + blockModel.tableColumnWidth(head, idx)
        } else {
            dividerDragChain = alone ? [rec, idx] : blockModel.dividerChain(rec, idx)
            dividerPreviewX = blockModel.dividerX(rec, idx)
        }
        dividerDragging = true
    }
    function updateDividerDrag(pageX) {
        const head = blockModel.tableHeadOf(dividerDragRecord)
        if (head >= 0) {   // px, no fraction snaps; a column never goes under 48 (preview = table-space x)
            dividerPreviewX = Math.max(blockModel.tableColumnLeft(head, dividerDragIndex) + 48, pageX - tableShift(head))
            return
        }
        dividerPreviewX = snapToFractions(pageX, 0, pageWidth)
    }
    function commitDividerDrag() {
        if (!dividerDragging) return
        dividerDragging = false
        const head = blockModel.tableHeadOf(dividerDragRecord)
        if (head >= 0)
            blockModel.setTableColumnWidth(head, dividerDragIndex,
                                           dividerPreviewX - blockModel.tableColumnLeft(head, dividerDragIndex))
        else
            blockModel.moveDivider(dividerDragRecord, dividerDragIndex, dividerPreviewX, dividerDragAlone)
        dividerDragRecord = -1; dividerDragIndex = -1; dividerDragChain = []
    }
    function cancelDividerDrag() { dividerDragging = false; dividerDragRecord = -1; dividerDragIndex = -1; dividerDragChain = [] }
    function beginPull(row, side, pageX) {
        // With a top-level run selected, pulling from one of its blocks wraps the whole run.
        const inRun = cursor.hasSel && cursor.loRow !== cursor.hiRow && row >= cursor.loRow && row <= cursor.hiRow
                      && blockModel.laneForRow(cursor.loRow) < 0 && blockModel.laneForRow(cursor.hiRow) < 0
        pullRow = row; pullSide = side
        pullLo = inRun ? cursor.loRow : row; pullHi = inRun ? cursor.hiRow : row
        pullPressX = pageX; pullPreviewX = pageX
        pulling = true
    }
    function updatePull(pageX) {
        const g = laneOf(pullRow), min = blockModel.minLaneWidth
        if (blockModel.tableHeadOf(pullRow) >= 0) { pullPreviewX = Math.max(g.x, Math.min(g.x + g.w, pageX)); return }
        pullPreviewX = snapToFractions(Math.max(g.x + min, Math.min(g.x + g.w - min, pageX)), g.x, g.w)
    }
    function commitPull() {
        if (!pulling) return
        pulling = false
        if (Math.abs(pullPreviewX - pullPressX) < 12) return        // a click on the band, not a pull
        const head = blockModel.tableHeadOf(pullRow)
        if (head >= 0) {   // SR-4 A6: in a table, a pull adds a column table-wide beside this cell
            const r = blockModel.tableRowOf(pullRow), c = blockModel.tableColumnOf(pullRow)
            const at = pullSide === 0 ? c + 1 : c
            if (blockModel.tableInsertColumn(head, at)) root.landInCell(head, r, at)
            return
        }
        const g = laneOf(pullRow)
        const leftShare = (pullPreviewX - g.x) / g.w
        const hadInk = blockModel.inkForRow(pullRow).length > 0
        // The pulled block keeps the side away from the edge it was pulled from.
        const fresh = blockModel.wrapRun(pullLo, pullHi, pullSide, pullSide === 0 ? leftShare : 1 - leftShare)
        if (fresh < 0) return
        cursor.setCaret(fresh, 0)
        root.ensureVisible(fresh)
        if (hadInk) Toasts.show(qsTr("This row's ink stays where it was drawn"))
    }
    function cancelPull() { pulling = false }
    // --- Table grips and add strips (SR-4 S7b) ---
    // The grip band under content point (cx, cy): {head, kind, index}, or null.
    function tableGripAt(cx, cy) {
        if (root.inkMode || root.activeFrameId !== "") return null
        const rec = blockModel.rowForY(Math.max(0, cy))
        if (rec < 0 || blockModel.typeForRow(rec) !== 10) return null
        const head = blockModel.tableHeadOf(rec)
        if (head < 0) return null
        const pageX = cx - leftEdge
        const tx = pageX - tableShift(head)                      // against the table's own left edge
        const top = blockModel.yForRow(rec) + blockModel.tablePadTop(rec)
        const bottom = blockModel.yForRow(rec) + blockModel.heightForRow(rec) - blockModel.tablePadBottom(rec)
        if (tx >= -18 && tx < -2 && cy >= top && cy < bottom)
            return { head: head, kind: "row", index: blockModel.tableRowOf(rec) }
        if (rec === head && cy >= top - 18 && cy < top - 2) {
            const c = tableColumnAtX(head, pageX)
            if (c >= 0) return { head: head, kind: "col", index: c }
        }
        return null
    }
    function tableColumnAtX(head, pageX) {
        pageX -= tableShift(head)
        const cols = blockModel.tableColumnCount(head)
        for (let k = 0; k < cols; ++k) {
            const l = blockModel.tableColumnLeft(head, k)
            if (pageX >= l && pageX < l + blockModel.tableColumnWidth(head, k)) return k
        }
        return -1
    }
    // A column grip drag's gap (0..columns) for a page x: before or after a column by its midpoint.
    function tableColGapAt(head, pageX) {
        pageX -= tableShift(head)
        const cols = blockModel.tableColumnCount(head)
        for (let k = 0; k < cols; ++k)
            if (pageX < blockModel.tableColumnLeft(head, k) + blockModel.tableColumnWidth(head, k) / 2) return k
        return cols
    }
    // The run a row handle (a rail number or a row grip) drags, as [from, count] (§4.12): a header
    // row carries its whole table; a body row inside a contiguous grip-picked body-row set carries
    // the set; any other split row carries itself.
    function dragRunFor(rec) {
        const head = blockModel.tableHeadOf(rec)
        if (head >= 0) {
            const recs = blockModel.tableRecords(head)
            if (blockModel.isHeaderRow(rec)) return [head, blockModel.splitRowLast(recs[recs.length - 1]) - head + 1]
            const s = tableSetLive() ? tableSet : null, r = blockModel.tableRowOf(rec)
            if (s && s.head === head && s.kind === "row" && s.items.indexOf(r) >= 0
                && s.items[0] >= blockModel.headerCount(head)
                && s.items[s.items.length - 1] - s.items[0] === s.items.length - 1) {
                const a = recs[s.items[0]]
                return [a, blockModel.splitRowLast(recs[s.items[s.items.length - 1]]) - a + 1]
            }
        }
        return [rec, blockModel.splitRowLast(rec) - rec + 1]
    }
    function tableSetLive() {
        return tableSet !== null && tableSet.rev === blockModel.contentRevision && blockModel.headerCount(tableSet.head) > 0
    }
    // A grip click (the v0.4.1 set gestures): plain → that row / column alone; Shift → the span from
    // the set's last pick; ⌘ → toggle. Sets are homogeneous — a pick of the other kind starts over.
    // The caret parks in the pick (a row's first cell; a column's first body cell).
    function tableGripClick(head, kind, index, mods) {
        const s = tableSetLive() && tableSet.head === head && tableSet.kind === kind ? tableSet : null
        let items = [index]
        if (s && (mods & Qt.ControlModifier))
            items = s.items.indexOf(index) >= 0 ? s.items.filter(function(i) { return i !== index }) : s.items.concat([index])
        else if (s && (mods & Qt.ShiftModifier)) {
            items = []
            for (let i = Math.min(s.last, index); i <= Math.max(s.last, index); ++i) items.push(i)
        }
        items.sort(function(a, b) { return a - b })
        const rows = blockModel.tableRowCount(head)
        if (kind === "row") root.landInCell(head, index, 0)
        else root.landInCell(head, Math.min(blockModel.headerCount(head), rows - 1), index)
        tableSet = items.length ? { head: head, kind: kind, items: items, last: index, rev: blockModel.contentRevision } : null
    }
    // A set op from the menu or a key: `fn(head, items, kind)`, then the caret back into the table
    // (or onto the nearest block when the table is gone).
    function tableSetMenuOp(fn) {
        const s = tableSetLive() ? tableSet : null
        tableSet = null
        if (!s) return
        fn(s.head, s.items, s.kind)
        const h = s.head
        if (blockModel.headerCount(h) > 0 && blockModel.tableHeadOf(h) === h) {
            const rows = blockModel.tableRowCount(h), cols = blockModel.tableColumnCount(h)
            if (s.kind === "row") root.landInCell(h, Math.min(s.items[0], rows - 1), 0)
            else root.landInCell(h, Math.min(blockModel.headerCount(h), rows - 1), Math.min(s.items[0], cols - 1))
            return
        }
        let l = Math.min(h, blockModel.count - 1)
        if (l >= 0 && blockModel.typeForRow(l) === 10) l = blockModel.nextLeaf(l - 1)
        cursor.setCaret(Math.max(0, l), 0)
        root.ensureVisible(Math.max(0, l))
    }
    // Delete / Backspace on a set clears its cells (one undo); deleting rows or columns is the menu's.
    function clearGridSet() {
        tableSetMenuOp(function(h, items, kind) {
            if (kind === "row") blockModel.tableClearRows(h, items)
            else blockModel.tableClearColumns(h, items)
        })
    }
    function commitGridColDrag() {
        const head = tableGripPressHead, from = tableGripPressIndex, gap = tableColGap
        tableColDragging = false; tableColGap = -1
        if (head < 0 || from < 0 || gap < 0 || gap === from || gap === from + 1) return
        const to = gap > from ? gap - 1 : gap
        const r = blockModel.tableHeadOf(cursor.focusRow) === head ? Math.max(0, blockModel.tableRowOf(cursor.focusRow))
                : Math.min(blockModel.headerCount(head), blockModel.tableRowCount(head) - 1)
        if (!blockModel.tableMoveColumn(head, from, to)) return
        root.landInCell(head, r, to)
        tableSet = { head: head, kind: "col", items: [to], last: to, rev: blockModel.contentRevision }   // the moved column stays picked
    }
    function tableAddRow(head) {
        const rows = blockModel.tableRowCount(head)
        const c = blockModel.tableHeadOf(cursor.focusRow) === head ? Math.max(0, blockModel.tableColumnOf(cursor.focusRow)) : 0
        if (blockModel.tableInsertRow(head, rows)) root.landInCell(head, rows, c)
    }
    function tableAddColumn(head) {
        const cols = blockModel.tableColumnCount(head)
        const r = blockModel.tableHeadOf(cursor.focusRow) === head ? Math.max(0, blockModel.tableRowOf(cursor.focusRow)) : 0
        if (blockModel.tableInsertColumn(head, cols)) root.landInCell(head, r, cols)
    }
    Connections {   // a grip-picked set lasts until the caret moves
        target: cursor
        function onFocusRowChanged() { root.tableSet = null }
        function onFocusColChanged() { root.tableSet = null }
        function onAnchorRowChanged() { root.tableSet = null }
        function onAnchorColChanged() { root.tableSet = null }
    }
    // Block menu: split the block (or the selected run) into two lanes.
    function splitMenu(lo, hi) {
        const fresh = blockModel.wrapRun(lo, hi, 0, 0.5)
        if (fresh >= 0) { cursor.setCaret(fresh, 0); root.ensureVisible(fresh) }
    }
    // The split row right below `record` when it has the same number of lanes, else -1.
    function mergeTargetBelow(record) {
        const next = blockModel.splitRowLast(record) + 1
        return (next < blockModel.count && blockModel.typeForRow(next) === 10
                && blockModel.laneCount(next) === blockModel.laneCount(record)) ? next : -1
    }
    // Delete lane (SR-0 §4.13): remove the lane's blocks — A4 collapses the lane or unwraps the row.
    function deleteLane(row) {
        const lane = blockModel.laneForRow(row), rec = blockModel.splitRowOf(row)
        if (lane < 0 || rec < 0) return
        let first = -1, last = -1
        for (let i = rec + 1; i < blockModel.count && blockModel.laneForRow(i) >= 0; ++i)
            if (blockModel.laneForRow(i) === lane) { if (first < 0) first = i; last = i }
        if (first < 0) return
        blockModel.removeBlocks(first, last)
        const land = Math.min(first, blockModel.count - 1)
        cursor.setCaret(blockModel.typeForRow(land) === 10 ? blockModel.nextLeaf(land) : land, 0)
    }
    // --- Table keys (SR-4 S6a) ---
    // The caret at the end of a table cell's last block (a ragged row: its last cell).
    function landInCell(head, r, c) {
        const cells = blockModel.tableCellCount(head, r)
        if (cells <= 0) return
        const blocks = blockModel.tableCellRows(head, r, Math.max(0, Math.min(c, cells - 1)))
        if (blocks.length === 0) return
        const b = blocks[blocks.length - 1]
        const t = blockModel.typeForRow(b)
        cursor.resetGoalX(); cursor.clearMarks()
        cursor.move(b, (t === 3 || t === 6) ? 0 : blockModel.contentForRow(b).length, false)
        root.ensureVisible(b)
    }
    // Enter in a table row (SR-0 A1): down the column; the last row appends a row, or — an empty
    // last body row — exits the table as a paragraph. Header rows never exit. `repeat` never
    // appends or exits (P6).
    function tableEnter(repeat) {
        const row = cursor.focusRow, head = blockModel.tableHeadOf(row)
        if (head < 0) return
        const r = blockModel.tableRowOf(row), c = blockModel.tableColumnOf(row)
        const rows = blockModel.tableRowCount(head)
        if (r < rows - 1) { landInCell(head, r + 1, c); return }
        if (repeat) return
        if (r >= blockModel.headerCount(head) && blockModel.tableRowIsEmpty(head, r)) {
            const p = blockModel.tableExitRow(head)
            if (p >= 0) { cursor.setCaret(p, 0); root.ensureVisible(p) }
            return
        }
        if (blockModel.tableInsertRow(head, rows)) landInCell(head, rows, c)
    }
    // A7 (SR-4 S6c): a selection whose ends sit in different cells of one table is a cell
    // rectangle {head, r0, c0, r1, c1}; null otherwise (inside one cell it's blocks/characters).
    readonly property var cellRect: {
        const dep = blockModel.contentRevision
        if (!cursor.hasSel) return null
        const ha = blockModel.tableHeadOf(cursor.anchorRow), hf = blockModel.tableHeadOf(cursor.focusRow)
        if (ha < 0 || ha !== hf) return null
        const ca = blockModel.tableColumnOf(cursor.anchorRow), cf = blockModel.tableColumnOf(cursor.focusRow)
        if (ca < 0 || cf < 0) return null
        const ra = blockModel.tableRowOf(cursor.anchorRow), rf = blockModel.tableRowOf(cursor.focusRow)
        if (ra === rf && ca === cf) return null
        return { head: ha, r0: Math.min(ra, rf), c0: Math.min(ca, cf), r1: Math.max(ra, rf), c1: Math.max(ca, cf) }
    }
    // What an Escape rung selected as an OBJECT (SR-0 §4.8/§4.10): a table row or a whole table —
    // {kind, head, r, lo, hi}. Deleting (or typing over) it removes it; the same range reached by
    // ⌘A or dragging is cells, and clears. Valid only while the selection is still that range.
    property var selObject: null
    function selObjectValid() {
        return selObject !== null && cursor.hasSel && cursor.loRow === selObject.lo && cursor.loCol === 0
            && cursor.hiRow === selObject.hi
    }
    function deleteSelectedObject() {
        const o = selObject
        selObject = null
        if (o.kind === "table") blockModel.deleteTable(o.head)
        else blockModel.tableDeleteRow(o.head, o.r)
        let land = Math.min(o.head, blockModel.count - 1)
        if (blockModel.tableHeadOf(land) >= 0 && blockModel.headerCount(land) > 0 && o.kind === "row") {
            const b = blockModel.tableCellAt(land, Math.min(o.r, blockModel.tableRowCount(land) - 1), 0)
            if (b >= 0) land = b
        }
        if (land >= 0 && blockModel.typeForRow(land) === 10) land = blockModel.nextLeaf(land - 1)
        cursor.setCaret(Math.max(0, land), 0)
        root.ensureVisible(Math.max(0, land))
    }
    // Block menu → a derived-table op on the right-clicked cell (SR-4 S7a). `op(head, r, c)` returns
    // the [r, c] to land the caret in, or null; when the table is gone the caret takes the nearest block.
    function tableMenuOp(op) {
        const h = blockModel.tableHeadOf(root.menuRow)
        if (h < 0) return
        const land = op(h, blockModel.tableRowOf(root.menuRow), blockModel.tableColumnOf(root.menuRow))
        if (blockModel.headerCount(h) > 0 && blockModel.tableHeadOf(h) === h) {
            if (land) root.landInCell(h, Math.min(land[0], blockModel.tableRowCount(h) - 1), Math.max(0, land[1]))
            return
        }
        let l = Math.min(h, blockModel.count - 1)
        if (l >= 0 && blockModel.typeForRow(l) === 10) l = blockModel.nextLeaf(l - 1)
        cursor.setCaret(Math.max(0, l), 0)
        root.ensureVisible(Math.max(0, l))
    }
    // ⌘Enter in a table row: a new row below, the caret in the same column.
    function tableInsertRowBelow() {
        const row = cursor.focusRow, head = blockModel.tableHeadOf(row)
        if (head < 0) return
        const r = blockModel.tableRowOf(row), c = blockModel.tableColumnOf(row)
        if (blockModel.tableInsertRow(head, r + 1)) landInCell(head, r + 1, c)
    }
    // The caret's typed-cell kind: 1 choice, 2 check, 0 otherwise (header rows are text).
    function typedCellHere() {
        const row = cursor.focusRow, head = blockModel.tableHeadOf(row)
        if (head < 0 || blockModel.isHeaderRow(row)) return 0
        const c = blockModel.tableColumnOf(row)
        return c < 0 ? 0 : blockModel.tableColumnKind(head, c)
    }
    // Whether the selection is exactly one whole table (Escape's rung 3 result).
    function selectionIsTable() {
        const head = blockModel.tableHeadOf(cursor.loRow)
        if (head < 0 || blockModel.tableHeadOf(cursor.hiRow) !== head) return false
        const recs = blockModel.tableRecords(head)
        const last = blockModel.splitRowLast(recs[recs.length - 1])
        return cursor.loRow === blockModel.nextLeaf(head) && cursor.loCol === 0 && cursor.hiRow === last
            && cursor.hiCol >= blockModel.contentForRow(last).length
    }
    // Whether the selection is exactly one whole split row (Escape's rung 2 result).
    function selectionIsSplitRow() {
        const rec = blockModel.splitRowOf(cursor.loRow)
        return rec >= 0 && blockModel.splitRowOf(cursor.hiRow) === rec
            && cursor.loRow === blockModel.nextLeaf(rec) && cursor.loCol === 0
            && cursor.hiRow === blockModel.splitRowLast(rec)
            && cursor.hiCol >= blockModel.contentForRow(cursor.hiRow).length
    }
    // Tab / Shift+Tab. In a lane (SR-0 §4.7): the next/previous lane, landing at the end
    // of its last block with no selection, then on in reading order. At top level: list
    // indent/outdent of the focused item, or of every list item in the selection.
    function tabKey(back, repeat) {
        if (blockModel.laneForRow(cursor.focusRow) >= 0) {
            cursor.resetGoalX(); cursor.clearMarks()
            const t = blockModel.tabTarget(cursor.focusRow, back)
            if (t === -2) {                          // BlockModel::kTabAppendsRow — SR-4 A3: Tab past a table's last cell appends a row
                if (repeat === true) return            // P6: never structural on auto-repeat
                const head = blockModel.tableHeadOf(cursor.focusRow), rows = blockModel.tableRowCount(head)
                if (blockModel.tableInsertRow(head, rows)) root.landInCell(head, rows, 0)
                return
            }
            if (t < 0) return
            const tt = blockModel.typeForRow(t)
            cursor.move(t, (tt === 3 || tt === 6) ? 0 : blockModel.contentForRow(t).length, false)
            return
        }
        blockModel.indentBlocks(cursor.hasSel ? cursor.loRow : cursor.focusRow,
                                cursor.hasSel ? cursor.hiRow : cursor.focusRow,
                                back ? -1 : 1)
    }
    // Map the sticky goal-x onto a visual line of `row`'s block (te-local y),
    // returning the column there. Falls back to col 0 if that block has no live
    // delegate (off-screen) or is media. Used when up/down crosses a boundary.
    function colAtGoalX(row, yLocal) {
        var cell = cellForRow(row)
        if (!cell || cell.isMedia) return 0
        // goalX is page-relative; the target's text starts at its own lane + decoration.
        return cell.teItem.positionAt(cursor.goalX - (cell.teItem.x - root.leftEdge), yLocal)
    }
    function navDown(shift) {
        cursor.clearMarks()
        var fb = root.focusBlockItem
        if (!fb) return
        var r = fb.positionToRectangle(Math.min(cursor.focusCol, fb.length))
        var lh = r.height > 0 ? r.height : 18
        // Goal-x is PAGE-relative (SR-0 §1): the text's left edge (fb.x already carries
        // the lane) plus the caret's x — so a vertical run keeps its column across lanes.
        const textLeft = fb.x - root.leftEdge
        if (cursor.goalX < 0) cursor.goalX = textLeft + r.x   // capture at the start of a vertical run
        if (r.y < fb.contentHeight - lh * 1.5)                 // another visual line below in this block
            cursor.move(cursor.focusRow, fb.positionAt(cursor.goalX - textLeft, r.y + lh * 1.5), shift)
        else {                                                  // the block below: in the lane, else the row below at goal-x
            const below = blockModel.leafBelow(cursor.focusRow, cursor.goalX)
            if (below < 0) { root.leaveSplitRowAtEdge(true, shift); return }
            cursor.move(below, colAtGoalX(below, 2), shift)
        }
    }
    function navUp(shift) {
        cursor.clearMarks()
        var fb = root.focusBlockItem
        if (!fb) return
        var r = fb.positionToRectangle(Math.min(cursor.focusCol, fb.length))
        var lh = r.height > 0 ? r.height : 18
        const textLeft = fb.x - root.leftEdge                  // page-relative goal-x (see navDown)
        if (cursor.goalX < 0) cursor.goalX = textLeft + r.x
        if (r.y > lh * 0.5)                                    // another visual line above in this block
            cursor.move(cursor.focusRow, fb.positionAt(cursor.goalX - textLeft, r.y - lh * 0.5), shift)
        else {                                                  // the block above: in the lane, else the row above at goal-x
            const above = blockModel.leafAbove(cursor.focusRow, cursor.goalX)
            if (above < 0) { root.leaveSplitRowAtEdge(false, shift); return }
            var prev = cellForRow(above)
            var yLast = (prev && !prev.isMedia) ? prev.teItem.contentHeight - 2 : 0
            cursor.move(above, colAtGoalX(above, yLast), shift)
        }
    }

    // Nearest text-editable row to `row` (skipping opaque blocks — media 3,
    // divider 6, table 7 — which the document caret can't sit in), scanning
    // `dir` first, then the other way. Keeps a coarse jump from stranding the
    // caret on a block it can't move off of.
    function caretLandRow(row, dir) {
        var n = blockModel.count
        if (n === 0) return 0
        function ok(r) { var t = blockModel.typeForRow(r); return t !== 3 && t !== 6 && t !== 10 }   // never a record
        var r = Math.max(0, Math.min(n - 1, row))
        var s = r
        while (s >= 0 && s < n) { if (ok(s)) return s; s += dir }
        s = r - dir
        while (s >= 0 && s < n) { if (ok(s)) return s; s -= dir }
        return r
    }
    // Home / End: to the beginning / end of the WHOLE document.
    // ⌘A (2026-09-09): the whole document as one range — anchor at the top,
    // focus at the end of the last row (col 0 on an opaque row). No scroll.
    // ⌘A (SR-0 A8): in a lane the block → its cell → the split row (a table: the whole table) →
    // the document, one rung per press. At top level straight to the document (= today).
    function selectAllLadder() {
        const row = cursor.focusRow
        const lane = blockModel.laneForRow(row)
        if (lane < 0) { selectAllDocument(); return }
        const rec = blockModel.splitRowOf(row), head = blockModel.tableHeadOf(row)
        let first = row, last = row
        while (first - 1 > rec && blockModel.laneForRow(first - 1) === lane) --first
        while (last + 1 < blockModel.count && blockModel.laneForRow(last + 1) === lane
               && blockModel.splitRowOf(last + 1) === rec) ++last
        const rungs = [[row, row], [first, last]]
        if (head >= 0) {
            const recs = blockModel.tableRecords(head)
            rungs.push([blockModel.nextLeaf(head), blockModel.splitRowLast(recs[recs.length - 1])])
        } else {
            rungs.push([blockModel.nextLeaf(rec), blockModel.splitRowLast(rec)])
        }
        for (let i = 0; i < rungs.length; ++i) {
            const a = rungs[i][0], b = rungs[i][1]
            const endCol = cursor.opaque(b) ? 0 : blockModel.contentForRow(b).length
            const whole = cursor.loRow === a && cursor.loCol === 0 && cursor.hiRow === b && cursor.hiCol >= endCol
                          && (cursor.hasSel || (a === b && endCol === 0))
            if (cursor.loRow >= a && cursor.hiRow <= b && !whole) {
                cursor.clearMarks()
                cursor.anchorRow = a; cursor.anchorCol = 0
                cursor.focusRow = b; cursor.focusCol = endCol
                cursor.goalX = -1
                cursor.sync()
                return
            }
        }
        selectAllDocument()
    }
    function selectAllDocument() {
        var n = blockModel.count
        if (n === 0) return
        cursor.clearMarks()
        cursor.anchorRow = 0; cursor.anchorCol = 0
        cursor.focusRow = n - 1
        cursor.focusCol = cursor.opaque(n - 1) ? 0 : blockModel.contentForRow(n - 1).length
        cursor.goalX = -1
        cursor.sync()
    }
    function navHome(shift) {
        cursor.resetGoalX(); cursor.clearMarks()
        var r = caretLandRow(0, 1)
        cursor.move(r, 0, shift); root.ensureVisible(r)
    }
    function navEnd(shift) {
        cursor.resetGoalX(); cursor.clearMarks()
        var r = caretLandRow(blockModel.count - 1, -1)
        cursor.move(r, blockModel.contentForRow(r).length, shift); root.ensureVisible(r)
    }
    // Page Up / Down: move the caret one full viewport up/down (landing on a text
    // block, never an opaque one) and scroll there.
    function navPageDown(shift) {
        cursor.clearMarks()
        var t = blockModel.rowForY(blockModel.yForRow(cursor.focusRow) + flick.height)
        if (t <= cursor.focusRow) t = cursor.focusRow + 1            // ensure progress past tall blocks
        var row = caretLandRow(Math.min(blockModel.count - 1, t), 1)
        cursor.move(row, 0, shift); root.ensureVisible(row)
    }
    function navPageUp(shift) {
        cursor.clearMarks()
        var t = blockModel.rowForY(Math.max(0, blockModel.yForRow(cursor.focusRow) - flick.height))
        if (t >= cursor.focusRow) t = cursor.focusRow - 1
        var row = caretLandRow(Math.max(0, t), -1)
        cursor.move(row, 0, shift); root.ensureVisible(row)
    }

    // Per-row selected range [start,end) for row r within the current selection.
    function rowSelStart(r) { return (r === cursor.loRow) ? cursor.loCol : 0 }
    function rowSelEnd(r)   { return (r === cursor.hiRow) ? cursor.hiCol : blockModel.contentForRow(r).length }

    // Apply a semantic format span over the current selection (menu/shortcut
    // path — NOT markdown; renders clean with no markers). Decides add-vs-remove
    // UNIFORMLY across the whole selection (all-covered → remove, else add), as
    // one grouped undo step.
    // Armed-mark state, for the rail's lit toggle when nothing is selected.
    readonly property bool boldArmed:      (cursor.activeMarks & 1) !== 0
    readonly property bool italicArmed:    (cursor.activeMarks & 2) !== 0
    readonly property bool codeArmed:      (cursor.activeMarks & 4) !== 0
    readonly property bool strikeArmed:    (cursor.activeMarks & 8) !== 0
    readonly property bool underlineArmed: (cursor.activeMarks & 16) !== 0
    // Lit when the caret/selection sits inside a link (rail toggle + edit mode).
    readonly property bool linkActive: (blockModel.contentRevision, cursor.active
                                        && blockModel.linkAt(cursor.focusRow, cursor.focusCol) !== "")

    function applyFormat(kind) {
        // No selection → Word-style toggle: arm the attribute for the next typing.
        if (!cursor.hasSel) { cursor.toggleMark(kind); return }
        var allCovered = true
        for (var r = cursor.loRow; r <= cursor.hiRow; ++r)
            if (!blockModel.hasFormat(r, rowSelStart(r), rowSelEnd(r), kind)) { allCovered = false; break }
        blockModel.beginGroup(cursor.loRow, cursor.hiRow)
        for (r = cursor.loRow; r <= cursor.hiRow; ++r)
            blockModel.setFormat(r, rowSelStart(r), rowSelEnd(r), kind, !allCovered)
        blockModel.endGroup()
        cursor.sync()
    }
    // Palette "Revert to default": strip ONE colour kind from the selection — the
    // palette tab's (text colour or highlight/cell background), never both. Table
    // sets / rects / a caret cell go through the cell-colour setters ("" = uncoloured;
    // the text-colour pass also strips text-colour spans inside the cells, the
    // Excel rule); a text selection strips that kind's spans. Also unarms the
    // matching pen so typing after a revert is plain. One undo entry either way.
    function revertColors(isFg) {
        if (isFg) cursor.armedFg = ""
        else      cursor.armedBg = ""
        if (root.tableSetLive() || root.cellRect || (!cursor.hasSel && root.caretInCell)) {
            applyTableColor(isFg, "")
            cursor.sync()
            return
        }
        if (!cursor.hasSel) return
        applyColorToSelection(isFg, "", false)
    }

    // Colour is palette-driven: picking a colour applies it LIVE to the current
    // selection (text colour or highlight per the palette tab). No selection →
    // nothing (the palette just holds the persistent colour). `coalesce` merges a
    // picker drag into one undo step.
    function applyTextColor(color) { applyColorToSelection(true,  "" + color, false) }
    function applyHighlight(color)  { applyColorToSelection(false, "" + color, false) }
    // Picking a TEXT colour in the palette: arm it as the pen (so the next typing
    // is that colour), apply it to any active selection, and pull focus back to
    // the document so typing continues immediately without re-clicking.
    function pickTextColor(hex) {
        cursor.armedFg = "" + hex
        if (cursor.hasSel || root.caretInCell) applyColorToSelection(true, "" + hex, true)
        forceActiveFocus()
    }
    // Highlight mirrors the text pen, plus a rail toggle. pickHighlight arms +
    // applies (palette Highlight tab). toggleHighlight (the rail button) flips it:
    // on → arm + highlight the selection; off → unarm + clear it from the selection.
    readonly property bool highlightArmed: cursor.armedBg !== ""
    function pickHighlight(hex) {
        cursor.armedBg = "" + hex
        if (cursor.hasSel || root.caretInCell) applyColorToSelection(false, "" + hex, true)
        forceActiveFocus()
    }
    function toggleHighlight(hex) {
        if (cursor.armedBg !== "") {                       // currently on → off
            if (cursor.hasSel || root.caretInCell) applyColorToSelection(false, "", false)   // "" removes it
            cursor.armedBg = ""
            forceActiveFocus()
        } else {
            pickHighlight("" + hex)
        }
    }
    // The caret sits in a table cell (a cell block, not a record): the palette's target when
    // nothing is selected — the cell itself, not a span (the 2026-09-15 walk).
    readonly property bool caretInCell: (blockModel.contentRevision, cursor.focusRow >= 0
        && blockModel.tableHeadOf(cursor.focusRow) >= 0 && blockModel.tableColumnOf(cursor.focusRow) >= 0)
    // Table targets (2026-09-15 walk: the Back tab was painting spans inside cells): a grip-picked
    // set colours its rows / columns as a unit, a cell rectangle its cells, a lone caret its cell;
    // text selected INSIDE one cell is still a span. Returns true when a table took the colour.
    function applyTableColor(isFg, hex) {
        const set = root.tableSetLive() ? root.tableSet : null
        if (set && set.head >= 0 && set.items.length) {
            if (set.kind === "row") blockModel.tableSetRowsColor(set.head, set.items, isFg, hex)
            else                    blockModel.tableSetColsColor(set.head, set.items, isFg, hex)
            return true
        }
        const rect = root.cellRect
        if (rect) { blockModel.tableSetCellColor(rect.head, rect.r0, rect.c0, rect.r1, rect.c1, isFg, hex); return true }
        if (!cursor.hasSel && root.caretInCell) {
            const row = cursor.focusRow, head = blockModel.tableHeadOf(row)
            const r = blockModel.tableRowOf(row), c = blockModel.tableColumnOf(row)
            blockModel.tableSetCellColor(head, r, c, r, c, isFg, hex)
            return true
        }
        return false
    }
    function applyColorToSelection(isFg, hex, coalesce) {
        if (applyTableColor(isFg, hex)) return
        if (!cursor.hasSel) return
        var key = coalesce ? (isFg ? "fgcolor" : "bgcolor") : ""
        if (cursor.loRow === cursor.hiRow) {
            var s = rowSelStart(cursor.loRow), e = rowSelEnd(cursor.loRow)
            if (isFg) blockModel.setTextColor(cursor.loRow, s, e, hex, key)
            else      blockModel.setHighlight(cursor.loRow, s, e, hex, key)
        } else {
            blockModel.beginGroup(cursor.loRow, cursor.hiRow)
            for (var r = cursor.loRow; r <= cursor.hiRow; ++r) {
                if (isFg) blockModel.setTextColor(r, rowSelStart(r), rowSelEnd(r), hex)
                else      blockModel.setHighlight(r, rowSelStart(r), rowSelEnd(r), hex)
            }
            blockModel.endGroup()
        }
        cursor.sync()
    }
    // Paragraph button: reset the selected block(s) to a plain paragraph (clears
    // heading / quote / list / task / code-block). Lit when already a paragraph —
    // the "nothing else" state.
    function setParagraph() {
        var lo = cursor.loRow, hi = cursor.hiRow
        blockModel.beginGroup(lo, hi)
        for (var r = lo; r <= hi; ++r) {
            var t = blockModel.typeForRow(r)
            if (t === 1 || t === 2 || t === 4 || t === 5 || t === 8 || t === 9) blockModel.setBlockType(r, 0)
        }
        blockModel.endGroup()
        cursor.sync()
    }

    // Link button / Cmd+K: open the URL editor over the right target. A single-row
    // selection is wrapped; a caret inside an existing link edits that whole link;
    // a bare caret inserts the typed URL as its own link. (Multi-row selections
    // apply the URL per row.) The target range is SNAPSHOTTED here because opening
    // the popup steals the editor's selection/focus.
    function applyLink() {
        if (!cursor.active) return
        var row = cursor.focusRow
        if (cursor.hasSel) {
            linkPopup.insertMode = false
            linkPopup.tRow0 = cursor.loRow; linkPopup.tCol0 = cursor.loCol
            linkPopup.tRow1 = cursor.hiRow; linkPopup.tCol1 = cursor.hiCol
            linkPopup.prefill = blockModel.linkAt(cursor.loRow, cursor.loCol)
        } else {
            var rng = blockModel.linkRangeAt(row, cursor.focusCol)
            if (rng && rng.length === 2) {                 // caret inside a link → edit it
                linkPopup.insertMode = false
                linkPopup.tRow0 = row; linkPopup.tCol0 = rng[0]
                linkPopup.tRow1 = row; linkPopup.tCol1 = rng[1]
                linkPopup.prefill = blockModel.linkAt(row, cursor.focusCol)
            } else {                                       // bare caret → insert URL as a link
                linkPopup.insertMode = true
                linkPopup.tRow0 = row; linkPopup.tCol0 = cursor.focusCol
                linkPopup.tRow1 = row; linkPopup.tCol1 = cursor.focusCol
                linkPopup.prefill = ""
            }
        }
        linkPopup.openAtCaret()
    }
    function commitLink(url) {
        url = (url || "").trim()
        var r0 = linkPopup.tRow0, r1 = linkPopup.tRow1
        blockModel.beginGroup(r0, r1)
        if (linkPopup.insertMode) {
            if (url.length > 0) {
                blockModel.insertText(r0, linkPopup.tCol0, url, 0)
                blockModel.setLink(r0, linkPopup.tCol0, linkPopup.tCol0 + url.length, url)
            }
        } else {
            for (var r = r0; r <= r1; ++r) {
                var s = (r === r0) ? linkPopup.tCol0 : 0
                var e = (r === r1) ? linkPopup.tCol1 : blockModel.contentForRow(r).length
                blockModel.setLink(r, s, e, url)           // empty url = remove the link
            }
        }
        blockModel.endGroup()
        cursor.sync()
    }
    // Type/level of the block under the caret — for the rail's heading state.
    readonly property int caretType:  (blockModel.contentRevision, blockModel.layoutRevision, blockModel.typeForRow(cursor.focusRow))
    readonly property int caretLevel: (blockModel.contentRevision, blockModel.layoutRevision, blockModel.levelForRow(cursor.focusRow))

    // Set heading `level` (1–5) on the caret's block(s); click the active level
    // again to toggle back to a paragraph. One grouped undo step; no selection
    // needed (acts on the caret block / each block in a selection).
    function setHeading(level) {
        var lo = cursor.loRow, hi = cursor.hiRow
        var isOn = caretType === 1 && caretLevel === level   // 1 = Heading
        blockModel.beginGroup(lo, hi)
        for (var r = lo; r <= hi; ++r) blockModel.setHeading(r, isOn ? 0 : level)
        blockModel.endGroup()
        cursor.sync()
    }
    // Toggle the caret block(s) to/from a block type (4 quote, 5 list); click the
    // active type again → paragraph. One grouped undo step.
    function toggleBlock(type) {
        var lo = cursor.loRow, hi = cursor.hiRow
        var isOn = caretType === type
        blockModel.beginGroup(lo, hi)
        for (var r = lo; r <= hi; ++r) blockModel.setBlockType(r, isOn ? 0 : type)
        blockModel.endGroup()
        cursor.sync()
    }
    function addDivider() { blockModel.insertDivider(cursor.focusRow); cursor.sync() }
    // Toggle the caret block to/from a (plain) code block. ```lang + Enter sets a
    // language; this button makes/removes a code block without one.
    function toggleCodeBlock() {
        var r = cursor.focusRow
        if (blockModel.typeForRow(r) === 2) blockModel.setBlockType(r, 0)
        else blockModel.makeCodeBlock(r, "")
        cursor.sync()
    }
    // --- Block context-menu actions (operate on the right-clicked row) ---
    // These move the caret off the block being edited, so each first consumes that
    // block's pending inline markdown into spans — the same rule as cursor.move.
    // (Right-click does NOT move the caret, so cursor.focusRow may differ from row;
    // only the focused row can hold uncommitted markdown. commitMarkdown is a safe
    // no-op when there's none / the block isn't a text block.)
    // ensureVisible (stable `row` param, NOT focusRow which setCaret has mutated)
    // keeps the caret on-screen — a new block created on the last visible row lands
    // below the fold otherwise (same "block exists but unseen" family as the Enter bug).
    function addBlockAbove(row) { blockModel.commitMarkdown(cursor.focusRow); blockModel.insertBlock(row);     cursor.setCaret(row, 0);     cursor.sync(); root.ensureVisible(row) }
    function addBlockBelow(row) { blockModel.commitMarkdown(cursor.focusRow); blockModel.insertBlock(row + 1); cursor.setCaret(row + 1, 0); cursor.sync(); root.ensureVisible(row + 1) }
    // Context-menu Paste: paste where the click pointed. Text-ish targets take
    // the clipboard at the caret (moved to the block's end if it wasn't
    // already inside); non-text targets (media/table/divider) get a fresh
    // paragraph below first — a caret parked inside their JSON content would
    // corrupt it — and media/table pastes then CONSUME that empty paragraph.
    function pasteAtBlock(row, r, c) {
        if (row < 0) return
        // A run menu: the selection is the target — replace it (⌘V's path).
        if (blockMenu.menuInSel) { doPaste(); return }
        var t = blockModel.typeForRow(row)
        var textish = !(t === 3 || t === 6)   // Media / Divider
        if (textish) {
            if (cursor.focusRow !== row)
                cursor.setCaret(row, blockModel.contentForRow(row).length)
        } else {
            addBlockBelow(row)                            // caret lands in it
        }
        doPaste()
    }
    function duplicateBlock(row) { blockModel.commitMarkdown(cursor.focusRow); blockModel.duplicateBlock(row); cursor.setCaret(row + 1, 0); cursor.sync(); root.ensureVisible(row + 1) }
    // Duplicate the run [lo, hi] after itself (run menu); the copy becomes the
    // selection so a second duplicate / move / delete keeps acting on blocks.
    function duplicateRun(lo, hi) {
        if (lo === hi) { duplicateBlock(lo); return }
        blockModel.beginGroup(Math.min(cursor.focusRow, lo), Math.max(cursor.focusRow, hi))
        blockModel.commitMarkdown(cursor.focusRow)
        blockModel.duplicateBlocks(lo, hi)
        blockModel.endGroup()
        var n = hi - lo + 1
        cursor.anchorRow = hi + 1; cursor.anchorCol = 0
        cursor.focusRow = hi + n; cursor.focusCol = cursor.opaque(hi + n) ? 0 : blockModel.contentForRow(hi + n).length
        cursor.goalX = -1; cursor.sync()
        root.ensureVisible(hi + n)
    }
    // Make code: skip the commit when converting the focused row itself, so its
    // markers stay LITERAL as code (don't strip *…* into a span code ignores).
    function makeCodeAt(row)    { if (cursor.focusRow !== row) blockModel.commitMarkdown(cursor.focusRow); blockModel.makeCodeBlock(row, ""); cursor.setCaret(row, 0); cursor.sync() }
    function insertTableAt(row) {
        blockModel.commitMarkdown(cursor.focusRow)
        // SR-4 S7a: a derived table — a header row and two body rows of three cells.
        const first = blockModel.insertTableRows(row, 3, 3)
        if (first >= 0) { cursor.setCaret(first, 0); root.ensureVisible(first) }
    }
    function insertTableAtCaret() { insertTableAt(cursor.focusRow) }

    // --- Clipboard (copy / cut / paste), table- and text-aware ---

    function doCopy() {
        // Derived tables (SR-4 S8c, R-I2): a cell rectangle or a grip-picked row / column set copies
        // by the CELL grain — the fragment payload, quoted TSV, and one HTML table (included header
        // rows in <thead>).
        const rect = root.cellRect
        const set = root.tableSetLive() ? root.tableSet : null
        if (rect || set) {
            const head = rect ? rect.head : set.head
            const rows = [], cols = []
            if (rect) {
                for (let r = rect.r0; r <= rect.r1; ++r) rows.push(r)
                for (let c = rect.c0; c <= rect.c1; ++c) cols.push(c)
            } else if (set.kind === "row") {
                for (let i = 0; i < set.items.length; ++i) rows.push(set.items[i])
                for (let c = 0; c < blockModel.tableColumnCount(head); ++c) cols.push(c)
            } else {
                for (let r = 0; r < blockModel.tableRowCount(head); ++r) rows.push(r)
                for (let i = 0; i < set.items.length; ++i) cols.push(set.items[i])
            }
            clipboard.writeBlocks(blockModel.tableCopyPayload(head, rows, cols), blockModel.tableCellsTSV(head, rows, cols),
                                  exporter.tableCellsHtml(head, rows, cols), "")
            return
        }
        // Document range (or the whole focus row when nothing is selected):
        // every flavour at once via copyRange.
        if (cursor.hasSel) {
            var e = cursor.effectiveRange()
            root.copyRange(e.lR, e.lC, e.hR, e.hC)
        } else {
            var fr0 = cursor.focusRow
            root.copyRange(fr0, 0, fr0, cursor.opaque(fr0) ? 0 : blockModel.contentForRow(fr0).length)
        }
    }
    // Paste Special ▸ Paste into cells (S8d): the next paste fills a table by position, header rows as content.
    property bool pasteIntoCells: false
    function pasteIntoCellsAt(row) {
        pasteIntoCells = true
        pasteAtBlock(row)
        pasteIntoCells = false
    }
    // Rich copy (0.5.0): the x-mnd-blocks payload PLUS the flavours other
    // apps read — plain text (a table's TSV), a lone table's HTML, a lone image
    // block's raster. Opaque rows never leak descriptor JSON.
    function copyRange(lR, lC, hR, hC) {
        var json = blockModel.clipboardPayloadForRange(lR, lC, hR, hC)
        var txt  = blockModel.plainTextForRange(lR, lC, hR, hC)
        var html = "", img = ""
        // A range holding a split row's record — whole layout rows or tables (S8c, R-I2): the text
        // is markdown (tables as GFM), the HTML the page walker's fragment.
        let structured = false
        for (let r = lR; r <= hR && !structured; ++r) structured = blockModel.typeForRow(r) === 10
        if (structured) {
            txt = exporter.copyMarkdown(lR, hR)
            html = exporter.htmlFragment(lR, hR)
        } else if (lR === hR) {
            var t = blockModel.typeForRow(lR)
            if (t === 3 && blockModel.mediaKind(lR) === "image") img = blockModel.mediaUrl(lR)
        }
        clipboard.writeBlocks(json, txt, html, img)
    }
    // Insert an inline choice chip at the caret (DT-2, ⌥⌘C 2026-08-20):
    // default tri-state set, picker opens immediately at the new chip
    // (the applyLink insert-mode shape).
    function insertChoiceChip() {
        if (!blockModel.documentOpen || root.inkMode) return
        // Frame tabs refuse — except a table's grid frame, whose cells are blocks and
        // take chips like the document view.
        if (root.activeFrameId !== "" && root.frameLo < 0) return
        if (root.typedCellHere() > 0) return    // a typed table cell holds its column's chip only
        if (cursor.hasSel) cursor.deleteSelection()
        var row = cursor.focusRow
        var s = blockModel.insertChoiceAt(row, cursor.focusCol)
        if (s < 0) return
        var range = blockModel.choiceRangeAt(row, s)
        if (range.length === 2) cursor.setCaret(row, range[1])   // park after the chip
        var cell = root.cellForRow(row)
        if (cell && cell.teItem) {
            var rr = cell.teItem.positionToRectangle(s)
            var pt = cell.teItem.mapToItem(root, rr.x, rr.y + rr.height + 4)
            root.openInlineChoicePicker(row, s, pt.x, pt.y)
        }
    }
    // Copy as Markdown (⇧⌘C, 2026-08-20): the selected block range as
    // clipboard markdown — whole blocks, the block-model grain. No selection
    // → the whole document (with its name header; fragments omit it).
    function copyAsMarkdown() {
        if (!blockModel.documentOpen) return
        var whole = !cursor.hasSel
        var md = whole ? exporter.copyMarkdown(-1, -1)
                       : exporter.copyMarkdown(cursor.loRow, cursor.hiRow)
        if (md.length === 0) return
        clipboard.writeText(md)
        var n = cursor.hiRow - cursor.loRow + 1
        Toasts.show(whole ? qsTr("Copied document as Markdown")
                  : n === 1 ? qsTr("Copied block as Markdown")
                            : qsTr("Copied %1 blocks as Markdown").arg(n))
    }
    // Paste (SR-4 S8d2, R-I9 8a): the C++ router (ClipboardPaster::route) decides which flavour
    // wins for this clipboard and caret; this executes it. A flavour that yields nothing is masked
    // and the router asked again (the old fall-through chain, made explicit).
    function doPaste() {
        // URLs + raster bytes on the SAME clipboard = the screen-capture-app
        // signature (Finder copies carry URLs only). The URL then points at
        // the app's temp file — force the sidecar copy even if the path
        // looks stable, in every URL branch below.
        const ephemeralUrls = clipboard.hasImage()
        const urls = clipboard.readUrls()
        const html = clipboard.hasHtml() ? clipboard.readHtml() : ""
        const payload = clipboard.hasBlocks() ? clipboard.readBlocks() : ""
        const txt = clipboard.readText()
        const input = { hasBlocks: payload.length > 0, hasHtml: html.length > 0, hasImage: clipboard.hasImage(),
                        bareRemoteImage: html.length > 0 && blockModel.htmlIsBareRemoteImage(html),
                        noTable: false, urls: urls.length, text: txt }
        const target = { sketchTab: root.activeSketchRow >= 0,
                         codeBlock: blockModel.typeForRow(cursor.focusRow) === 2 && (!cursor.hasSel || cursor.loRow === cursor.hiRow),
                         inTable: blockModel.tableHeadOf(cursor.focusRow) >= 0 }
        for (let guard = 0; guard < 8; ++guard) {
            const action = paster.routePaste(input, target)
            switch (action) {
            case "nothing":
                return
            case "sketchUrls": {   // images (copied from our app or outside) drop onto the canvas
                let any = false
                for (let i = 0; i < urls.length; ++i)
                    if (blockModel.sketchAddImageFromUrl(root.activeSketchRow, urls[i], ephemeralUrls)) any = true
                if (any) return
                input.urls = 0
                continue
            }
            case "sketchRaster":
                blockModel.sketchAddImageFromClipboard(root.activeSketchRow)
                return
            case "blocks":   // our own flavour: the paster deletes the selection itself, ONE undo entry, reports via onPasteFinished
                if (cursor.hasSel) {
                    const pe = cursor.effectiveRange()
                    paster.startPaste(blockModel, payload, cursor.focusRow, cursor.focusCol, pe.lR, pe.lC, pe.hR, pe.hC, root.pasteIntoCells)
                } else {
                    paster.startPaste(blockModel, payload, cursor.focusRow, cursor.focusCol, -1, 0, -1, 0, root.pasteIntoCells)
                }
                return
            case "codeVerbatim": {   // user-caught 2026-08-21: no HTML flavouring, no markdown prefixes, no table detection
                if (cursor.hasSel) cursor.deleteSelection()
                const code = txt.replace(/\r\n/g, "\n").replace(/\r/g, "\n")
                const ccol = cursor.focusCol
                blockModel.insertText(cursor.focusRow, ccol, code)
                cursor.setCaret(cursor.focusRow, ccol + code.length)
                root.ensureVisible(cursor.focusRow)
                return
            }
            case "html": {   // Word / Docs / Excel / web → structured blocks; a table-only clipboard into a cell fills it (S8d)
                const hg = root.pasteGroupBegin()
                const hrect = root.cellRect   // a cell rectangle's anchor is its top-left cell
                const hanchor = hrect ? blockModel.tableCellAt(hrect.head, hrect.r0, hrect.c0) : cursor.focusRow
                const hc = blockModel.pasteHtml(hanchor >= 0 ? hanchor : cursor.focusRow, hanchor >= 0 && hrect ? 0 : cursor.focusCol, html)
                root.pasteGroupEnd(hg)
                if (hc && hc.length === 2) { cursor.setCaret(hc[0], hc[1]); root.ensureVisible(hc[0]); return }
                input.hasHtml = false   // nothing usable (e.g. a bare image wrapper): the media flavours next
                continue
            }
            case "urls": {   // copied file(s) → media, like a drop; the caret leaves its block, so commit its inline md first
                blockModel.commitMarkdown(cursor.focusRow)
                let afterRow = cursor.focusRow, any = false
                for (let i = 0; i < urls.length; ++i) {
                    const nr = blockModel.insertMediaFromUrl(afterRow, urls[i], ephemeralUrls)
                    if (nr >= 0) { afterRow = nr; any = true }
                }
                if (any) { cursor.setCaret(afterRow, 0); root.ensureVisible(afterRow); return }
                input.urls = 0
                continue
            }
            case "raster": {   // a screenshot / Copy Image → a media block
                blockModel.commitMarkdown(cursor.focusRow)
                const imgRow = blockModel.insertImageFromClipboard(cursor.focusRow)
                if (imgRow >= 0) { cursor.setCaret(imgRow, 0); root.ensureVisible(imgRow); return }
                input.hasImage = false
                continue
            }
            case "tableTsv": {   // SR-4 A5: fill cells from the anchor (a rectangle's top-left), growing the table
                const gh = blockModel.tableHeadOf(cursor.focusRow), rect = root.cellRect
                const gland = blockModel.tablePasteTSV(gh, rect ? rect.r0 : blockModel.tableRowOf(cursor.focusRow),
                                                      rect ? rect.c0 : blockModel.tableColumnOf(cursor.focusRow), txt)
                if (gland >= 0) { cursor.setCaret(gland, blockModel.contentForRow(gland).length); root.ensureVisible(gland) }
                return
            }
            case "tableFromTsv": {   // rectangular TSV → a derived table below the caret's block
                const tg = root.pasteGroupBegin()
                blockModel.commitMarkdown(cursor.focusRow)
                const tr = blockModel.insertGridFromTSV(cursor.focusRow, txt)
                root.pasteGroupEnd(tg)
                if (tr >= 0) { cursor.setCaret(tr, blockModel.contentForRow(tr).length); root.ensureVisible(tr); return }
                input.noTable = true
                continue
            }
            case "text": {   // smart paste: blocks, markdown prefixes, inline marks, fences — one undo step
                const tg = root.pasteGroupBegin()
                const caret = blockModel.pasteText(cursor.focusRow, cursor.focusCol, txt)
                root.pasteGroupEnd(tg)
                if (caret && caret.length === 2) { cursor.setCaret(caret[0], caret[1]); root.ensureVisible(caret[0]) }
                return
            }
            default:
                return
            }
        }
    }
    function doCut() {
        doCopy()
        if (cursor.hasSel) cursor.deleteSelection()
        else if (cursor.opaqueHere()) root.deleteBlock(cursor.focusRow)   // ⌘X on media/divider cuts it
    }
    function copyBlock(row) {
        root.copyRange(row, 0, row, cursor.opaque(row) ? 0 : blockModel.contentForRow(row).length)
    }
    // Spell menu: the flagged word's text, and "replace with a suggestion" as
    // ONE undo step (replaceText keeps spans covering the word).
    function menuIssueWord() {
        var it = root.menuIssue
        if (!it) return ""
        return blockModel.contentForRow(root.menuRow).substring(it.s, it.e)
    }
    // Fix all: the top suggestion of every issue in the block, applied right-to-left
    // so earlier offsets stay valid — ONE undo.
    function applyAllSuggestions() {
        var row = root.menuRow
        if (row < 0) return
        var list = spell.issuesForRow(row)
        var fixes = []
        for (var i = 0; i < list.length; ++i) if (list[i].suggestions.length > 0) fixes.push(list[i])
        if (fixes.length === 0) return
        fixes.sort(function(a, b) { return b.s - a.s })
        blockModel.beginGroup(row, row)
        for (var k = 0; k < fixes.length; ++k) {
            var f = fixes[k]
            blockModel.replaceText(row, f.s, f.e, f.suggestions[0])
        }
        blockModel.endGroup()
        if (cursor.focusRow === row)
            cursor.setCaret(row, Math.min(cursor.focusCol, blockModel.contentForRow(row).length))
        spell.flushRow(row)
        root.menuIssue = null
        Toasts.show(fixes.length === 1 ? qsTr("Fixed 1 issue") : qsTr("Fixed %1 issues").arg(fixes.length))
    }
    function applySpellSuggestion(sug) {
        var it = root.menuIssue
        if (!it || sug === undefined || sug === "") return
        blockModel.replaceText(root.menuRow, it.s, it.e, sug)
        cursor.setCaret(root.menuRow, it.s + sug.length)
        spell.flushRow(root.menuRow)
        root.menuIssue = null
    }
    // Replace-selection pastes are ONE undo step: open the group over the
    // selection band (pre-mutation coords), delete, paste, close — and close
    // BEFORE the caret write (the entry's caret-before reads the model's
    // cursor at endTxn time).
    function pasteGroupBegin() {
        if (!cursor.hasSel) return false
        blockModel.beginGroup(cursor.loRow, cursor.hiRow)
        cursor.deleteSelection()
        return true
    }
    function pasteGroupEnd(opened) { if (opened) blockModel.endGroup() }
    // Comment the current single-row text selection: mint the thread, then
    // open its card in the Inspector's comments view.
    function addCommentOnSelection() {
        if (!cursor.hasSel || cursor.loRow !== cursor.hiRow) return
        var tid = blockModel.addComment(cursor.loRow, cursor.loCol, cursor.hiCol)
        if (tid !== "" && inspector) inspector.showComments(tid)
    }

    function deleteBlock(row) {
        // Deleting a DIFFERENT block (right-click menu) moves the caret off the
        // edited row → commit its inline md first. Skip when deleting the focused
        // block itself: its content is about to vanish (no point in an undo step),
        // and the backspace/forwardDelete callers always pass row == focusRow.
        // The commit is GROUPED with the delete so the gesture is ONE undo step
        // (ungrouped it pushed two entries = two ⌘Z per delete). endGroup runs
        // BEFORE setCaret: the entry's caret-before reads the model's cursor at
        // endTxn time, and setCaret→sync→noteCaret would clobber it first.
        var grouped = cursor.focusRow !== row
        if (grouped) {
            blockModel.beginGroup(Math.min(cursor.focusRow, row), Math.max(cursor.focusRow, row))
            blockModel.commitMarkdown(cursor.focusRow)
        }
        // removeBlocks refills a would-be-empty document with a fresh paragraph
        // inside the same txn (the old setContent("") path left a media row
        // holding empty JSON).
        blockModel.removeBlocks(row, row)
        if (grouped) blockModel.endGroup()
        cursor.setCaret(Math.max(0, Math.min(row, blockModel.count - 1)), 0)
        cursor.sync()
    }
    // Delete the run [lo, hi] (context menu "Delete blocks" on a selected run):
    // the same one-undo-step shape as deleteBlock, over the whole band.
    function deleteRun(lo, hi) {
        if (lo === hi) { deleteBlock(lo); return }
        blockModel.beginGroup(Math.min(cursor.focusRow, lo), Math.max(cursor.focusRow, hi))
        if (cursor.focusRow < lo || cursor.focusRow > hi) blockModel.commitMarkdown(cursor.focusRow)
        blockModel.removeBlocks(lo, hi)
        blockModel.endGroup()
        cursor.setCaret(Math.max(0, Math.min(lo, blockModel.count - 1)), 0)
        cursor.sync()
    }

    // Open the block context menu at viewport (vx,vy) for `row`. (vx,vy) is also
    // reused to anchor the language picker if "Change language…" is chosen.
    function closeBlockMenu() { blockMenu.close() }
    function openBlockMenu(vx, vy, row) {
        if (row >= 0) spell.requestSuggestions(row)   // every issue in the block gets suggestions for "Fix all"
        root.menuRow = row; root.menuX = vx; root.menuY = vy
        blockMenu.open()    // x/y are reactive bindings that clamp it on-screen
    }
    // Shorten a URL for a menu label: drop the scheme/www, keep host + a little
    // path (the menu also elides, so this is just for a tidy label).
    function truncUrl(u) {
        var s = u.replace(/^https?:\/\//, "").replace(/^www\./, "")
        return s.length > 30 ? s.substring(0, 29) + "…" : s
    }
    // Language picker for the code block at `row`, anchored where the menu was.
    // The field opens EMPTY (it's a filter over the full list; prefilling
    // would filter the list down to the current language) — the current
    // pick shows as a check in the list instead.
    function openLangPopupForRow(row) {
        langPopup.targetRow = row
        langField.text = ""
        langPopup.open()    // x/y are reactive bindings (root.menuX/menuY → clamped)
        langField.forceActiveFocus()
    }

    // Clear ALL formatting → plain paragraph: reset heading/quote/list block
    // style AND strip inline spans. Acts on the caret's block (no selection
    // needed); with a selection, clears spans over the selected range of each
    // block. Code blocks are left as-is. One grouped undo step.
    function clearFormatting() {
        var lo = cursor.loRow, hi = cursor.hiRow
        blockModel.beginGroup(lo, hi)
        for (var r = lo; r <= hi; ++r) {
            blockModel.setHeading(r, 0)                  // heading/quote/list → paragraph
            var rs = cursor.hasSel ? rowSelStart(r) : 0
            var re = cursor.hasSel ? rowSelEnd(r) : blockModel.contentForRow(r).length
            blockModel.clearFormat(r, rs, re)            // strip inline spans
        }
        blockModel.endGroup()
        cursor.sync()
    }

    // Space-up releases the sketch tab's hand-pan (press side lives in the
    // sketch branch of Keys.onPressed below). Gated off while the text
    // overlay edits — a stuck panMode would hide the frame handles.
    Keys.onReleased: (event) => {
        if (root.activeSketchRow >= 0 && !sketchTextSession.active
                && event.key === Qt.Key_Space && !event.isAutoRepeat) {
            sketchEditCanvas.panMode = false
            event.accepted = true
        }
        else if (root.activePdfRow >= 0
                 && event.key === Qt.Key_Space && !event.isAutoRepeat) {
            root.pdfSpaceHeld = false
            event.accepted = true
        }
    }

    Keys.onPressed: (event) => {
        var shift = (event.modifiers & Qt.ShiftModifier) !== 0
        var cmd = (event.modifiers & Qt.ControlModifier) !== 0   // Cmd on macOS (Qt maps it)
        var k = event.key
        // Sketch text overlay owns the keyboard while editing (belt-and-braces:
        // the TextEdit consumes nearly everything before it can bubble here).
        if (root.activeSketchRow >= 0 && sketchTextSession.active) {
            if (k === Qt.Key_Escape) sketchTextSession.commit()
            event.accepted = true
        }
        // Same island rule for the ink text overlay.
        else if (root.inkMode && inkTextSession.active) {
            if (k === Qt.Key_Escape) inkTextSession.commit()
            event.accepted = true
        }
        // And for the PDF page-chip overlay (must sit above the pdf branch).
        else if (root.activePdfRow >= 0 && pdfTextSession.active) {
            if (k === Qt.Key_Escape) pdfTextSession.commit()
            event.accepted = true
        }
        else if (k === Qt.Key_Escape) {
            // Cancel the current op: block-drag (revert, no move) → text-drag →
            // collapse selection → disarm format toggle. In a table: collapse the
            // cell selection, else step the caret out below the table.
            // Studio first: drop an in-flight stroke, then disarm the tool.
            // Ink mode: drop an in-flight stroke, else ONE press back to Type —
            // the exit clears the selection itself (onInkModeChanged).
            if (root.inkMode && inkCanvas.drawing) { inkCanvas.cancelStroke() }
            else if (root.inkMode) { root.inspector.drawTool = "type" }
            else if (root.activeVideoRow >= 0 && studioAnnotator.drawing) { studioAnnotator.cancelStroke() }
            else if (root.activeSketchRow >= 0 && sketchEditCanvas.drawing) { sketchEditCanvas.cancelStroke() }
            else if (root.activePdfRow >= 0 && root.pdfActiveInk && root.pdfActiveInk.drawing) { root.pdfActiveInk.cancelStroke() }
            else if ((root.activeVideoRow >= 0 || root.activeSketchRow >= 0 || root.activePdfRow >= 0)
                     && root.inspector && root.inspector.drawTool !== "type"
                     && root.inspector.drawTool !== "select") { root.inspector.drawTool = "select" }
            else if (root.activeSketchRow >= 0 && sketchEditCanvas.hasSelection) { sketchEditCanvas.clearSelection() }
            else if (root.activeVideoRow >= 0 && studioAnnotator.hasSelection) { studioAnnotator.clearSelection() }
            else if (root.activePdfRow >= 0 && root.pdfActiveInk && root.pdfActiveInk.hasSelection) { root.pdfActiveInk.clearSelection() }
            else if (root.dividerDragging) { root.cancelDividerDrag() }
            else if (root.pulling) { root.cancelPull() }
            else if (root.blockDragging) { root.blockDragging = false; root.blockDragRow = -1; root.dropGap = -1; root.blockDragCount = 1 }
            else if (root.dragging) { root.dragging = false }
            else if (root.tableColDragging) { root.tableColDragging = false; root.tableColGap = -1 }
            else if (root.tableSetLive()) { root.tableSet = null }
            else if (root.boardMode) { root.showGridView() }   // board → grid
            else if (cursor.hasSel && root.selectionIsSplitRow()
                     && blockModel.tableHeadOf(cursor.loRow) >= 0
                     && blockModel.tableRowCount(blockModel.tableHeadOf(cursor.loRow)) > 1) {
                // SR-0 A8 rung 3: a selected table row → the whole table.
                const head = blockModel.tableHeadOf(cursor.loRow), recs = blockModel.tableRecords(head)
                const first = blockModel.nextLeaf(head), last = blockModel.splitRowLast(recs[recs.length - 1])
                cursor.anchorRow = first; cursor.anchorCol = 0
                cursor.focusRow = last; cursor.focusCol = blockModel.contentForRow(last).length
                cursor.sync()
                root.selObject = { kind: "table", head: head, r: 0, lo: first, hi: last }
            }
            else if (cursor.hasSel && (root.selectionIsSplitRow() || root.selectionIsTable())) {
                // SR-0 §4.8 rung 4: a selected split row or table → the caret to the block below it
                // (a top-level paragraph is made when there is none).
                const head = blockModel.tableHeadOf(cursor.loRow)
                const lastRec = head >= 0 ? blockModel.tableRecords(head).slice(-1)[0] : blockModel.splitRowOf(cursor.loRow)
                const below = blockModel.nextLeaf(blockModel.splitRowLast(lastRec))
                const land = (below >= 0 && blockModel.laneForRow(below) < 0) ? below
                                                                              : blockModel.insertParagraphBelow(lastRec)
                cursor.setCaret(land, 0); root.ensureVisible(land)
            }
            else if (cursor.hasSel) { cursor.setCaret(cursor.focusRow, cursor.focusCol) }
            else if (cursor.activeMarks !== 0 || cursor.armedFg !== "" || cursor.armedBg !== "") { cursor.clearMarks() }
            else if (blockModel.laneForRow(cursor.focusRow) >= 0) {
                // Rung 2: a caret in a lane → select its whole split row.
                const rec = blockModel.splitRowOf(cursor.focusRow)
                const first = blockModel.nextLeaf(rec), last = blockModel.splitRowLast(rec)
                cursor.anchorRow = first; cursor.anchorCol = 0
                cursor.focusRow = last; cursor.focusCol = blockModel.contentForRow(last).length
                cursor.sync()
                const th = blockModel.tableHeadOf(rec)   // a table row picked this way is an object (Delete removes it)
                root.selObject = th >= 0 ? { kind: "row", head: th, r: blockModel.tableRowOf(rec), lo: first, hi: last } : null
            }
            event.accepted = true
        }
        // Studio: ⌘Z routes to the ANNOTATION undo stack — video notes live
        // outside the document DB, so doc undo must not fire while the
        // hidden document is invisible.
        else if (cmd && (k === Qt.Key_Z || k === Qt.Key_Y) && root.activeVideoRow >= 0) {
            if (k === Qt.Key_Y || shift) studioAnnotator.redo(); else studioAnnotator.undo()
            event.accepted = true
        }
        // PDF tab: page ink is document content (the sketch precedent) — doc
        // ⌘Z/redo and Delete-selected-stroke pass through; everything else is
        // still swallowed so typing can't invisibly edit the hidden document.
        // Must sit above the generic ⌘Z/⌘V branches.
        else if (root.activePdfRow >= 0) {
            if (cmd && k === Qt.Key_Z && shift) blockModel.redo()
            else if (cmd && k === Qt.Key_Z) blockModel.undo()
            else if (cmd && k === Qt.Key_Y) blockModel.redo()
            else if ((k === Qt.Key_Delete || k === Qt.Key_Backspace)
                     && root.pdfActiveInk && root.pdfActiveInk.hasSelection)
                root.pdfActiveInk.deleteSelection()
            else if (k === Qt.Key_Space && !event.isAutoRepeat)
                root.pdfSpaceHeld = true            // hold the hand (list pan)
            // Zoom keys (2026-08-20) — the ZoomBadge menu's advertised pair
            // (Fit ⌘1 / 100% ⌘0) plus ⌘±, previously swallowed unhandled.
            else if (cmd && (k === Qt.Key_Plus || k === Qt.Key_Equal)) root.pdfZoomStep(1)
            else if (cmd && k === Qt.Key_Minus) root.pdfZoomStep(-1)
            else if (cmd && k === Qt.Key_0) root.pdfZoom100()
            else if (cmd && k === Qt.Key_1) root.pdfZoomFit()
            event.accepted = true
        }
        // Copy as Markdown — document view only (full-frame tabs and ink
        // mode have their own clipboard rules below).
        else if (cmd && shift && k === Qt.Key_C && root.activeFrameId === ""
                 && !root.inkMode) {
            root.copyAsMarkdown()
            event.accepted = true
        }
        // Insert an inline choice chip (DT-2, ⌥⌘C) — same document-view gate.
        // macOS trap (user-caught 2026-08-21): with Option held, Qt often
        // reports the OPTION-LAYER character — ⌥C arrives as Key_Ccedilla
        // ("ç") on many layouts — so match both.
        else if (cmd && (event.modifiers & Qt.AltModifier)
                 && (k === Qt.Key_C || k === Qt.Key_Ccedilla)
                 && (root.activeFrameId === "" || root.frameLo >= 0)
                 && !root.inkMode) {
            root.insertChoiceChip()   // doc view OR a table's grid frame
            event.accepted = true
        }
        // Video/sketch tabs + ink mode: clipboard ops target the (hidden or
        // annotation-covered) document — gate ⌘V/⌘X (silent document edits);
        // ⌘C copies an invisible selection, swallow it too. EXCEPT ⌘V in a
        // SKETCH tab (regression fix 2026-08-21): the canvas is a real paste
        // target — doPaste's sketch branch drops images onto it and never
        // touches the document.
        else if (cmd && (k === Qt.Key_C || k === Qt.Key_V || k === Qt.Key_X)
                 && (root.activeVideoRow >= 0 || root.activeSketchRow >= 0
                     || root.inkMode)) {
            if (k === Qt.Key_V && root.activeSketchRow >= 0) root.doPaste()
            event.accepted = true
        }
        else if (cmd && k === Qt.Key_Z && shift) { blockModel.redo(); event.accepted = true }
        else if (cmd && k === Qt.Key_Z) { blockModel.undo(); event.accepted = true }
        else if (cmd && k === Qt.Key_Y) { blockModel.redo(); event.accepted = true }
        // ⌘] / ⌘[ (SR-0 §4.7): list indent / outdent everywhere — inside a lane Tab moves
        // between lanes, so indent needs its own keys.
        else if (cmd && (k === Qt.Key_BracketRight || k === Qt.Key_BracketLeft)) {
            blockModel.indentBlocks(cursor.hasSel ? cursor.loRow : cursor.focusRow,
                                    cursor.hasSel ? cursor.hiRow : cursor.focusRow,
                                    k === Qt.Key_BracketRight ? 1 : -1)
            event.accepted = true
        }
        else if (cmd && k === Qt.Key_C) { root.doCopy(); event.accepted = true }
        else if (cmd && k === Qt.Key_V) { root.doPaste(); event.accepted = true }
        else if (cmd && !shift && k === Qt.Key_X) { root.doCut(); event.accepted = true }
        // ⌥⌘↑ / ⌥⌘↓ (0.5.0): move the focused block — or the whole selected
        // run — one slot. Document view only. (Arrow keys carry no macOS
        // option-layer character, unlike the ⌥⌘C trap above.)
        else if (cmd && (event.modifiers & Qt.AltModifier) !== 0
                 && (k === Qt.Key_Up || k === Qt.Key_Down)
                 && root.activeFrameId === "" && !root.inkMode && !root.boardMode) {
            var mlo = cursor.hasSel ? cursor.loRow : cursor.focusRow
            var mhi = cursor.hasSel ? cursor.hiRow : cursor.focusRow
            // Within its container (SR-0 §4.12): a lane block stays in its lane; a top-level
            // block or a selected split row steps over whole split rows. [] = at the edge.
            const mt = blockModel.moveTarget(mlo, mhi, k === Qt.Key_Up ? -1 : 1)
            if (mt.length === 4) root.moveRun(mt[0], mt[1], mt[2], mt[3])
            event.accepted = true
        }
        // Video studio: transport keys, then swallow everything else so typing
        // can't invisibly edit the hidden document underneath.
        else if (root.activeVideoRow >= 0) {
            if ((k === Qt.Key_Delete || k === Qt.Key_Backspace) && studioAnnotator.hasSelection)
                studioAnnotator.deleteSelection()
            else if (k === Qt.Key_Space) { root.ensureVideoActive(root.activeVideoRow); root.toggleVideo() }
            else if (k === Qt.Key_Left)  { root.ensureVideoActive(root.activeVideoRow); root.stepVideoFrames(-1) }
            else if (k === Qt.Key_Right) { root.ensureVideoActive(root.activeVideoRow); root.stepVideoFrames(1) }
            else if (k === Qt.Key_Home)  { root.ensureVideoActive(root.activeVideoRow); root.seekVideoStart() }
            else if (k === Qt.Key_End)   { root.ensureVideoActive(root.activeVideoRow); root.seekVideoEnd() }
            else if (k === Qt.Key_R)     { root.ensureVideoActive(root.activeVideoRow)
                                           if (shift) root.setVideoSpeed(1.0); else root.cycleVideoSpeed() }
            event.accepted = true
        }
        // Sketch tab: the canvas is mouse-driven; swallow everything so typing
        // can't invisibly edit the hidden document. (cmd+Z above = doc undo —
        // sketch strokes are document content, unlike video notes.) Delete/
        // Backspace removes the selected stroke/image when in select mode;
        // space holds the hand (camera pan); ⌘+/− zoom, ⌘0 = 100%, ⌘1 = Fit.
        else if (root.activeSketchRow >= 0) {
            if ((k === Qt.Key_Delete || k === Qt.Key_Backspace) && sketchEditCanvas.hasSelection)
                sketchEditCanvas.deleteSelection()
            else if (k === Qt.Key_Space && !event.isAutoRepeat)
                sketchEditCanvas.panMode = true
            else if (cmd && (k === Qt.Key_Plus || k === Qt.Key_Equal))
                sketchStage.zoomStep(1)
            else if (cmd && (k === Qt.Key_Minus || k === Qt.Key_Underscore))
                sketchStage.zoomStep(-1)
            else if (cmd && k === Qt.Key_0)
                sketchStage.zoomTo100()
            else if (cmd && k === Qt.Key_1)
                sketchStage.fitCamera()
            event.accepted = true
        }
        // Ink mode: the canvas is mouse-driven; swallow everything so typing
        // can't edit the document mid-annotation. (⌘Z above stays DOC undo —
        // ink is document content, the sketch precedent.) Delete/Backspace
        // removes the selected stroke.
        else if (root.inkMode) {
            if ((k === Qt.Key_Delete || k === Qt.Key_Backspace) && inkCanvas.hasSelection)
                inkCanvas.deleteSelection()
            event.accepted = true
        }
        // Board mode: cards are mouse-driven; swallow everything else so typing
        // can't invisibly edit the grid underneath.
        else if (root.boardMode) { event.accepted = true }
        else if ((k === Qt.Key_Backspace || k === Qt.Key_Delete) && root.tableSetLive()) {
            root.clearGridSet()   // S7b: a grip-picked set clears its cells
            event.accepted = true
        }
        else if (cmd && k === Qt.Key_A) { root.selectAllLadder(); event.accepted = true }
        else if (cmd && (k === Qt.Key_Return || k === Qt.Key_Enter) && blockModel.tableHeadOf(cursor.focusRow) >= 0) {
            if (!event.isAutoRepeat) root.tableInsertRowBelow()   // ⌘Enter: a row below, caret in the same column
            event.accepted = true
        }
        else if (k === Qt.Key_Space && !cmd && root.typedCellHere() === 2) {   // §4.14: Space cycles a check cell
            if (!event.isAutoRepeat) {
                const head = blockModel.tableHeadOf(cursor.focusRow)
                blockModel.tableCycleCellCheck(head, blockModel.tableRowOf(cursor.focusRow), blockModel.tableColumnOf(cursor.focusRow))
                cursor.setCaret(cursor.focusRow, 0)
            }
            event.accepted = true
        }
        else if (cmd && k === Qt.Key_B) { applyFormat("bold"); event.accepted = true }
        else if (cmd && k === Qt.Key_I) { applyFormat("italic"); event.accepted = true }
        else if (cmd && k === Qt.Key_U) { applyFormat("underline"); event.accepted = true }
        else if (cmd && shift && k === Qt.Key_X) { applyFormat("strike"); event.accepted = true }
        else if (cmd && k === Qt.Key_E) { applyFormat("code"); event.accepted = true }
        else if (cmd && shift && k === Qt.Key_H) { if (root.inspector) root.toggleHighlight(root.inspector.bgColor); event.accepted = true }
        else if (cmd && k === Qt.Key_Backslash) { clearFormatting(); event.accepted = true }
        else if (cmd && k === Qt.Key_K) { applyLink(); event.accepted = true }
        else if (k === Qt.Key_Right) { navRight(shift); event.accepted = true }
        else if (k === Qt.Key_Left) { navLeft(shift); event.accepted = true }
        else if (k === Qt.Key_Down) { navDown(shift); event.accepted = true }
        else if (k === Qt.Key_Up) { navUp(shift); event.accepted = true }
        else if (k === Qt.Key_Home) { navHome(shift); event.accepted = true }
        else if (k === Qt.Key_End) { navEnd(shift); event.accepted = true }
        else if (k === Qt.Key_PageDown) { navPageDown(shift); event.accepted = true }
        else if (k === Qt.Key_PageUp) { navPageUp(shift); event.accepted = true }
        else if (k === Qt.Key_Backspace) { cursor.backspace(event.isAutoRepeat); event.accepted = true }
        else if (k === Qt.Key_Delete) { cursor.forwardDelete(event.isAutoRepeat); event.accepted = true }
        else if (k === Qt.Key_Tab || k === Qt.Key_Backtab) {
            root.tabKey(k === Qt.Key_Backtab, event.isAutoRepeat)   // lanes navigate, lists indent; Tab never types
            event.accepted = true
        }
        else if (k === Qt.Key_Return || k === Qt.Key_Enter) { cursor.splitLine(shift, event.isAutoRepeat); event.accepted = true }
        else if (event.text.length === 1 && event.text >= " " && !cmd && root.typedCellHere() > 0) {
            // §4.14: a typed table cell takes values, not text. In a choice cell Space opens the
            // picker and a character opens it filtered ("d" + Enter picks "Doing").
            if (root.typedCellHere() === 1 && !event.isAutoRepeat) {
                const row = cursor.focusRow
                root.openGridChoicePicker(blockModel.tableHeadOf(row), blockModel.tableRowOf(row),
                                          blockModel.tableColumnOf(row), event.text === " " ? "" : event.text)
            }
            event.accepted = true
        }
        else if (event.text.length === 1 && event.text >= " ") { cursor.insertChar(event.text); event.accepted = true }
    }

    Timer { interval: 530; running: true; repeat: true; onTriggered: root.caretOn = !root.caretOn }

    // --- Dev-only pool probe (SR-2). Inert unless launched with
    // --pool-probe=<file.md> (Main.qml imports the fixture into a fresh tab).
    // Sweeps the document down, jumps around, nudges by a few pixels, runs
    // structural edits, sweeps back up — and after every step asserts that each
    // visible row has exactly ONE active pool delegate sitting at the model's y.
    // Prints POOL-PROBE lines and exits with the failure count (capped).
    Timer {
        id: poolProbe
        readonly property bool armed: Qt.application.arguments.some(
            function(a) { return a.indexOf("--pool-probe=") === 0 })
        property int phase: 0          // 0 sweep down · 1 jumps · 2 edits · 3 sweep up · 4 lanes · 5 sweep with lanes · 6 keys · 7 lane edits · 8 gestures · 9 tables · 10 sweep with tables · 11 table keys
        property int step: 0
        property int phaseStep: 0
        property int checks: 0
        property int fails: 0
        property int maxRows: 0
        property int seed: 12345
        interval: 40; repeat: true
        running: armed && flick.visible && blockModel.count > 1000
        onRunningChanged: if (running) console.log("POOL-PROBE START blocks", blockModel.count)
        function rand(n) { seed = (seed * 1103515245 + 12345) % 2147483648; return seed % n }
        function fail(msg) { if (++fails <= 40) console.log("POOL-PROBE FAIL", "step", step, "phase", phase, msg) }
        function verify() {
            blockModel.flushLayoutSpike()   // measure-back spikes are coalesced to the next turn; the checks read now
            var byRow = ({})
            for (var i = 0; i < pool.count; ++i) {
                var c = pool.itemAt(i)
                if (!c || !c.active) continue
                if (byRow[c.logicalRow] !== undefined) fail("row " + c.logicalRow + " in two delegates")
                byRow[c.logicalRow] = c
            }
            const inView = blockModel.visibleBlocks(flick.contentY, flick.contentY + flick.height)
            for (var vi = 0; vi < inView.length; ++vi) {
                const r = inView[vi]
                ++checks
                var d = byRow[r]
                if (!d) { fail("row " + r + " in view has no delegate"); continue }
                if (!d.visible || Math.abs(d.y - blockModel.yForRow(r)) > 0.5)
                    fail("row " + r + " at y " + d.y + " visible " + d.visible + ", model y " + blockModel.yForRow(r))
                if (blockModel.typeForRow(r) === 10) continue   // a record resolves to its lanes' blocks
                // Lane geometry: the delegate's column matches the lane.
                const g = root.laneOf(r)
                if (Math.abs(d.colLeft - d.cellInset - (root.leftEdge + g.x)) > 0.5 || Math.abs(d.measure + 2 * d.cellInset - g.w) > 0.5)
                    fail("row " + r + " column " + d.colLeft + "/" + d.measure + ", lane " + (root.leftEdge + g.x) + "/" + g.w)
                // A table cell sits at its column (SR-4 S5).
                if (d.tableCol >= 0 && Math.abs(g.x - root.tableShift(d.tableHead) - blockModel.tableColumnLeft(d.tableHead, d.tableCol)) > 0.5)
                    fail("table cell row " + r + " at x " + g.x + ", column " + d.tableCol + " starts at "
                         + blockModel.tableColumnLeft(d.tableHead, d.tableCol))
                // Pointer path: a point just inside the block's top-left resolves to it.
                const h = root.hitTest(root.leftEdge + g.x + 12, blockModel.yForRow(r) + 1)
                if (h.row !== r) fail("hitTest at the top of row " + r + " resolved row " + h.row)
            }
            maxRows = Math.max(maxRows, inView.length)
            // T3: while a table's body is under the viewport top with its header scrolled away,
            // the sticky header shows.
            const st = blockModel.tableStickyAt(flick.contentY)
            if (st.head !== undefined && flick.contentY > st.headerTop
                    && flick.contentY < st.tableBottom - (st.headerBottom - st.headerTop)) {
                ++checks
                if (!stickyHeader.visible) fail("sticky header hidden over table " + st.head + " at contentY " + flick.contentY)
                else if (!stickyShot) {                    // one inspection artifact next to the fixture
                    stickyShot = true
                    const arg = Qt.application.arguments.filter(function(a) { return a.indexOf("--pool-probe=") === 0 })[0]
                    flick.grabToImage(function(res) { res.saveToFile(arg.substring("--pool-probe=".length).replace(/[^\/]*$/, "") + "sticky.png") })
                }
            }
        }
        property bool stickyShot: false
        property bool frozenShot: false
        property int holdX: 0
        // T3: sideways past the left of the page, every table row in view has a frozen first cell.
        function verifyFrozen() {
            if (flick.contentX <= root.leftEdge + 1) return
            const inView = blockModel.visibleBlocks(flick.contentY, flick.contentY + flick.height)
            let tableRows = 0
            for (let i = 0; i < inView.length; ++i) {
                const head = blockModel.typeForRow(inView[i]) === 10 ? blockModel.tableHeadOf(inView[i]) : -1
                if (head >= 0 && flick.contentX > root.tableX(head) + 1
                    && root.tableX(head) + blockModel.tableWidth(head) > flick.contentX) ++tableRows
            }
            ++checks
            if (frozenColumn.rows.length !== tableRows)
                fail("frozen column shows " + frozenColumn.rows.length + " rows, " + tableRows + " table rows in view")
            else if (tableRows > 0 && !frozenShot) {
                frozenShot = true
                holdX = 3                                  // the grab lands a frame later: keep the sideways view
                const arg = Qt.application.arguments.filter(function(a) { return a.indexOf("--pool-probe=") === 0 })[0]
                flick.grabToImage(function(res) { res.saveToFile(arg.substring("--pool-probe=".length).replace(/[^\/]*$/, "") + "frozen.png") })
            }
        }
        function next(phaseDone) {
            if (phaseDone) {
                let tables = 0
                for (let i = 0; i < blockModel.count; ++i) if (blockModel.headerCount(i) > 0) ++tables
                console.log("POOL-PROBE PHASE", phase, "done at step", step, "blocks", blockModel.count, "tables", tables,
                            "checks", checks, "maxVisibleRows", maxRows)
                ++phase; phaseStep = 0
            } else ++phaseStep
        }
        onTriggered: {
            verify()
            verifyFrozen()
            ++step
            var maxY = Math.max(0, flick.contentHeight - flick.height)
            if (phase === 0) {
                flick.contentY = Math.min(maxY, flick.contentY + flick.height * 0.37)
                next(flick.contentY >= maxY)
            } else if (phase === 1) {
                flick.contentY = rand(Math.max(1, Math.floor(maxY)))
                next(phaseStep >= 80)
            } else if (phase === 2) {
                var r = Math.min(blockModel.count - 1, root.firstVisible + 2)
                switch (phaseStep % 8) {
                case 0: blockModel.insertBlock(r); break
                case 1: blockModel.removeBlock(r); break
                case 2: blockModel.undo(); break
                case 3: blockModel.redo(); break
                case 4: blockModel.setContent(r, "probe ".repeat(120)); break
                case 5: blockModel.undo(); break
                case 6: flick.contentY = Math.min(maxY, flick.contentY + 13); break
                case 7: flick.contentY = Math.max(0, flick.contentY - 29); break
                }
                next(phaseStep >= 96)
            } else if (phase === 3) {
                flick.contentY = Math.max(0, flick.contentY - flick.height * 0.61)
                next(flick.contentY <= 0)
            } else if (phase === 4) {
                // Lanes (SR-3): split blocks in view into two or three lanes, grow a
                // lane, pull a block into one — then keep walking down.
                const t = Math.min(blockModel.count - 1, root.firstVisible + 1 + phaseStep % 4)
                const ty = blockModel.typeForRow(t)
                if (ty !== 10) {
                    if (phaseStep % 3 === 2 && blockModel.laneForRow(t) >= 0) blockModel.insertBlock(t + 1)
                    else blockModel.splitIntoColumns(t, phaseStep % 2, 0.35 + 0.1 * (phaseStep % 4))
                }
                if (phaseStep % 5 === 4) flick.contentY = Math.min(maxY, flick.contentY + flick.height * 0.8)
                next(phaseStep >= 90)
            } else if (phase === 5) {
                if (phaseStep === 0) flick.contentY = 0   // sweep the whole document with lanes present
                else flick.contentY = Math.min(maxY, flick.contentY + flick.height * 0.37)
                next(phaseStep > 0 && flick.contentY >= maxY)
            } else if (phase === 6) {
                // Keys across lanes (SR-3 S6a): a random walk of arrows and Tab starting
                // inside split rows. The caret never lands on a record, and Tab lands
                // where the model says. Tables/media restart the walk in a lane.
                const restart = function() {
                    for (let i = rand(blockModel.count), k = 0; k < blockModel.count; ++k, i = (i + 1) % blockModel.count)
                        if (blockModel.typeForRow(i) === 10) { cursor.setCaret(blockModel.nextLeaf(i), 0); return }
                }
                const ft = blockModel.typeForRow(cursor.focusRow)
                if (phaseStep === 0 || ft === 3 || ft === 6) restart()
                else {
                    const before = cursor.focusRow
                    switch (rand(6)) {
                    case 0: root.navDown(false); break
                    case 1: root.navUp(false); break
                    case 2: cursor.setCaret(before, blockModel.contentForRow(before).length); root.navRight(false); break
                    case 3: cursor.setCaret(before, 0); root.navLeft(false); break
                    case 4: {
                        const want = blockModel.tabTarget(before, false)
                        root.tabKey(false)
                        if (want >= 0 && cursor.focusRow !== want)
                            fail("Tab from " + before + " landed on " + cursor.focusRow + ", model says " + want)
                        break
                    }
                    case 5: root.tabKey(true); break
                    }
                    ++checks
                    if (blockModel.typeForRow(cursor.focusRow) === 10) fail("the caret landed on a record at " + cursor.focusRow)
                }
                if (cursor.focusRow >= 0) root.ensureVisible(cursor.focusRow)
                next(phaseStep >= 400)
            } else if (phase === 7) {
                // Editing across lanes (SR-3 S6b): a random walk of Backspace, Delete, typing
                // and Enter at lane edges. The structure stays valid after every edit and
                // the caret never sits on a record.
                const ft = blockModel.typeForRow(cursor.focusRow)
                if (phaseStep === 0 || blockModel.laneForRow(cursor.focusRow) < 0 && rand(4) === 0) {
                    for (let i = rand(blockModel.count), k = 0; k < blockModel.count; ++k, i = (i + 1) % blockModel.count)
                        if (blockModel.typeForRow(i) === 10) { cursor.setCaret(blockModel.nextLeaf(i), 0); break }
                } else {
                    const row = cursor.focusRow
                    const len = blockModel.contentForRow(row).length
                    switch (rand(7)) {
                    case 0: cursor.setCaret(row, 0); cursor.backspace(false); break
                    case 1: cursor.setCaret(row, len); cursor.forwardDelete(false); break
                    case 2: cursor.backspace(true); break                        // a held key: never structural
                    case 3: cursor.insertChar("x"); break
                    case 4: cursor.splitLine(false); break
                    case 5: {                                                     // empty the block, then Backspace
                        if (ft !== 3 && ft !== 6 && len > 0) blockModel.setContent(row, "")
                        cursor.setCaret(row, 0); cursor.backspace(false); break
                    }
                    case 6: {                                                     // select across a split row and delete
                        const last = blockModel.splitRowLast(row)
                        if (last >= 0) { cursor.anchorRow = row; cursor.anchorCol = 0; cursor.focusRow = last; cursor.focusCol = 0; cursor.deleteSelection() }
                        break
                    }
                    }
                    checks += 2
                    if (!blockModel.structureValid()) fail("the split-row structure broke after an edit at " + row)
                    if (blockModel.typeForRow(cursor.focusRow) === 10) fail("the caret landed on a record at " + cursor.focusRow)
                }
                if (cursor.focusRow >= 0) root.ensureVisible(cursor.focusRow)
                next(phaseStep >= 300)
            } else if (phase === 8) {
                // Lane gestures (SR-3 S7b), through the same begin/update/commit functions the
                // mouse uses: pull a lane out of a block in view, drag a divider (with its
                // chain, or alone). The structure stays valid after every gesture.
                const row = Math.min(blockModel.count - 1, root.firstVisible + 1 + rand(4))
                const t = blockModel.typeForRow(row)
                if (phaseStep % 2 === 0 && t !== 10) {
                    const g = root.laneOf(row)
                    if (g.w >= 2 * blockModel.minLaneWidth + blockModel.laneGap) {
                        const before = blockModel.count
                        const side = rand(2)
                        root.beginPull(row, side, side === 1 ? g.x : g.x + g.w)
                        root.updatePull(g.x + g.w * (0.3 + 0.1 * rand(5)))
                        root.commitPull()
                        ++checks
                        if (blockModel.count <= before) fail("pulling a lane out of row " + row + " made nothing")
                    }
                } else {
                    const rec = blockModel.splitRowOf(row)
                    if (rec >= 0 && blockModel.laneCount(rec) >= 2) {
                        root.beginDividerDrag(rec, 0, blockModel.dividerX(rec, 0), rand(2) === 0)
                        root.updateDividerDrag(blockModel.dividerX(rec, 0) + (rand(2) ? 60 : -60))
                        root.commitDividerDrag()
                    }
                }
                ++checks
                if (!blockModel.structureValid()) fail("the structure broke after a lane gesture near row " + row)
                if (phaseStep % 6 === 5) flick.contentY = Math.min(maxY, flick.contentY + flick.height * 0.7)
                next(phaseStep >= 120)
            } else if (phase === 9) {
                // Tables (SR-4 S5): make tables in view, type into cells (auto widths move the
                // columns), set manual widths, change column kinds, sort, insert columns, undo —
                // verify() keeps every cell at its column; the structure stays valid.
                const at = Math.min(blockModel.count - 1, root.firstVisible + 1)
                let head = -1
                for (let i = root.firstVisible; i < Math.min(blockModel.count, root.firstVisible + 80) && head < 0; ++i)
                    if (blockModel.headerCount(i) > 0) head = i
                switch (phaseStep % 8) {
                case 0: if (blockModel.laneForRow(at) < 0 && blockModel.typeForRow(at) !== 10) blockModel.insertTableRows(at, 4, 3); break
                case 1: if (head >= 0) blockModel.setContent(blockModel.tableCellAt(head, 1 + rand(3), rand(3)), "cell ".repeat(1 + rand(12))); break
                case 2: if (head >= 0) blockModel.setTableColumnWidth(head, rand(3), rand(2) ? 0 : 220 + rand(300)); break
                case 3: if (head >= 0) blockModel.tableSetColumnKind(head, rand(3), rand(3)); break
                case 4: if (head >= 0) blockModel.tableSortByColumn(head, rand(3), rand(2) === 0); break
                case 5: if (head >= 0) blockModel.tableInsertColumn(head, rand(3)); break
                case 6: if (rand(3) === 0) blockModel.undo(); break
                case 7: flick.contentY = Math.min(maxY, flick.contentY + flick.height * 0.6); break
                }
                ++checks
                if (!blockModel.structureValid()) fail("the structure broke after a table op near row " + at)
                if (phaseStep === 12 || phaseStep === 150) {   // inspection artifacts next to the fixture
                    const arg = Qt.application.arguments.filter(function(a) { return a.indexOf("--pool-probe=") === 0 })[0]
                    const dir = arg.substring("--pool-probe=".length).replace(/[^\/]*$/, "")
                    flick.grabToImage(function(res) { res.saveToFile(dir + "tables-" + step + ".png") })
                }
                next(phaseStep >= 160)
            } else if (phase === 10) {
                if (phaseStep === 0) flick.contentY = 0   // sweep the whole document with tables present
                else flick.contentY = Math.min(maxY, flick.contentY + flick.height * 0.37)
                // Every third step sideways (the frozen column), then back.
                if (holdX > 0) { --holdX; next(false); return }
                flick.contentX = phaseStep % 3 === 2 ? Math.max(0, flick.contentWidth - flick.width) : 0
                next(phaseStep > 0 && flick.contentY >= maxY)
            } else if (phase === 11) {
                // Table keys (SR-4 S6a), through the functions the key handler calls: Enter walks down
                // a column (appending at the end, exiting on an empty last body row), Tab and
                // Shift+Tab walk the cells (appending past the last), ⌘Enter inserts a row below.
                if (phaseStep === 0) flick.contentX = 0
                if (phaseStep === 0 || blockModel.tableHeadOf(cursor.focusRow) < 0) {
                    let head = -1
                    for (let i = rand(blockModel.count), k = 0; k < blockModel.count && head < 0; ++k, i = (i + 1) % blockModel.count)
                        if (blockModel.headerCount(i) > 0) head = i
                    if (head >= 0) root.landInCell(head, 0, 0)
                } else {
                    const row = cursor.focusRow, head = blockModel.tableHeadOf(row)
                    const r = blockModel.tableRowOf(row), rows = blockModel.tableRowCount(head)
                    switch (rand(14)) {
                    case 13: {   // S9b: the grid frame shows one table — every visible delegate is inside it
                        root.setActiveTab(blockModel.idForRow(head))
                        if (root.boardMode) root.showGridView()
                        let outside = 0, inside = 0
                        for (let i = 0; i < pool.count; ++i) {
                            const c = pool.itemAt(i)
                            if (!c || !c.active || !c.visible) continue
                            if (c.logicalRow >= root.frameLo && c.logicalRow <= root.frameHi) ++inside; else ++outside
                        }
                        checks += 2
                        if (root.frameLo !== head || outside > 0) fail("the grid frame of table " + head + " showed " + outside + " blocks outside it")
                        if (flick.contentY < root.frameTop - Theme.dim.toolStripHeight - 1) fail("the grid frame scrolled above its table (" + flick.contentY + " < " + root.frameTop + ")")
                        root.setActiveTab("")
                        break
                    }
                    case 12: {   // SR-0 §4.9: a row range reaching into a table from above takes the whole table
                        const recs = blockModel.tableRecords(head), last = blockModel.splitRowLast(recs[recs.length - 1])
                        const above = head - 1
                        if (above < 0 || blockModel.laneForRow(above) >= 0 || blockModel.typeForRow(above) === 10) break
                        const mid = blockModel.tableCellAt(head, Math.floor(recs.length / 2), 0)
                        if (mid < 0) break
                        cursor.setCaret(above, 0)
                        cursor.move(mid, 0, true)
                        ++checks
                        if (cursor.loRow !== above || cursor.hiRow !== last)
                            fail("a range from row " + above + " into table " + head + " spans " + cursor.loRow + "–" + cursor.hiRow
                                 + ", not the whole table to " + last)
                        cursor.setCaret(mid, 0)
                        break
                    }
                    case 9: {   // S7b: grip picks — a row span (Shift); Delete clears its cells, the rows stay
                        const rowsBefore = blockModel.tableRowCount(head)
                        const a = rand(rowsBefore), z = rand(rowsBefore)
                        root.tableGripClick(head, "row", a, 0)
                        root.tableGripClick(head, "row", z, Qt.ShiftModifier)
                        checks += 2
                        if (!root.tableSetLive() || root.tableSet.items.length !== Math.abs(a - z) + 1)
                            fail("a Shift grip pick in table " + head + " from row " + a + " to " + z + " didn't make the span")
                        root.clearGridSet()
                        if (blockModel.headerCount(head) <= 0 || blockModel.tableRowCount(head) !== rowsBefore)
                            fail("clearing a row set in table " + head + " changed its rows")
                        {   // the grip bands: beside a row in the left margin, above a column in the first row's pocket
                            const recs = blockModel.tableRecords(head), rr = rand(recs.length), rec = recs[rr]
                            const rowY = blockModel.yForRow(rec) + blockModel.tablePadTop(rec) + 4
                            const gRow = root.tableGripAt(root.tableX(head) - 10, rowY)
                            const colY = blockModel.yForRow(head) + blockModel.tablePadTop(head) - 10
                            const gCol = root.tableGripAt(root.tableX(head) + blockModel.tableColumnLeft(head, 0) + 4, colY)
                            checks += 2
                            if (!gRow || gRow.kind !== "row" || gRow.head !== head || gRow.index !== rr)
                                fail("the row grip beside row " + rr + " of table " + head + " at y " + rowY + " answered " + JSON.stringify(gRow))
                            if (!gCol || gCol.kind !== "col" || gCol.head !== head || gCol.index !== 0)
                                fail("the column grip above column 0 of table " + head + " at y " + colY + " answered " + JSON.stringify(gCol)
                                     + " (padTop " + blockModel.tablePadTop(head) + ", rowForY " + blockModel.rowForY(colY) + ")")
                        }
                        const b = blockModel.tableCellAt(head, 0, 0)   // a drop over cell (0,0) targets it
                        if (b >= 0) {
                            const pt = root.tableCellAtPoint(root.tableX(head) + blockModel.tableColumnLeft(head, 0) + 4, blockModel.yForRow(b) + 2)
                            ++checks
                            if (!pt || pt.head !== head || pt.r !== 0 || pt.c !== 0) fail("the drop target over cell (0,0) of table " + head + " missed it")
                        }
                        break
                    }
                    case 10: {   // S7b: a column grip drag moves the column, its header text with it
                        const cols = blockModel.tableColumnCount(head)
                        const from = rand(cols), gap = rand(cols + 1)
                        if (cols < 2 || from >= blockModel.tableCellCount(head, 0)) break
                        const txt = blockModel.tableCellText(head, 0, from)
                        root.tableGripPressHead = head; root.tableGripPressIndex = from
                        root.tableColDragging = true; root.tableColGap = gap
                        root.commitGridColDrag()
                        const to = gap > from ? gap - 1 : gap
                        checks += 2
                        if (blockModel.tableColumnCount(head) !== cols) fail("a column grip drag in table " + head + " changed its column count")
                        if (blockModel.tableCellText(head, 0, to) !== txt)
                            fail("column " + from + " of table " + head + " didn't land at " + to)
                        break
                    }
                    case 11: {   // S7b: a row handle's drag — a header row carries its table, and no table swallows another's rows
                        const recs = blockModel.tableRecords(head)
                        const rec = recs[rand(recs.length)], header = blockModel.isHeaderRow(rec)
                        const id = blockModel.idForRow(head), mine = {}, others = {}
                        let tables = 0
                        for (let i = 0; i < blockModel.count; ++i) {
                            if (blockModel.headerCount(i) > 0) ++tables
                            if (blockModel.typeForRow(i) !== 10) continue
                            const h = blockModel.tableHeadOf(i)
                            if (h === head) mine[blockModel.idForRow(i)] = true
                            else if (h >= 0) others[blockModel.idForRow(i)] = true
                        }
                        const run = root.dragRunFor(rec)
                        root.blockDragRow = run[0]; root.blockDragCount = run[1]; root.blockDragging = true
                        root.aimBlockDrag(root.leftEdge + 10, rand(Math.max(1, Math.floor(flick.contentHeight))))
                        root.commitBlockDrag()
                        let after = 0, moved = -1
                        for (let i = 0; i < blockModel.count; ++i) {
                            if (blockModel.headerCount(i) > 0) ++after
                            if (blockModel.idForRow(i) === id) moved = i
                        }
                        checks += 2
                        if (after !== tables) fail("a row handle drag from table " + head + " changed the table count " + tables + " → " + after)
                        if (header) {
                            const now = moved >= 0 ? blockModel.tableRecords(moved) : []
                            let swallowed = moved < 0
                            for (let k = 0; k < now.length; ++k) if (others[blockModel.idForRow(now[k])]) swallowed = true
                            if (swallowed || now.length < recs.length)
                                fail("a header row drag broke table " + head + ": " + recs.length + " rows → " + now.length + (swallowed ? ", took another table's rows" : ""))
                        }
                        break
                    }
                    case 7: {   // A6: drag a column's right border — its px width, never under 48
                        const c = blockModel.tableColumnOf(row)
                        const edge = root.tableShift(head) + blockModel.tableColumnLeft(head, c) + blockModel.tableColumnWidth(head, c)
                        root.beginDividerDrag(blockModel.splitRowOf(row), c, edge, false)
                        root.updateDividerDrag(edge + (rand(2) ? 60 : -500))
                        root.commitDividerDrag()
                        ++checks
                        if (blockModel.tableColumnWidth(head, c) < 48) fail("column " + c + " of table " + head + " went under 48 px")
                        break
                    }
                    case 8: {   // A6: a pull from a cell's right edge adds a column table-wide
                        const colsBefore = blockModel.tableColumnCount(head)
                        if (colsBefore >= 8) break                       // keep the walk's tables representative
                        const g = root.laneOf(row)
                        root.beginPull(row, 0, g.x + g.w)
                        root.updatePull(g.x + g.w * 0.3)
                        root.commitPull()
                        ++checks
                        if (colsBefore < 63 && blockModel.tableColumnCount(head) !== colsBefore + 1)
                            fail("a pull in table " + head + " made " + blockModel.tableColumnCount(head) + " columns from " + colsBefore)
                        break
                    }
                    case 6: {   // A7: select a cell rectangle and delete it — the cells clear, the rows stay
                        const rowsBefore = blockModel.tableRowCount(head)
                        const a = blockModel.tableCellAt(head, 0, 0)
                        const z = blockModel.tableCellAt(head, rowsBefore - 1, Math.max(0, blockModel.tableCellCount(head, rowsBefore - 1) - 1))
                        if (a >= 0 && z >= 0 && a !== z) {
                            cursor.setCaret(a, 0)
                            cursor.move(z, blockModel.contentForRow(z).length, true)
                            root.selObject = null
                            cursor.deleteSelection()
                            ++checks
                            if (blockModel.tableHeadOf(cursor.focusRow) !== head || blockModel.tableRowCount(head) !== rowsBefore)
                                fail("deleting a cell rectangle in table " + head + " changed its rows")
                        }
                        break
                    }
                    case 5: {   // ⌘A climbs to the whole table (block → cell → table), then collapse
                        cursor.setCaret(row, 0)
                        for (let k = 0; k < 3 && !root.selectionIsTable(); ++k) root.selectAllLadder()
                        ++checks
                        if (!root.selectionIsTable()) fail("⌘A from table cell " + row + " never selected the table")
                        cursor.setCaret(row, 0)
                        break
                    }
                    case 0: case 1:
                        root.tableEnter(false)
                        ++checks
                        if (r < rows - 1 && blockModel.tableRowOf(cursor.focusRow) !== r + 1)
                            fail("Enter in table row " + r + " landed in row " + blockModel.tableRowOf(cursor.focusRow))
                        break
                    case 2: root.tabKey(false); break
                    case 3: root.tabKey(true); break
                    case 4: root.tableInsertRowBelow(); break
                    }
                }
                checks += 2
                if (!blockModel.structureValid()) fail("the structure broke after a table key near row " + cursor.focusRow)
                if (blockModel.typeForRow(cursor.focusRow) === 10) fail("the caret landed on a record at " + cursor.focusRow)
                if (cursor.focusRow >= 0) root.ensureVisible(cursor.focusRow)
                next(phaseStep >= 250)
            } else {
                running = false
                console.log("POOL-PROBE DONE steps", step, "checks", checks, "fails", fails,
                            "maxVisibleRows", maxRows, "pool", root.poolSize, "blocks", blockModel.count)
                Qt.exit(Math.min(fails, 100))
            }
        }
    }

    // --- HUD telemetry (same surface as the other arms) ---
    // Load-bearing revision read (reactivity rule 1e): rowForY() is a Q_INVOKABLE,
    // so QML can't see that it depends on the Fenwick heights / row count. Without
    // the contentRevision dep the visible-row WINDOW (firstVisible→firstRow) goes
    // stale after any structural edit (insert/remove/split/undo/redo) until a
    // scroll moves contentY — the delegate pool then renders the wrong rows
    // (blocks disappear, undo/redo layout corruption). contentRevision (NOT
    // layoutRevision) is used deliberately: rendering bumps only layoutRevision
    // (height settle), so depending on it here would loop window→pool→measure→
    // window; pure height settles are already handled by onHeightSettled's
    // contentY nudge below.
    readonly property int firstVisible: (blockModel.contentRevision,
                                         blockModel.rowForY(flick.contentY))
    readonly property int lastVisible: (blockModel.contentRevision,
                                        Math.min(blockModel.count - 1,
                                                 blockModel.rowForY(flick.contentY + flick.height - 1)))
    // EVERY video row in the document — the per-video transport toolbars are all
    // built up front (on load), NOT lazily as rows scroll into view, so scrolling
    // never creates/destroys a toolbar (zero flicker; the scrubber never resets).
    // Each toolbar just repositions (its y binding) and toggles visibility as it
    // enters/leaves the viewport. Held as a STABLE array recomputed only when the
    // SET changes (a video inserted/deleted/retyped — contentChangedSpike), so
    // ordinary text edits don't churn the Repeater.
    property var allVideoRows: []
    function _recomputeVideoRows() {
        var out = []
        var n = blockModel.count
        for (var r = 0; r < n; ++r)
            if (blockModel.typeForRow(r) === 3 && blockModel.mediaKind(r) === "video")
                out.push(r)
        var cur = allVideoRows
        if (cur.length === out.length) {
            var same = true
            for (var i = 0; i < out.length; ++i) if (cur[i] !== out[i]) { same = false; break }
            if (same) return            // unchanged set → keep the same array (no churn)
        }
        allVideoRows = out
    }
    property var allPdfRows: []
    function _recomputePdfRows() {
        var out = []
        var n = blockModel.count
        for (var r = 0; r < n; ++r)
            if (blockModel.typeForRow(r) === 3 && blockModel.mediaKind(r) === "pdf")
                out.push(r)
        var cur = allPdfRows
        if (cur.length === out.length) {
            var same = true
            for (var i = 0; i < out.length; ++i) if (cur[i] !== out[i]) { same = false; break }
            if (same) return
        }
        allPdfRows = out
    }
    Connections {
        target: blockModel
        function onContentChangedSpike() {     // insert/delete/type change
            root._recomputeVideoRows(); root._recomputePdfRows()
            root._reconcileVideoPlayingRow()
        }
    }
    // Structural edits shift row indices, and videoPlayingRow is imperative
    // state (the studio self-heals by block id; the inline player can't — a
    // pooled row index is all it has). If the playing block moved, re-point to
    // its new row (closest path match, so a doc with the same clip twice picks
    // the right one); if it's gone (deleted, or undone away), tear the player
    // down instead of painting the surface over whatever block shifted into
    // its old row.
    function _reconcileVideoPlayingRow() {
        if (videoPlayingRow < 0) return
        // Compare by playback SOURCE (what the decoder was opened with — a
        // path or a stream spec); pure lookups, nothing extracts.
        if (blockModel.mediaPlaybackSource(videoPlayingRow) === _videoPlayingPath) return
        var best = -1
        for (var i = 0; i < allVideoRows.length; ++i) {
            var r = allVideoRows[i]
            if (blockModel.mediaPlaybackSource(r) !== _videoPlayingPath) continue
            if (best < 0 || Math.abs(r - videoPlayingRow) < Math.abs(best - videoPlayingRow)) best = r
        }
        if (best >= 0) videoPlayingRow = best
        else stopVideo()
    }
    // Switching NOTES re-points blockModel but emits no contentChangedSpike, so the
    // always-built video/pdf transport toolbars would keep the previous note's rows
    // (lingering toolbars) until the new note is edited. Force a rebuild for the
    // now-active document. Clear first so the rebuild runs even if the row indices
    // coincide — the underlying blocks (and their media) are a different document.
    Connections {
        target: docs
        function onActiveChanged() {
            // Tear down the live inline player too: videoPlayingRow is imperative
            // state (not a blockModel binding), so without this the previous note's
            // video surface keeps painting over the same row index in the new note
            // until a scroll/interaction pushes it out of the visible range.
            root.stopVideo()
            Qt.callLater(function() {
                root.allVideoRows = []; root.allPdfRows = []
                root._recomputeVideoRows(); root._recomputePdfRows()
            })
        }
    }

    // SR-3: the pool renders the blocks in view plus an overscan band, straight from
    // the model's two-level index — a split row contributes its record and only its
    // lanes' visible blocks. layoutRevision is safe here since SR-2: the slot table
    // is stable, so a height settle only hands ENTERING blocks to free slots (no
    // re-render, no re-measure), and measure-back is asynchronous anyway.
    readonly property real overscanPx: Math.max(200, flick.height * 0.5)
    // The overscan is half a screen of PIXELS each side — ~15 paragraphs, but hundreds of cells
    // where a table is dense (2026-09-15 walk: a table entering the band rebound ~700 delegates
    // in one frame at 0.3 ms each). Cap it by ROWS beyond the viewport: still screens of prose,
    // half a screen of table.
    readonly property int overscanRows: Qt.application.arguments.indexOf("--no-overscan-cap") >= 0 ? 1000000 : 60   // the switch: an A/B on the same build
    readonly property var poolRows: {
        const dep = blockModel.contentRevision + blockModel.layoutRevision
        const y0 = flick.contentY, y1 = flick.contentY + flick.height
        const all = blockModel.visibleBlocks(Math.max(0, y0 - overscanPx), y1 + overscanPx)
        const view = blockModel.visibleBlocks(y0, y1)
        if (view.length === 0 || all.length <= view.length + 2 * overscanRows) return all
        const first = all.indexOf(view[0]), last = all.indexOf(view[view.length - 1])
        if (first < 0 || last < 0) return all
        return all.slice(Math.max(0, first - overscanRows), Math.min(all.length, last + 1 + overscanRows))
    }
    // The pool's SIZE is a ListModel that only grows, in chunks of 16 (and shrinks only when the
    // document has fewer blocks than slots). An int model on the pool Repeater regenerated EVERY
    // delegate whenever the count changed, and with a table in view (records + cells) the visible
    // count changed on every scroll step — the 2026-09-14 big-table crawl.
    readonly property int poolNeed: Math.min(blockModel.count,
        Math.max(poolRows.length, Math.ceil(root.height / 38) + 2 * overscan + 4))
    readonly property int poolSize: poolModel.count
    readonly property int delegateCount: poolSize
    // Instrument (2026-09-15 walk): `--perf-log` counts delegate rebinds per burst and the
    // synchronous milliseconds from the first rebind to the end of the event-loop turn — the
    // number a table entering the view should shrink.
    readonly property bool perfLog: Qt.application.arguments.indexOf("--perf-log") >= 0
    readonly property bool noSticky: Qt.application.arguments.indexOf("--no-sticky") >= 0   // A/B: no sticky header / frozen column
    readonly property bool noRail: Qt.application.arguments.indexOf("--no-rail") >= 0         // A/B: no rail numbers / row rules
    property int rebindBurst: 0
    property int rebindTotal: 0
    property int imagesReady: 0        // MediaBlock counts decoded images here (perfLog only)
    // The frame monitor: every frame over 25 ms, with what changed in it — so a hitch names its
    // cause instead of being sampled for.
    FrameAnimation {
        running: root.perfLog
        property real lastY: 0
        property int lastPool: 0
        property int lastRebinds: 0
        property int lastImages: 0
        property int lastSticky: -1
        onTriggered: {
            const ms = frameTime * 1000
            const sticky = stickyHeader.stHead
            if (ms > 25)
                console.log("[frame]", Math.round(ms), "ms  dy", Math.round(flick.contentY - lastY), " pool +" + (poolModel.count - lastPool),
                            " rebinds", root.rebindTotal - lastRebinds, " images", root.imagesReady - lastImages,
                            " sticky", lastSticky, "→", sticky, " y", Math.round(flick.contentY))
            lastY = flick.contentY; lastPool = poolModel.count; lastRebinds = root.rebindTotal
            lastImages = root.imagesReady; lastSticky = sticky
        }
    }
    property real rebindBurstStart: 0
    property real rebindBurstLast: 0
    function noteRebind() {
        if (rebindBurst === 0) rebindBurstStart = Date.now()
        rebindBurst++; rebindTotal++
        rebindBurstLast = Date.now()
        rebindTimer.restart()
    }
    Timer {
        id: rebindTimer; interval: 0
        onTriggered: {   // cascade = first → last rebind (the delegates' own work); turn = to the end of the event-loop turn
            console.log("[perf] rebind burst:", root.rebindBurst, "delegates, cascade", root.rebindBurstLast - root.rebindBurstStart,
                        "ms, turn", Date.now() - root.rebindBurstStart, "ms, pool", poolModel.count, "at y", Math.round(flick.contentY))
            root.rebindBurst = 0
        }
    }
    ListModel { id: poolModel }
    function sizePool() {
        const want = Math.min(blockModel.count, Math.ceil(poolNeed / 8) * 8)
        while (poolModel.count < want) poolModel.append({ slot: poolModel.count })
        while (poolModel.count > want && poolModel.count > blockModel.count) poolModel.remove(poolModel.count - 1)
    }
    onPoolNeedChanged: sizePool()
    // Pre-warm (2026-09-15 walk: the first scroll into a dense table hitched while slots were
    // created mid-flick): while the view rests, grow the pool a couple of slots per tick up to a
    // few screens of small cells, so a table is met with delegates already built.
    // Deeper and faster (2026-09-15 A/B on the user's document): the worst frames were the ones
    // where the pool GREW mid-flick — ~0.8 ms per delegate created on top of ~0.13 ms per rebind —
    // and the old 2-per-40 ms warm-up never got ahead of the scroll. Target: eight screens of small
    // cells (never fewer than 480), four slots per tick while the view rests.
    readonly property int poolPrewarm: Math.min(blockModel.count, Math.max(480, 8 * (Math.ceil(root.height / 38) + 2 * overscan + 4)))
    Timer {
        interval: 20; repeat: true
        running: blockModel.documentOpen && !flick.moving && !flick.dragging && poolModel.count < root.poolPrewarm
        onTriggered: { for (let k = 0; k < 4 && poolModel.count < root.poolPrewarm; ++k) poolModel.append({ slot: poolModel.count }) }
    }
    // Which block each pool slot renders. Blocks that stay in view keep their
    // delegate. sync RETURNS the revision and runs inside this binding, so everything
    // reading slotRev before rowForSlot() sees the updated table.
    readonly property int slotRev: viewSlots.sync(poolRows, poolSize)
    // BlockView (the extracted block renderer) reads the editor's controllers
    // through these — ids don't cross file boundaries.
    readonly property var cursorObj: cursor
    readonly property var flickItem: flick
    readonly property real barFraction: flick.contentHeight > flick.height
        ? flick.contentY / (flick.contentHeight - flick.height) : 0
    readonly property real trueFraction: barFraction
    readonly property int caretRow: cursor.focusRow
    readonly property bool hasSelection: cursor.hasSel
    readonly property string selSummary: cursor.hasSel
        ? ("r" + cursor.loRow + ":" + cursor.loCol + " → r" + cursor.hiRow + ":" + cursor.hiCol
           + "  (" + (cursor.hiRow - cursor.loRow + 1) + " blocks)")
        : ("caret r" + cursor.focusRow + ":" + cursor.focusCol)

    function jumpToEnd() { flick.contentY = Math.max(0, flick.contentHeight - flick.height) }
    function jumpToStart() { flick.contentY = 0 }
    property alias scrollY: flick.contentY
    readonly property real maxScrollY: Math.max(0, flick.contentHeight - flick.height)

    Flickable {
        id: flick
        visible: root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0   // hidden in a full-frame tab
                 && !(root.activeGridId !== "" && root.boardMode)                 // a table's board covers it
        anchors.fill: parent
        // The grid frame's clamp (S9b): the table's top sits under the tab toolbar, its bottom at the end.
        topMargin: root.frameLo >= 0 ? -(root.frameTop - Theme.dim.toolStripHeight) : 0
        bottomMargin: root.frameLo >= 0 ? -Math.max(0, contentHeight - root.frameBottom) : 0
        // In ink mode the content is wider than the viewport (locked page +
        // margins) and pans natively; contentSpan == width otherwise, so this
        // is a no-op outside the mode.
        contentWidth: root.contentSpan
        contentHeight: blockModel.totalHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // A resize (or leaving ink mode) can strand contentX past the new
        // clamp range — snap back (the kanban board does the same).
        // Deferred a tick: a width change can land INSIDE a scroll's binding
        // cascade (a recycled delegate's code block reports its width
        // synchronously → maxContentWidth → contentWidth), and returnToBounds'
        // fixup nudges contentY there — re-entering firstVisible (binding loop).
        onWidthChanged: Qt.callLater(returnToBounds)
        onContentWidthChanged: Qt.callLater(returnToBounds)

        Connections {
            target: blockModel
            function onHeightSettled(row, delta) { if (row < root.firstVisible) flick.contentY += delta }
        }

        // Gutter tints while the PageRuler drags a width: the marginalia
        // zones travel visibly with the edge (transient drag feedback).
        Rectangle {
            visible: root.widthDragging
            z: -1
            x: root.sheetLeft; width: root.inkGutter
            y: flick.contentY; height: flick.height
            color: Theme.colors.bgAlt2
        }
        Rectangle {
            visible: root.widthDragging
            z: -1
            x: root.leftEdge + root.pageWidth; width: root.inkGutter
            y: flick.contentY; height: flick.height
            color: Theme.colors.bgAlt2
        }

        Item {        // "you are here" — the FOCUSED block's full row, page and
                      // margins alike. The one fill on the field, and it MEANS
                      // something (the zebra's parity flipped on every insert;
                      // this doesn't). DUAL TONE since the sheet tint (ruling
                      // 2026-08-20): the highlight steps ONE rung above
                      // whatever ground it crosses — bgAlt2 over the sheet,
                      // bgAlt over the desk beyond it — so the row still runs
                      // the full field but the sheet shape stays legible
                      // through it. Same token ladder, no new colour.
            visible: cursor.focusRow >= 0 && cursor.focusRow < blockModel.count
            z: -1
            x: 0
            width: Math.max(flick.width, root.contentSpan)
            // In a lane, the whole split row is "here" — its record spans the row.
            readonly property int fillRow: (blockModel.contentRevision,
                blockModel.splitRowOf(cursor.focusRow) >= 0 ? blockModel.splitRowOf(cursor.focusRow)
                                                            : cursor.focusRow)
            y: (blockModel.layoutRevision, blockModel.yForRow(fillRow))
            // layoutRevision dep (rule 1): without it the fill only re-evaluates
            // on focusRow change — a "# " conversion's height settle wouldn't
            // reach it until Return moved the caret.
            height: Math.max(16, (blockModel.layoutRevision, blockModel.heightForRow(fillRow)))
            readonly property real sheetL: root.sheetLeft    // matches the sheet tint, both edges
            readonly property real sheetR: root.sheetRight
            Rectangle {
                x: 0; width: Math.max(0, parent.sheetL)
                height: parent.height
                color: Theme.colors.bgAlt
            }
            Rectangle {
                x: parent.sheetL; width: Math.max(0, Math.min(parent.width, parent.sheetR) - parent.sheetL)
                height: parent.height
                color: Theme.colors.bgAlt2
            }
            Rectangle {
                x: parent.sheetR
                width: Math.max(0, parent.width - parent.sheetR)
                height: parent.height
                color: Theme.colors.bgAlt
            }
        }
        // (The block rules are gone — 2026-09-15 walk: the user dropped them for scroll smoothness.)

        Repeater {
            id: pool
            model: poolModel   // grows by insertion: never a full regenerate
            delegate: BlockView {
                required property int index
                editor: root
                logicalRow: (root.slotRev, viewSlots.rowForSlot(index))
            }
        }

        // T3 (SR-4 S5c): once the page scrolls sideways past a table's left edge, a mirror of the
        // table's first column stays pinned at the viewport's left for every table row in view.
        // Under the sticky header (z 2.5 < 3); the pinned corner cell sits over both.
        Item {
            id: frozenColumn
            // `computed` is a fresh array per scroll frame; `rows` (the Repeater's model) only changes when
            // the visible set does — a model reset per frame rebuilt every mirror cell (the big-table crawl).
            property var rows: []
            onComputedChanged: {
                const a = computed, b = rows
                let same = a.length === b.length
                for (let i = 0; same && i < a.length; ++i)
                    same = a[i].head === b[i].head && a[i].gr === b[i].gr && a[i].y === b[i].y && a[i].h === b[i].h
                        && a[i].w === b[i].w && a[i].tw === b[i].tw && a[i].header === b[i].header
                if (!same) rows = a
            }
            readonly property var computed: {
                const dep = blockModel.layoutRevision + blockModel.contentRevision + flick.contentY + flick.contentX
                if (root.noSticky) return []
                const out = []
                const inView = blockModel.visibleBlocks(flick.contentY, flick.contentY + flick.height)
                for (let i = 0; i < inView.length; ++i) {
                    const r = inView[i]
                    if (blockModel.typeForRow(r) !== 10) continue
                    const head = blockModel.tableHeadOf(r)
                    if (head < 0) continue
                    const tw = blockModel.tableWidth(head), tx = root.tableX(head)
                    if (flick.contentX <= tx + 1) continue                // the table's left edge is still in view
                    if (tx + tw <= flick.contentX) continue               // the whole table is scrolled away
                    const padTop = blockModel.tablePadTop(r)
                    out.push({ head: head, gr: blockModel.tableRowOf(r), header: blockModel.isHeaderRow(r),
                               y: blockModel.yForRow(r) + padTop,
                               h: blockModel.heightForRow(r) - padTop - blockModel.tablePadBottom(r),
                               w: blockModel.tableColumnWidth(head, 0), tw: tw, tx: tx })
                }
                return out
            }
            x: flick.contentX
            z: 2.5
            Repeater {
                model: frozenColumn.rows
                delegate: Rectangle {
                    required property var modelData
                    readonly property string bg: (blockModel.contentRevision, blockModel.tableCellBg(modelData.head, modelData.gr, 0))
                    readonly property string fg: (blockModel.contentRevision, blockModel.tableCellFg(modelData.head, modelData.gr, 0))
                    // Pushed off to the left as the table's right edge arrives (never over its last column).
                    x: Math.min(0, modelData.tx + modelData.tw - modelData.w - flick.contentX)
                    y: modelData.y
                    width: modelData.w
                    height: modelData.h
                    clip: true
                    color: bg !== "" ? bg : modelData.header ? Theme.colors.surfaceHover : Theme.colors.surface
                    Rectangle { width: parent.width; height: 1; color: Theme.colors.border }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border }
                    Rectangle { width: 1; height: parent.height; color: Theme.colors.border }
                    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.colors.border }
                    Text {
                        x: 8; y: 6
                        width: parent.width - 16
                        text: (blockModel.contentRevision, blockModel.tableCellText(modelData.head, modelData.gr, 0))
                        color: fg !== "" ? fg : Theme.colors.text
                        font.family: Theme.font.body; font.pixelSize: Theme.font.sizeBody; font.bold: modelData.header
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
        Rectangle {   // the corner: the header's first cell, pinned at the top and the left
            id: frozenCorner
            readonly property int head: stickyHeader.stHead
            visible: stickyHeader.visible && head >= 0 && flick.contentX > root.tableX(head) + 1
                     && root.tableX(head) + (blockModel.layoutRevision, blockModel.tableWidth(head)) > flick.contentX
            x: flick.contentX + (head >= 0 ? Math.min(0, root.tableX(head) + (blockModel.layoutRevision, blockModel.tableWidth(head))
                                                         - width - flick.contentX) : 0)
            y: stickyHeader.y
            z: 4
            width: head >= 0 ? (blockModel.layoutRevision, blockModel.tableColumnWidth(head, 0)) : 0
            height: stickyHeader.headerH
            clip: true
            color: Theme.colors.surfaceHover
            Rectangle { width: parent.width; height: 1; color: Theme.colors.border }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border }
            Rectangle { width: 1; height: parent.height; color: Theme.colors.border }
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.colors.border }
            Text {
                x: 8; y: 6
                width: parent.width - 16
                text: frozenCorner.head >= 0 ? (blockModel.contentRevision, blockModel.tableCellText(frozenCorner.head, 0, 0)) : ""
                color: Theme.colors.text
                font.family: Theme.font.body; font.pixelSize: Theme.font.sizeBody; font.bold: true
                wrapMode: Text.Wrap
            }
        }

        // T3 (SR-4 S5b): once a table's header rows scroll above the viewport while its body is
        // still on screen, a mirror of them stays pinned at the top — pushed up as the table ends.
        // Plain text in the header's styling; interaction still goes to the rows beneath.
        Item {
            id: stickyHeader
            readonly property var st: (blockModel.layoutRevision, blockModel.contentRevision,
                                       root.noSticky ? ({}) : blockModel.tableStickyAt(flick.contentY))
            readonly property bool has: st.head !== undefined
            readonly property real headerH: has ? st.headerBottom - st.headerTop : 0
            readonly property real headerTop: has ? st.headerTop : 0
            // `st` is a fresh map per scroll frame. The Repeater's model and the cells' head are STABLE
            // properties reassigned only when the sticky table or its header rows change — a model reset
            // per frame rebuilt every header cell (the big-table crawl).
            property int stHead: -1
            property var headerRows: []
            onStChanged: {
                const h = st.head !== undefined ? st.head : -1   // not `has`: it may lag st in this handler
                const rows = h >= 0 && st.headerRows !== undefined ? st.headerRows : []
                let same = h === stHead && rows.length === headerRows.length
                for (let i = 0; same && i < rows.length; ++i) same = rows[i] === headerRows[i]
                if (same) return
                stHead = h
                headerRows = rows
            }
            visible: has && flick.contentY > st.headerTop && flick.contentY < st.tableBottom - headerH
            x: 0
            y: flick.contentY + (has ? Math.min(0, st.tableBottom - headerH - flick.contentY) : 0)
            z: 3
            width: flick.contentWidth
            height: headerH
            Repeater {
                model: stickyHeader.headerRows
                delegate: Item {
                    id: stickyRow
                    required property var modelData
                    required property int index
                    readonly property int rec: modelData
                    readonly property real pad: index === 0 ? blockModel.tablePadTop(rec) : 0
                    y: (blockModel.layoutRevision, blockModel.yForRow(rec)) + pad - stickyHeader.headerTop
                    width: stickyHeader.width
                    height: (blockModel.layoutRevision, blockModel.heightForRow(rec)) - pad
                    Repeater {
                        model: (blockModel.contentRevision, blockModel.tableColumnCount(stickyHeader.stHead))
                        delegate: Rectangle {
                            required property int index
                            readonly property int head: stickyHeader.stHead
                            readonly property string bg: (blockModel.contentRevision, blockModel.tableCellBg(head, stickyRow.index, index))
                            readonly property string fg: (blockModel.contentRevision, blockModel.tableCellFg(head, stickyRow.index, index))
                            x: root.tableX(head) + (blockModel.layoutRevision, blockModel.tableColumnLeft(head, index))
                            width: (blockModel.layoutRevision, blockModel.tableColumnWidth(head, index))
                            height: stickyRow.height
                            color: bg !== "" ? bg : Theme.colors.surfaceHover
                            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.colors.border }
                            Rectangle { anchors.bottom: parent.bottom; height: 1; width: parent.width; color: Theme.colors.border }
                            Rectangle { visible: index === 0; width: 1; height: parent.height; color: Theme.colors.border }
                            Text {
                                x: 8; y: 6
                                width: parent.width - 16
                                text: (blockModel.contentRevision, blockModel.tableCellText(head, stickyRow.index, index))
                                color: fg !== "" ? fg : Theme.colors.text
                                font.family: Theme.font.body; font.pixelSize: Theme.font.sizeBody; font.bold: true
                                wrapMode: Text.Wrap
                                horizontalAlignment: {
                                    const a = (blockModel.contentRevision, blockModel.tableColAlign(head, index))
                                    return a === 1 ? Text.AlignHCenter : a === 2 ? Text.AlignRight : Text.AlignLeft
                                }
                            }
                        }
                    }
                }
            }
        }

        // Central mouse handling — click to place the caret, drag to select
        // ACROSS blocks (preventStealing keeps Flickable from hijacking the drag
        // for a flick; wheel/trackpad scroll still works since we don't take it).
        MouseArea {
            id: mouse
            width: flick.contentWidth
            height: flick.contentHeight
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            preventStealing: true
            hoverEnabled: true
            property bool overClickable: false   // over a task checkbox / table check or choice cell
            property real lastDblClickMs: 0      // triple-click detection (whole-block select)
            property int  lastDblClickRow: -1
            cursorShape: root.blockDragging ? Qt.ClosedHandCursor
                       : root.tableColDragging ? Qt.ClosedHandCursor
                       : (root.tableGripKind !== "" || root.tableGripPressed) ? Qt.OpenHandCursor
                       : (root.dividerDragging || root.pulling
                          || root.dividerHoverRecord >= 0 || root.pullHoverRow >= 0) ? Qt.SplitHCursor
                       : overClickable ? Qt.PointingHandCursor
                       : Qt.IBeamCursor

            onPressed: (m) => {
                root.forceActiveFocus()
                // A press anywhere on the document closes an open margin
                // thread card (the press then does its normal work).
                if (root.openThreadId !== "") root.openThreadId = ""
                // Right-click anywhere on a block → its context menu (capturing the
                // cell when over a table, for the row/column ops).
                if (m.button === Qt.RightButton) {
                    var trow = blockModel.blockAt(root.pageXAt(m.x, m.y), m.y)
                    root.menuLinkUrl = ""; root.menuIssue = null
                    var rh = root.hitTest(m.x, m.y)              // link / misspelling under the click?
                    root.menuLinkUrl = blockModel.linkAt(rh.row, rh.col)
                    var bi = spell.issueAt(rh.row, rh.col)
                    root.menuIssue = bi.ruleId !== undefined ? bi : null
                    root.openBlockMenu(m.x - flick.contentX, m.y - flick.contentY, trow)
                    return
                }
                // (Block drag-reorder starts from the ruler's number handles
                // now — the left grip gutter is retired.)
                cursor.resetGoalX(); cursor.clearMarks()
                {   // SR-4 S7b: a table grip — a click picks a set, a drag moves (decided on the first move)
                    const gg = root.tableGripAt(m.x, m.y)
                    if (gg) {
                        root.tableGripPressed = true
                        root.tableGripPressHead = gg.head; root.tableGripPressKind = gg.kind; root.tableGripPressIndex = gg.index
                        root.tableGripPressMods = m.modifiers; root.tableGripPressX = m.x; root.tableGripPressY = m.y
                        return
                    }
                }
                // Lane gestures (SR-3 S7b) start before any caret placement.
                if (root.dividerHoverRecord >= 0 && !(m.modifiers & Qt.ControlModifier)) {   // ⌘ = pull, never resize
                    root.beginDividerDrag(root.dividerHoverRecord, root.dividerHoverIndex, m.x - root.leftEdge,
                                          (m.modifiers & Qt.AltModifier) !== 0)
                    return
                }
                if ((m.modifiers & Qt.ControlModifier) && !(m.modifiers & Qt.ShiftModifier)) {   // ⌘-press on the pull band
                    // Re-test at the press: the hover cue only tracks ⌘ while the pointer moves.
                    const prow = blockModel.blockAt(root.pageXAt(m.x, m.y), m.y), ps = root.pullSideAt(prow, m.x)
                    if (ps >= 0 && root.taskCheckboxAt(m.x, m.y) < 0) {
                        root.pullArmed = true; root.pullArmRow = prow; root.pullArmSide = ps
                        root.pullArmMods = m.modifiers; root.pullArmX = m.x; root.pullArmY = m.y
                        return
                    }
                }
                // Click a task-item checkbox → cycle its status (todo→doing→done).
                var tcb = root.taskCheckboxAt(m.x, m.y)
                if (tcb >= 0) { blockModel.toggleTask(tcb); return }
                // Click a code block's language chip → the language picker,
                // anchored under the chip.
                var lcRow = root.codeLangChipAt(m.x, m.y)
                if (lcRow >= 0) {
                    var lcCell = root.cellForRow(lcRow)
                    var lcp = lcCell.langChip.mapToItem(root, 0, lcCell.langChip.height + 4)
                    root.menuX = lcp.x; root.menuY = lcp.y
                    root.openLangPopupForRow(lcRow)
                    return
                }
                var h = root.hitTest(m.x, m.y)
                // Triple-click (2026-09-09): a press right after a double-click
                // on the same row selects the WHOLE block. No drag arm.
                if (mouse.lastDblClickMs > 0 && h.row === mouse.lastDblClickRow
                    && Date.now() - mouse.lastDblClickMs < 500) {
                    mouse.lastDblClickMs = 0
                    cursor.setCaret(h.row, 0)
                    cursor.move(h.row, blockModel.contentForRow(h.row).length, true)
                    return
                }
                mouse.lastDblClickMs = 0
                // T5 (S9c): a video in a cell too narrow for its transport opens the review view on click.
                if (blockModel.typeForRow(h.row) === 3 && blockModel.mediaKind(h.row) === "video"
                    && blockModel.laneForRow(h.row) >= 0 && root.measureForRow(h.row) < root.transportMinW) {
                    root.setActiveTab(blockModel.idForRow(h.row))
                    return
                }
                {   // SR-4 §4.14: a typed table cell — a click opens a choice cell's picker; a click on a
                    // check cell's box cycles it. The caret parks at the cell's start.
                    const gh = blockModel.tableHeadOf(h.row)
                    const gc = gh >= 0 && !blockModel.isHeaderRow(h.row) ? blockModel.tableColumnOf(h.row) : -1
                    const gk = gc >= 0 ? blockModel.tableColumnKind(gh, gc) : 0
                    if (gk === 1) {
                        cursor.setCaret(h.row, 0)
                        root.openGridChoicePicker(gh, blockModel.tableRowOf(h.row), gc, "")
                        return
                    }
                    const gcell = gk === 2 ? root.cellForRow(h.row) : null
                    if (gcell && m.x >= gcell.colLeft - 2 && m.x <= gcell.colLeft + 18) {
                        cursor.setCaret(h.row, 0)
                        blockModel.tableCycleCellCheck(gh, blockModel.tableRowOf(h.row), gc)
                        return
                    }
                }
                // Inline choice chip (DT-2) → picker; the press never places
                // the caret (chips are atomic — the caret parks after it).
                {
                    var crange = blockModel.choiceRangeAt(h.row, h.col)
                    if (crange.length === 2) {
                        cursor.setCaret(h.row, crange[1])
                        root.openInlineChoicePicker(h.row, crange[0],
                            m.x - flick.contentX, m.y - flick.contentY)
                        return
                    }
                }
                // Caret takes priority over links: a click always edits. Opening a
                // link is via the hover tooltip / context menu (never steals the press).
                root.hoverLinkUrl = ""; linkTipHide.stop()
                if (m.modifiers & Qt.ShiftModifier) cursor.move(h.row, h.col, true)
                else {
                    // Clicking into a different block leaves the old one → commit it.
                    if (h.row !== cursor.focusRow) blockModel.commitMarkdown(cursor.focusRow)
                    cursor.setCaret(h.row, h.col)
                }
                root.dragging = true
                root.dragX = m.x; root.dragViewY = m.y - flick.contentY
            }
            onPositionChanged: (m) => {
                if (root.pullArmed) {   // the pull strip's press decides on its first move
                    const dx = Math.abs(m.x - root.pullArmX), dy = Math.abs(m.y - root.pullArmY)
                    if (Math.max(dx, dy) <= 4) return
                    root.pullArmed = false
                    if (dx > dy) {
                        root.beginPull(root.pullArmRow, root.pullArmSide, root.pullArmX - root.leftEdge)
                        root.updatePull(m.x - root.leftEdge)
                    } else {
                        root.edgePressCaret(root.pullArmX, root.pullArmY, root.pullArmMods, true)
                        const eh = root.hitTest(m.x, m.y)
                        cursor.move(eh.row, eh.col, true)
                    }
                    return
                }
                if (root.dividerDragging) { root.updateDividerDrag(m.x - root.leftEdge); return }
                if (root.pulling) { root.updatePull(m.x - root.leftEdge); return }
                if (root.blockDragging) {
                    root.blockDragViewY = m.y - flick.contentY
                    root.blockDragX = m.x
                    root.aimBlockDrag(m.x, m.y)
                    return
                }
                if (root.tableGripPressed) {   // S7b: past the threshold a grip press becomes a drag
                    if (Math.abs(m.x - root.tableGripPressX) + Math.abs(m.y - root.tableGripPressY) <= 4) return
                    root.tableGripPressed = false
                    root.tableGripKind = ""
                    if (root.tableGripPressKind === "row") {   // the rail's drag: a header row carries its table
                        const run = root.dragRunFor(blockModel.tableRecords(root.tableGripPressHead)[root.tableGripPressIndex])
                        root.blockDragRow = run[0]; root.blockDragCount = run[1]
                        root.blockDragging = true
                        root.blockDragViewY = m.y - flick.contentY; root.blockDragX = m.x
                        root.aimBlockDrag(m.x, m.y)
                    } else {
                        root.tableColDragging = true
                        root.tableColGap = root.tableColGapAt(root.tableGripPressHead, m.x - root.leftEdge)
                    }
                    return
                }
                if (root.tableColDragging) { root.tableColGap = root.tableColGapAt(root.tableGripPressHead, m.x - root.leftEdge); return }
                if (root.dragging) {
                    root.dragX = m.x; root.dragViewY = m.y - flick.contentY
                    var h = root.hitTest(m.x, m.y)
                    cursor.move(h.row, h.col, true)
                    return
                }
                // hover (not pressed).
                root.hoverRow = blockModel.blockAt(root.pageXAt(m.x, m.y), m.y)
                // Over an interactive widget (block task checkbox, an inline
                // choice chip, or a table check/choice body cell) → a
                // pointing-hand cursor instead of the I-beam.
                var clk = root.taskCheckboxAt(m.x, m.y) >= 0
                root.codeChipHoverRow = root.codeLangChipAt(m.x, m.y)
                clk = clk || root.codeChipHoverRow >= 0
                if (!clk) {
                    var chh = root.hitTest(m.x, m.y)
                    clk = blockModel.choiceAt(chh.row, chh.col) !== ""
                }
                mouse.overClickable = clk
                // Lane gestures (SR-3 S7b): a lane gap → drag its divider; the hot band just
                // inside a block's column edge → pull out a lane. Clickables and borders win.
                const gg = root.tableGripAt(m.x, m.y)   // SR-4 S7b: table grips win over the lane gestures
                root.tableGripHead = gg ? gg.head : -1
                root.tableGripKind = gg ? gg.kind : ""
                root.tableGripIndex = gg ? gg.index : -1
                // ⌘ held = the pull band's drag, so the border resize steps aside (either/or, 2026-09-15).
                const dv = (clk || gg || (m.modifiers & Qt.ControlModifier)) ? null : root.dividerAt(root.hoverRow, m.x - root.leftEdge)
                root.dividerHoverRecord = dv ? dv.record : -1
                root.dividerHoverIndex = dv ? dv.index : -1
                // The pull band needs ⌘ held (user walk 2026-09-15: a plain drag at a cell's edge is the
                // column-border resize; pulling out a lane is the special drag).
                const ps = (clk || dv || gg || !(m.modifiers & Qt.ControlModifier)) ? -1 : root.pullSideAt(root.hoverRow, m.x)
                root.pullHoverRow = ps >= 0 ? root.hoverRow : -1
                root.pullHoverSide = ps
                // Hovering an image row → show its resize handles. Don't clear on a
                // non-image *handle* hover (the central layer onExits then); only a
                // different block hides them.
                if (root._isResizableMediaRow(root.hoverRow)) root.imgHandleRow = root.hoverRow
                else root.imgHandleRow = -1
                // Link under the pointer → anchor the open-link tooltip there. A
                // grace timer (not an immediate clear) lets the pointer travel up
                // onto the pill to click it.
                var lh = root.hitTest(m.x, m.y)
                var lurl = blockModel.linkAt(lh.row, lh.col)
                if (lurl.length > 0) {
                    // Anchor the pill ONCE on entering a link and freeze it — if it
                    // tracked the mouse, moving up to click it would chase it away.
                    if (lurl !== root.hoverLinkUrl) {
                        root.hoverLinkUrl = lurl
                        root.hoverLinkX = m.x - flick.contentX; root.hoverLinkViewY = m.y - flick.contentY
                    }
                    linkTipHide.stop()
                } else if (root.hoverLinkUrl.length > 0) {
                    linkTipHide.restart()
                }
            }
            onExited: { root.hoverRow = -1
                        root.dividerHoverRecord = -1; root.dividerHoverIndex = -1; root.pullHoverRow = -1
                        root.tableGripKind = ""; root.tableGripHead = -1; root.tableGripIndex = -1
                        root.codeChipHoverRow = -1
                        if (root.hoverLinkUrl.length > 0) linkTipHide.restart() }
            onReleased: {
                if (root.pullArmed) {   // a click on the pull strip: the caret goes there
                    root.pullArmed = false
                    root.edgePressCaret(root.pullArmX, root.pullArmY, root.pullArmMods, false)
                }
                else if (root.tableGripPressed) {
                    root.tableGripPressed = false
                    root.tableGripClick(root.tableGripPressHead, root.tableGripPressKind, root.tableGripPressIndex, root.tableGripPressMods)
                }
                else if (root.tableColDragging) root.commitGridColDrag()
                else if (root.dividerDragging) root.commitDividerDrag()
                else if (root.pulling) root.commitPull()
                else if (root.blockDragging) root.commitBlockDrag()
                else root.dragging = false
            }
            onCanceled: {
                root.tableGripPressed = false; root.tableColDragging = false; root.tableColGap = -1; root.pullArmed = false
                root.cancelDividerDrag(); root.cancelPull()
                if (root.blockDragging) { root.blockDragging = false; root.blockDragRow = -1; root.dropGap = -1; root.blockDragCount = 1 }
                else root.dragging = false
            }
            onDoubleClicked: (m) => {
                // End the press-drag the 2nd press armed, so a tiny mouse jitter
                // before release can't re-extend the selection back to the click
                // point (which collapsed the word to word-start→cursor).
                root.dragging = false
                // Double-click a file-attachment chip → reveal it in Finder/Explorer.
                var mrow = blockModel.blockAt(root.pageXAt(m.x, m.y), m.y)
                if (blockModel.typeForRow(mrow) === 3 && blockModel.mediaKind(mrow) === "file") {
                    blockModel.revealMedia(mrow); return
                }
                // select the word under the cursor
                var h = root.hitTest(m.x, m.y)
                var t = blockModel.contentForRow(h.row)
                var s = h.col, e = h.col
                while (s > 0 && /\w/.test(t.charAt(s - 1))) s--
                while (e < t.length && /\w/.test(t.charAt(e))) e++
                cursor.setCaret(h.row, s); cursor.move(h.row, e, true)
                mouse.lastDblClickMs = Date.now(); mouse.lastDblClickRow = h.row
            }
        }

        // Edge auto-scroll while drag-selecting OR drag-reordering near the
        // top/bottom (the persistent `mouse` area keeps its grab through scroll).
        Timer {
            interval: 16; repeat: true
            running: root.dragging || root.blockDragging || root.mergeDragActive
            onTriggered: {
                var margin = 44, sp = 0
                var viewY = root.mergeDragActive ? root.mergeDragViewY
                          : root.blockDragging ? root.blockDragViewY : root.dragViewY
                if (viewY < margin) sp = -Math.max(6, margin - viewY)
                else if (viewY > flick.height - margin) sp = Math.max(6, viewY - (flick.height - margin))
                if (sp === 0) return
                flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + sp))
                var cy = viewY + flick.contentY               // content point under the held cursor
                if (root.mergeDragActive) { root.mergeDropGap = root.mergeGapForY(cy); return }
                if (root.blockDragging) { root.aimBlockDrag(root.blockDragX, cy); return }
                var h = root.hitTest(root.dragX, cy)
                cursor.move(h.row, h.col, true)
            }
        }

        ScrollBar.vertical: MnScrollBar {}
        // The page-level horizontal bar — appears only when a wide table (or
        // ink-mode pan span) pushes contentWidth past the viewport.
        ScrollBar.horizontal: MnScrollBar {}
    }

    // --- Full-frame kanban board (the active table tab in board mode). Scrolls
    // both ways; the board view owns all card interaction directly (a dedicated
    // mode like the table frame — no document mouse layer above it). ---
    Flickable {
        id: boardFrame
        visible: root.activeGridHead >= 0 && root.boardMode
        anchors.fill: parent
        anchors.topMargin: Theme.dim.toolStripHeight   // room for the tab toolbar
        contentWidth: Math.max(width, boardView.implicitWidth + 40)
        contentHeight: Math.max(height, boardView.implicitHeight + 40)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: MnScrollBar {}
        ScrollBar.horizontal: MnScrollBar {}
        BlockKanban {
            id: boardView
            x: 20; y: 20
            width: implicitWidth; height: implicitHeight
            active: boardFrame.visible
            head: root.activeGridHead
            groupCol: root.boardCol
            onShowGrid: root.leaveBoard()        // grouping column vanished → grid (or the document, S9a)
            onEditClosed: root.forceActiveFocus()
            onOpenCard: (r, c) => {              // double-click → the grid frame (S9b), cell focused
                const gh = root.activeGridHead
                root.showGridView()
                if (gh >= 0) root.landInCell(gh, r, c)
                root.forceActiveFocus()
            }
            onLaneMenuRequested: (li, bx, by) => {
                var p = boardView.mapToItem(root, bx, by)
                laneMenu.li = li
                laneMenu.x = Math.max(8, Math.min(p.x, root.width - laneMenu.width - 8))
                laneMenu.y = Math.max(8, Math.min(p.y, root.height - laneMenu.height - 8))
                laneMenu.open()
            }
        }
        Timer {   // edge auto-scroll while a card is being dragged
            interval: 16; repeat: true
            running: boardView.dragRow >= 0
            onTriggered: {
                var margin = 44
                var vx = 20 + boardView.dragX - boardFrame.contentX
                var vy = 20 + boardView.dragY - boardFrame.contentY
                var dx = 0, dy = 0
                if (vx < margin) dx = -Math.max(6, margin - vx)
                else if (vx > boardFrame.width - margin) dx = Math.max(6, vx - (boardFrame.width - margin))
                if (vy < margin) dy = -Math.max(6, margin - vy)
                else if (vy > boardFrame.height - margin) dy = Math.max(6, vy - (boardFrame.height - margin))
                if (dx === 0 && dy === 0) return
                boardFrame.contentX = Math.max(0, Math.min(boardFrame.contentWidth - boardFrame.width, boardFrame.contentX + dx))
                boardFrame.contentY = Math.max(0, Math.min(boardFrame.contentHeight - boardFrame.height, boardFrame.contentY + dy))
                // The pointer is stationary in the viewport while content slides
                // under it — refresh the board-space drag point + drop target.
                boardView.dragX = vx + boardFrame.contentX - 20
                boardView.dragY = vy + boardFrame.contentY - 20
                boardView.updateDrop(boardView.dragX, boardView.dragY)
            }
        }
    }

    // Lane-header right-click menu (board view). Self-contained rows — MenuRow
    // is wired to blockMenu's close/sub-panel machinery, so it isn't reused here.
    component LaneMenuRow: Rectangle {
        property alias text: laneMenuRowLabel.text
        signal activated()
        width: 168; height: 24; radius: 0
        color: laneMenuRowMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
        Text {
            id: laneMenuRowLabel
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: 10
            color: Theme.colors.text
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }
        MouseArea {
            id: laneMenuRowMA
            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
            onClicked: { parent.activated(); laneMenu.close() }
        }
    }
    Popup {
        id: laneMenu
        property int li: -1
        readonly property var lane: (li >= 0 && li < boardView.lanes.length) ? boardView.lanes[li] : null
        // Option ops only make sense on a real choice option (not check lanes,
        // not the trailing "No status" lane).
        readonly property bool optionLane: lane !== null && root.boardCol >= 0
            && (blockModel.contentRevision, root.boardKind()) === 1
            && lane.key !== ""
        padding: 4; z: 60
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: root.forceActiveFocus()
        background: Rectangle { color: Theme.colors.surfaceRaised; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 1
            LaneMenuRow { text: "Add card"; onActivated: boardView.addCard(laneMenu.li) }
            LaneMenuRow { visible: laneMenu.optionLane && laneMenu.li > 0
                          text: "Move lane left"
                          onActivated: root.moveBoardOption(laneMenu.lane.key, laneMenu.li - 1) }
            LaneMenuRow { visible: laneMenu.optionLane && laneMenu.li < boardView.lanes.length - 2
                          text: "Move lane right"
                          onActivated: root.moveBoardOption(laneMenu.lane.key, laneMenu.li + 1) }
            LaneMenuRow { visible: laneMenu.lane !== null && root.boardCol >= 0
                                   && (blockModel.contentRevision, root.boardKind()) === 1
                          text: "Edit options…"
                          onActivated: root.openBoardOptions() }
        }
    }
    Rectangle {   // table-tab toolbar: the family flat-button strip above the frame
        id: tableTabBar
        visible: root.activeGridHead >= 0
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: Theme.dim.toolStripHeight
        color: Theme.colors.surface
        z: 20
        Rectangle {   // bottom hairline against the frame (mirrors BottomRail's)
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1; color: Theme.colors.border
        }
        Row {   // Table / Board — a two-segment view toggle. Type mirrors the
                // bottom tab strip (13px, bright when active / muted otherwise)
                // so the two bars read as one family.
            anchors.left: parent.left; anchors.leftMargin: 8
            height: parent.height - 1
            FlatButton {
                iconName: "table"; text: "Table"
                height: parent.height
                checked: !root.boardMode
                checkedColor: Theme.colors.divider   // grey — keep accent for real highlights
                labelSize: Theme.font.sizeChrome
                labelColor: checked ? Theme.colors.textBright : Theme.colors.textMuted
                iconColor: labelColor
                onClicked: { root.leaveBoard(); root.forceActiveFocus() }
            }
            FlatButton {
                iconName: "kanban"; text: "Board"
                height: parent.height
                checked: root.boardMode
                enabled_: root.boardCol >= 0 || root.firstGroupCol >= 0
                checkedColor: Theme.colors.divider   // grey — keep accent for real highlights
                labelSize: Theme.font.sizeChrome
                labelColor: !enabled_ ? Theme.colors.textSubtle
                          : checked ? Theme.colors.textBright : Theme.colors.textMuted
                iconColor: labelColor
                tooltip: enabled_ ? "" : "Needs a choice or checkmark column"
                tooltipSide: "right"
                onClicked: {
                    if (!root.boardMode)
                        root.openBoard(root.activeGridHead, root.boardCol >= 0 ? root.boardCol : root.firstGroupCol)
                    root.forceActiveFocus()
                }
            }
        }
        Row {   // T4: the row filter (derived tables' grid only) — a plain TextInput in a themed
                // frame (a Controls TextField won't theme under the native macOS style) + the count.
            visible: root.activeGridHead >= 0 && !root.boardMode
            anchors.right: parent.right; anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.tableFilter !== ""
                readonly property int total: root.activeGridHead >= 0
                    ? (blockModel.contentRevision, blockModel.tableRecords(root.activeGridHead).length - blockModel.headerCount(root.activeGridHead)) : 0
                text: "Showing " + (total - root.tableHiddenCount) + " of " + total + " rows"
                color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
            }
            Rectangle {
                width: 180; height: 22; radius: 0
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.colors.codeBg; border.width: 1
                border.color: tableFilterField.activeFocus ? Theme.colors.textMuted : Theme.colors.border
                TextInput {
                    id: tableFilterField
                    anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true; selectByMouse: true
                    color: Theme.colors.text; selectionColor: Theme.colors.selectionBg
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
                    text: root.tableFilter
                    onTextEdited: root.applyGridFilter(text)
                    Keys.onEscapePressed: { if (text !== "") { text = ""; root.applyGridFilter("") } else root.forceActiveFocus() }
                    onAccepted: root.forceActiveFocus()
                    Text {
                        anchors.fill: parent; verticalAlignment: Text.AlignVCenter
                        visible: tableFilterField.text.length === 0
                        text: "Filter rows…"
                        color: Theme.colors.textSubtle; font: tableFilterField.font
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    // --- Full-frame PDF view (the active PDF tab): every page, continuous scroll.
    // A dedicated mode with no central mouse layer, so PdfMultiPageView owns its
    // own scrolling/selection. The document is (re)created from the active row's
    // file URL; "" while no PDF tab is open. ---
    Rectangle {
        id: pdfFrame
        visible: root.activePdfRow >= 0
        anchors.fill: parent
        color: Theme.colors.bgAlt   // full-frame tabs share the page field tone
        // A continuous-scroll page list (NOT PdfMultiPageView, whose internal
        // TableView always overflows horizontally by the vertical-scrollbar width
        // → a stray horizontal bar over the page). One PdfPageImage per page,
        // fit-width with a scrollbar gutter; vertical scroll only.
        // Build the document + page list only while a PDF tab is open. Otherwise
        // PdfDocument.source would bind to "" and QML's PdfDocument logs
        // "Cannot open:" on every empty source — the Loader keeps it uninstantiated.
        Loader {
            anchors.fill: parent
            active: root.activePdfRow >= 0
            sourceComponent: Item {
                PdfDocument {
                    id: pdfFrameDoc
                    source: blockModel.mediaUrl(root.activePdfRow)
                }
                ListView {
                    id: pdfList
                    anchors.fill: parent
                    anchors.margins: 10
                    clip: true
                    model: pdfFrameDoc.pageCount
                    spacing: 10
                    cacheBuffer: Math.max(0, Math.round(height * 1.5))   // height is transiently <0 during layout
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: MnScrollBar {}
                    // Zoom (2026-08-20): a ListView IS a Flickable — past
                    // fit-width the content grows a horizontal axis and
                    // flicking/space-hand pan work on both.
                    readonly property real fitPageW:
                        Math.max(1, width - 2 * Theme.dim.scrollBarWidth - 8)
                    contentWidth: Math.max(width,
                        fitPageW * root.pdfZoom + 2 * Theme.dim.scrollBarWidth + 8)
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    onFitPageWChanged: root.pdfFitPageW = fitPageW   // ⌘0 needs it at root
                    Component.onCompleted: root.pdfFitPageW = fitPageW
                    // Space-hand affordance: the open hand says "drag scrolls now".
                    HoverHandler {
                        enabled: root.pdfSpaceHeld
                        cursorShape: Qt.OpenHandCursor
                    }
                    // ⌘-wheel zoom (the sketch-canvas convention), √2 steps.
                    WheelHandler {
                        acceptedModifiers: Qt.ControlModifier
                        onWheel: (event) => root.pdfZoomStep(event.angleDelta.y > 0 ? 1 : -1)
                    }
                    delegate: Item {
                        required property int index
                        readonly property size pts: pdfFrameDoc.status === PdfDocument.Ready
                            ? pdfFrameDoc.pagePointSize(index) : Qt.size(8.5, 11)
                        // Reserve a full scrollbar width on each side of the centered page
                        // so the (right-edge) vertical bar never overlaps it.
                        readonly property real pageW: pdfList.fitPageW * root.pdfZoom
                        width: pdfList.contentWidth
                        height: pts.width > 0 ? Math.round(pageW * pts.height / pts.width)
                                              : Math.round(pageW * 1.294)
                        Rectangle {   // white page with a hairline edge on the dark backdrop
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: pageW; height: parent.height
                            color: "white"; border.width: 1; border.color: Theme.colors.border
                            PdfPageImage {
                                anchors.fill: parent
                                document: pdfFrameDoc
                                currentFrame: index
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                sourceSize.width: Math.round(parent.width * Screen.devicePixelRatio)
                            }
                            // Per-PAGE ink overlay (2026-08-19): the sketch canvas in
                            // its inline-embed shape (frame = the page rect), storing
                            // normalized page coords + source-unit widths via the
                            // block's content JSON (pdfSetPageInk → document undo).
                            // Text chips (2026-08-20): the canvas paints/selects/
                            // erases them natively; create/edit route to the
                            // ROOT-level pdfTextSession (this delegate recycles).
                            SketchCanvas {
                                id: pageInk
                                readonly property int pdfPage: index
                                anchors.fill: parent
                                // Space-hand: the canvas yields the mouse so drags
                                // fall through to the list and scroll it.
                                enabled: !root.pdfSpaceHeld
                                data: (blockModel.contentRevision,
                                       root.activePdfRow >= 0
                                       ? blockModel.pdfPageInk(root.activePdfRow, index) : "")
                                sourceWidth: Math.max(1, blockModel.mediaW(root.activePdfRow))
                                sourceHeight: Math.max(1, blockModel.mediaH(root.activePdfRow))
                                fontFamily: Theme.font.body
                                tool: (root.activePdfRow >= 0 && root.inspector
                                       && root.inspector.drawTool !== "type")
                                          ? root.inspector.drawTool : ""
                                color: root.inspector ? root.inspector.drawColor : "#FF0000"
                                strokeWidth: root.inspector ? root.inspector.drawWidth : 6
                                selectable: true
                                // Group gestures: PDF pages carry only strokes
                                // (one edited() = one txn), so the brackets are
                                // future-proofing at zero cost.
                                onGroupCommitBegan: blockModel.beginGroup(root.activePdfRow, root.activePdfRow)
                                onGroupCommitEnded: blockModel.endGroup()
                                onEdited: (json) => blockModel.pdfSetPageInk(root.activePdfRow, index, json)
                                // Text-chip session routing (the sketch contract).
                                editingTextIndex: pdfTextSession.mode === "edit"
                                                  && pdfTextSession.canvas === pageInk
                                                      ? pdfTextSession.index : -1
                                onTextCreateRequested: (nx, ny) => pdfTextSession.beginCreate(pageInk, index, nx, ny)
                                onTextEditRequested: (i) => pdfTextSession.beginEdit(pageInk, index, i)
                                onTextBoxChanged: (i, x, y, w, s) => blockModel.pdfSetPageTextBox(root.activePdfRow, index, i, x, y, w, s)
                                onTextRemoved: (i) => blockModel.pdfRemovePageText(root.activePdfRow, index, i)
                                // One page's canvas at a time owns Esc/Delete.
                                onDrawingChanged: if (drawing) root._setPdfActiveInk(pageInk)
                                onSelectionChanged: if (hasSelection) root._setPdfActiveInk(pageInk)
                                Component.onDestruction: {
                                    // A recycled delegate mid-session commits: the
                                    // session's buffer is self-contained, so the
                                    // model write survives this canvas dying.
                                    if (pdfTextSession.canvas === pageInk) pdfTextSession.commit()
                                    if (root.pdfActiveInk === pageInk) root.pdfActiveInk = null
                                }
                            }
                        }
                    }
                }
                // --- Text-chip edit overlay (2026-08-20): hosted BESIDE the
                // list — delegates recycle, so it can't live in one. Position
                // maps from the session's canvas; the contentY/width reads
                // are LOAD-BEARING deps (reactivity rule 1e — mapToItem is
                // not reactive by itself).
                Item {
                    id: pdfChipOverlay
                    anchors.fill: parent
                    visible: pdfTextSession.active && !!pdfTextSession.canvas
                    readonly property real pxPerSrc:
                        pdfTextSession.canvas
                            ? pdfTextSession.canvas.width
                              / Math.max(1, blockModel.mediaW(root.activePdfRow))
                            : 1
                    readonly property real sizePx: pdfTextSession.esize * pxPerSrc
                    readonly property real padPx: 0.4 * sizePx   // the 0.4em chip pad rule
                    readonly property point origin: {
                        pdfList.contentY; pdfList.width      // re-map on scroll/resize
                        if (!pdfTextSession.canvas) return Qt.point(0, 0)
                        return pdfTextSession.canvas.mapToItem(
                            pdfChipOverlay,
                            pdfTextSession.ex * pdfTextSession.canvas.width,
                            pdfTextSession.ey * pdfTextSession.canvas.height)
                    }
                    readonly property real chipW: pdfTextSession.canvas
                        ? pdfTextSession.ew * pdfTextSession.canvas.width : 0
                    Rectangle {   // the live CHIP: fill = element color, accent border = session
                        visible: pdfTextEditor.visible
                        x: pdfChipOverlay.origin.x; y: pdfChipOverlay.origin.y
                        width: pdfChipOverlay.chipW
                        height: pdfTextEditor.height + 2 * pdfChipOverlay.padPx
                        color: pdfTextSession.ecolor
                        border.width: 1; border.color: Theme.colors.accent
                    }
                    TextEdit {
                        id: pdfTextEditor
                        visible: pdfTextSession.active
                        x: pdfChipOverlay.origin.x + pdfChipOverlay.padPx
                        y: pdfChipOverlay.origin.y + pdfChipOverlay.padPx
                        width: Math.max(4, pdfChipOverlay.chipW - 2 * pdfChipOverlay.padPx)
                        // Height implicit: the chip grows downward while typing.
                        wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                        textMargin: 0
                        font.family: Theme.font.body   // == the canvas's fontFamily
                        font.pixelSize: Math.max(1, pdfChipOverlay.sizePx)
                        color: pdfTextSession.canvas
                                   ? pdfTextSession.canvas.textInkFor(pdfTextSession.ecolor)
                                   : Theme.colors.text
                        selectByMouse: true
                        selectionColor: Theme.colors.divider
                        // Session-owned buffer: the commit must survive this
                        // editor dying with the Loader/delegate.
                        onTextChanged: pdfTextSession.buf = text
                        onVisibleChanged: if (visible) {
                            text = pdfTextSession.mode === "edit" ? pdfTextSession.origText : ""
                            cursorPosition = text.length
                            forceActiveFocus()
                        }
                        onActiveFocusChanged: if (!activeFocus && visible) pdfTextSession.commit()
                        // Escape COMMITS (the sketch-session precedent; blanking deletes).
                        Keys.onEscapePressed: pdfTextSession.commit()
                    }
                }
                ZoomBadge {   // the tab's only zoom chrome (the sketch-tab pattern)
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.rightMargin: 10 + Theme.dim.scrollBarWidth
                    anchors.bottomMargin: 10
                    // Readout is TRUE page scale: 100% = one unit per PDF point,
                    // whatever the fit ratio happens to be.
                    zoomValue: root.pdfFitPageW > 0 && blockModel.mediaW(root.activePdfRow) > 0
                               ? root.pdfZoom * root.pdfFitPageW / blockModel.mediaW(root.activePdfRow)
                               : 1
                    showFitInk: false
                    onFitRequested: root.pdfZoomFit()
                    onHundredRequested: root.pdfZoom100()
                }
            }
        }
    }

    // --- PDF text-chip session (2026-08-20): the sketchTextSession contract
    // with a SELF-CONTAINED text buffer — the overlay and the hosting canvas
    // can both die mid-session (delegate recycling, tab exit) and the commit
    // still lands, because row/page/coords/buf are all captured here. ---
    QtObject {
        id: pdfTextSession
        property string mode: ""        // "" | "create" | "edit"
        property int row: -1            // captured at begin — commit may land later
        property int page: -1
        property var canvas: null       // the hosting pageInk (may die first)
        property int index: -1          // edit mode: element index
        property real ex: 0; property real ey: 0; property real ew: 0
        property real esize: 16
        property color ecolor: "#E4E3E2"
        property string origText: ""
        property string buf: ""         // mirrored from the overlay TextEdit
        readonly property bool active: mode !== ""
        function beginCreate(c, pg, nx, ny) {
            commit()                    // a canvas press can race focus-out
            row = root.activePdfRow; page = pg; canvas = c
            esize = root.inspector ? root.inspector.drawTextSize : 16
            ecolor = root.inspector ? root.inspector.drawColor : "#E4E3E2"
            ex = nx; ey = ny
            ew = Math.min(0.5, Math.max(240, 2 * esize)
                          / Math.max(1, blockModel.mediaW(root.activePdfRow)))
            index = -1; origText = ""; buf = ""
            mode = "create"
        }
        function beginEdit(c, pg, i) {
            commit()
            row = root.activePdfRow; page = pg; canvas = c
            var t = c.textElementAt(i)
            if (t.text === undefined) return
            index = i
            ex = t.x; ey = t.y; ew = t.w
            esize = t.size; ecolor = t.color
            origText = t.text; buf = t.text
            mode = "edit"
        }
        function commit() {
            if (mode === "") return
            // Close the session BEFORE the model call so the data round-trip
            // can't re-enter it (Connections below).
            var m = mode, r = row, p = page, i = index, orig = origText, txt = buf
            mode = ""; canvas = null
            if (m === "create" && txt.trim() !== "")
                blockModel.pdfAddPageText(r, p, ex, ey, ew, txt, esize, "" + ecolor)
            else if (m === "edit" && txt !== orig)
                blockModel.pdfSetPageText(r, p, i, txt)   // blank ⇒ model deletes
            root.forceActiveFocus()
        }
        function cancel() {
            if (mode === "") return
            mode = ""; canvas = null
            root.forceActiveFocus()
        }
    }
    Connections {
        // External data change mid-EDIT (undo, another surface) → the element
        // under the overlay is stale: cancel. Create sessions just continue.
        target: pdfTextSession.canvas
        function onDataChanged() {
            if (pdfTextSession.mode === "edit") pdfTextSession.cancel()
        }
    }
    Connections {   // any tool change commits (the sketch-session rationale)
        target: root.inspector
        function onDrawToolChanged() { pdfTextSession.commit() }
    }
    Connections {   // leaving the PDF tab commits (captured row survives)
        target: root
        function onActivePdfRowChanged() {
            if (root.activePdfRow < 0) pdfTextSession.commit()
        }
    }

    // --- Full-frame video studio (the active video tab): the shared decoder's
    // surface on a centered stage, the family transport bar, and the notes
    // panel chassis at the bottom (the QCView-interop filmstrip lands with
    // VA-2 — see PLAN-video-annotations.md). Entering the tab activates the
    // video PAUSED at its remembered playhead (onActiveVideoIdChanged); the
    // inline surface + per-row bars hide while the studio owns the decoder. ---
    Rectangle {
        id: studioFrame
        visible: root.activeVideoRow >= 0
        anchors.fill: parent
        color: Theme.colors.bgAlt   // full-frame tabs share the page field tone
        readonly property int r: root.activeVideoRow
        readonly property real notesPanelH: 320   // taller filmstrip → more room for long notes (stage shrinks to fit)

        // image://videoframe poster URL (base64url path @ frame) — the stage
        // shows the banked frame until the decoder paints its first one. The byte
        // array (not a string) avoids Qt.btoa's deprecated UTF-16 string overload
        // and matches the provider's QString::fromUtf8 decode.
        function _vframeSrc(path, frame) {
            if (path === "") return ""
            var enc = encodeURIComponent(path), bytes = []
            for (var i = 0; i < enc.length; ++i) {
                if (enc[i] === '%') { bytes.push(parseInt(enc.substr(i + 1, 2), 16)); i += 2 }
                else bytes.push(enc.charCodeAt(i))
            }
            // Qt.btoa's array overload returns a QByteArray (not a JS String) — coerce.
            var b = ("" + Qt.btoa(bytes)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
            return "image://videoframe/" + b + "@" + frame
        }

        // The stage: the frame aspect-fitted + centered above the transport. An
        // explicit fit box (not the surface's own letterboxing) so the poster
        // aligns exactly and VA-3's stroke overlay knows the frame rect.
        Item {
            id: studioStage
            anchors { top: parent.top; left: parent.left; right: parent.right
                      bottom: studioBar.top; margins: 16; bottomMargin: 12 }
            readonly property int vw: studioFrame.r >= 0 ? (blockModel.contentRevision, blockModel.mediaW(studioFrame.r)) : 0
            readonly property int vh: studioFrame.r >= 0 ? blockModel.mediaH(studioFrame.r) : 0
            readonly property real fitScale: (vw > 0 && vh > 0 && width > 0 && height > 0)
                ? Math.min(width / vw, height / vh) : 0
            readonly property real boxW: Math.round(vw * fitScale)
            readonly property real boxH: Math.round(vh * fitScale)

            Item {
                id: studioBox
                anchors.centerIn: parent
                width: studioStage.boxW; height: studioStage.boxH

                Image {   // poster: the banked playhead frame until the live one paints
                    anchors.fill: parent
                    visible: !studioSurface.visible
                    source: studioFrame.r >= 0
                        ? studioFrame._vframeSrc(blockModel.mediaPlaybackSource(studioFrame.r),
                                                 (root.videoPlayheadRev, root.videoPlayheadFor(studioFrame.r)))
                        : ""
                    asynchronous: true; cache: true
                    fillMode: Image.PreserveAspectFit
                    sourceSize.width: Math.round(width * Screen.devicePixelRatio)
                    smooth: true
                }
                VideoSurfaceItem {
                    id: studioSurface
                    anchors.fill: parent
                    videoDecoder: videoDec
                    visible: root.activeVideoRow >= 0
                             && root.videoPlayingRow === root.activeVideoRow
                             && root._videoSurfaceReady
                    fillColor: Qt.rgba(Theme.colors.bgAlt.r, Theme.colors.bgAlt.g,
                                       Theme.colors.bgAlt.b, 1.0)   // letterbox = the tab's field tone
                }

                // The stroke layer: renders the current frame's strokes
                // (QCView's + ours) and captures drawing when a tool is
                // armed in the Inspector's Draw target. Sized to the frame
                // box exactly → its [0..1] space IS QCView's. Disarmed it
                // refuses mouse events, so playback clicks pass through.
                VideoAnnotator {
                    id: studioAnnotator
                    anchors.fill: parent
                    // Only over a LIVE surface: strokes paint instantly but
                    // the decoder publishes the sought frame later (first
                    // seek especially) — ink over a blank/stale stage reads
                    // as "annotations without the screenshot".
                    // Also hidden when the user toggles a clean (notes-off) view.
                    visible: studioSurface.visible && !root.annotationsHidden
                    notes: vnotes
                    sourceWidth: studioStage.vw
                    // Load-bearing reads only (reactivity rule 1e).
                    frame: root.videoPlayingRow === studioFrame.r
                        ? videoDec.currentFrame
                        : (root.videoPlayheadRev >= 0 ? root.videoPlayheadFor(studioFrame.r) : 0)
                    tool: (root.activeVideoRow >= 0 && root.inspector
                           && root.inspector.drawTool !== "type") ? root.inspector.drawTool : ""
                    color: root.inspector ? root.inspector.drawColor : "#FF0000"
                    strokeWidth: root.inspector ? root.inspector.drawWidth : 6
                    // Strokes pin to a frame — drawing on a moving target
                    // would land on whatever frame the release hits.
                    onStrokeStarted: { videoDec.pause(); videoAudio.pause() }
                }
            }
        }

        VideoTransport {
            id: studioBar
            anchors { left: parent.left; right: parent.right; bottom: studioNotes.top }
            editor: root; dec: videoDec; audio: videoAudio
            row: studioFrame.r
            live: studioFrame.r >= 0 && root.videoPlayingRow === studioFrame.r
        }

        // Clicking studio dead space reclaims focus (commits a card's text
        // edit and restores the transport keys). Sits UNDER the notes panel
        // and transport, so their controls still take clicks first.
        MouseArea { anchors.fill: parent; z: -1; onClicked: root.forceActiveFocus() }

        // Notes panel — the QCView filmstrip in family dress: sticky add-note
        // tile, then one card per note (thumbnail / timecode / text /
        // addressed / delete). Cards key by timecode; card click seeks.
        Rectangle {
            id: studioNotes
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: studioFrame.notesPanelH
            color: Theme.colors.surfaceRaised   // chrome tone — the studio's bottom apparatus (cards recess into it, the Inspector recipe)

            Rectangle {   // add-note tile (sticky left)
                id: addNoteTile
                anchors { left: parent.left; top: parent.top; bottom: hideTile.top
                          topMargin: 13; bottomMargin: 8; leftMargin: 12 }
                width: 96
                color: addNoteMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
                border.width: 1; border.color: Theme.colors.border
                Column {
                    anchors.centerIn: parent
                    spacing: 8
                    Icon { name: "plus"; size: 22; color: Theme.colors.textMuted
                           anchors.horizontalCenter: parent.horizontalCenter }
                    Text {
                        text: "Add note"
                        color: Theme.colors.textMuted
                        font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                }
                MouseArea {
                    id: addNoteMA
                    anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        // The note pins to the frame on screen: the scrub
                        // target mid-scrub, else the live playhead, else the
                        // banked one (decoder failed to open).
                        var f = root.videoPlayingRow === studioFrame.r
                            ? root._vidIntendedFrame()
                            : root.videoPlayheadFor(studioFrame.r)
                        vnotes.addNoteAtFrame(f)
                        root.forceActiveFocus()
                    }
                }
            }

            Rectangle {   // hide/show the on-video annotation overlay (clean view)
                id: hideTile
                anchors { left: parent.left; bottom: parent.bottom
                          leftMargin: 12; bottomMargin: 12 }
                width: 96; height: 40
                color: root.annotationsHidden ? Theme.colors.divider
                     : (hideMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
                border.width: 1
                border.color: root.annotationsHidden ? Theme.colors.textBright : Theme.colors.border
                Row {
                    anchors.centerIn: parent; spacing: 6
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: root.annotationsHidden ? "eye" : "eye-slash"
                        size: 16
                        color: root.annotationsHidden ? Theme.colors.textBright : Theme.colors.textMuted
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.annotationsHidden ? "Show" : "Hide"
                        color: root.annotationsHidden ? Theme.colors.textBright : Theme.colors.textMuted
                        font.family: Theme.font.family; font.pixelSize: 12
                    }
                }
                MouseArea {
                    id: hideMA
                    anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.annotationsHidden = !root.annotationsHidden
                }
            }

            Text {   // empty state, in the filmstrip's space
                visible: vnotes.count === 0 && !vnotes.loading
                anchors.centerIn: parent
                text: "No notes yet"
                color: Theme.colors.textSubtle
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
            }

            ListView {
                id: noteStrip
                anchors { left: addNoteTile.right; right: parent.right
                          top: parent.top; bottom: parent.bottom
                          topMargin: 13; bottomMargin: 4; leftMargin: 12; rightMargin: 12 }
                orientation: ListView.Horizontal
                spacing: 12
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.horizontal: MnScrollBar {}
                // The revision read must be LOAD-BEARING (ternary), not a
                // comma-tuple: qmlcachegen elides a discarded left operand,
                // killing the dependency capture — the strip then never
                // refreshes (live-debugged twice, 2026-06-11).
                model: vnotes.revision >= 0 ? vnotes.noteList() : []

                delegate: Rectangle {
                    id: noteCard
                    required property var modelData
                    readonly property string tc: modelData.timecode
                    // 16:9 thumbnail — the overwhelmingly common delivery
                    // aspect; other aspects letterbox inside it (fit).
                    readonly property real thumbH: Math.round((width - 16) * 9 / 16)
                    // The card under the playhead gets the family's selected
                    // look (divider fill is too heavy here — border brightens).
                    readonly property bool current: root.videoPlayingRow === studioFrame.r
                        && videoDec.currentFrame === modelData.frame
                    width: 220
                    // Shrink to clear the horizontal scrollbar when it's shown (card
                    // width is fixed, so contentWidth doesn't depend on this — no loop).
                    height: noteStrip.height
                            - (noteStrip.contentWidth > noteStrip.width ? Theme.dim.scrollBarWidth + 6 : 12)
                    // Outline-defined cards (user ruling): the fill matches the
                    // panel (transparent tracks it), the BORDER draws the card,
                    // and the bright border marks the playhead's current frame.
                    color: "transparent"
                    border.width: 1
                    border.color: current ? Theme.colors.textBright : Theme.colors.border

                    Column {
                        anchors { fill: parent; margins: 8 }
                        spacing: 6

                        Rectangle {   // thumbnail (click = seek to this frame)
                            width: parent.width; height: noteCard.thumbH
                            color: Theme.colors.surfaceRecess   // image bed reads as an inset well
                            clip: true
                            Image {
                                anchors.fill: parent
                                // Revision-keyed source: the PNG lands async
                                // after the note is minted — each bump retries.
                                // A Windows drive path (C:/…) needs file:/// (the
                                // bare "file://" + "C:/…" parses C: as a host and
                                // drops the colon); a POSIX path (/…) needs file://.
                                source: modelData.image !== ""
                                    ? (modelData.image.charAt(0) === "/" ? "file://" : "file:///")
                                      + modelData.image + "?v=" + vnotes.revision
                                    : ""
                                asynchronous: true; cache: false
                                fillMode: Image.PreserveAspectFit
                                sourceSize.width: Math.round(220 * Screen.devicePixelRatio)
                                smooth: true
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.ensureVideoActive(studioFrame.r)
                                    root.seekVideoFrame(noteCard.modelData.frame)
                                    root.forceActiveFocus()
                                }
                            }
                        }

                        Item {   // timecode + frame number
                            width: parent.width; height: 16
                            Text {
                                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                                text: noteCard.tc
                                color: Theme.colors.textBright
                                font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeMono
                            }
                            Text {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                text: "f " + noteCard.modelData.frame
                                color: Theme.colors.textSubtle
                                font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
                            }
                        }

                        Rectangle {   // note text (commit on focus-out, QCView style)
                            width: parent.width
                            height: parent.height - noteCard.thumbH - 16 - 26 - 18   // thumb + tc row + buttons + 3×spacing
                            color: Theme.colors.surfaceRecess   // input = recessed well
                            border.width: 1
                            border.color: noteText.activeFocus ? Theme.colors.divider : Theme.colors.border
                            Flickable {
                                anchors { fill: parent; margins: 6 }
                                contentHeight: noteText.implicitHeight
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                TextEdit {
                                    id: noteText
                                    width: parent.parent.width - 12
                                    text: noteCard.modelData.text
                                    wrapMode: TextEdit.Wrap
                                    color: Theme.colors.text
                                    selectionColor: Theme.colors.divider
                                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                                    onActiveFocusChanged: if (!activeFocus) vnotes.setText(noteCard.tc, text)
                                    Keys.onEscapePressed: root.forceActiveFocus()
                                }
                            }
                            Text {   // placeholder
                                visible: noteText.text === "" && !noteText.activeFocus
                                anchors { left: parent.left; top: parent.top; margins: 6 }
                                text: "Add a note…"
                                color: Theme.colors.textSubtle
                                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                            }
                        }

                        Item {   // addressed + delete
                            width: parent.width; height: 26
                            FlatButton {
                                anchors.left: parent.left
                                height: 26
                                iconName: "check"
                                checked: noteCard.modelData.addressed
                                checkedColor: Theme.colors.divider
                                iconColor: checked ? Theme.colors.textBright : Theme.colors.textSubtle
                                tooltip: qsTr("Addressed"); tooltipSide: "top"
                                onClicked: { vnotes.setAddressed(noteCard.tc, !noteCard.modelData.addressed)
                                             root.forceActiveFocus() }
                            }
                            FlatButton {
                                anchors.right: parent.right
                                height: 26
                                iconName: "x"
                                tooltip: qsTr("Delete note"); tooltipSide: "top"
                                onClicked: { vnotes.removeNote(noteCard.tc); root.forceActiveFocus() }
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Full-frame sketch tab: the canvas centered at fit size, editable
    // with the Inspector's Draw tools (direct mouse — full-frame-tab
    // pattern). Edits commit through sketchSetShapes → beginTxn, so ⌘Z here
    // is plain DOCUMENT undo; the data binding feeds the result back. ---
    Rectangle {
        id: sketchFrame
        visible: root.activeSketchRow >= 0
        anchors.fill: parent
        color: Theme.colors.bgAlt   // full-frame tabs share the page field tone
        readonly property int r: root.activeSketchRow

        // Clicking dead space reclaims focus (restores the key swallow).
        MouseArea { anchors.fill: parent; z: -1; onClicked: root.forceActiveFocus() }

        Item {
            id: sketchStage
            anchors { fill: parent; margins: 24 }
            readonly property int vw: sketchFrame.r >= 0
                ? (blockModel.contentRevision, blockModel.mediaW(sketchFrame.r)) : 0
            readonly property int vh: sketchFrame.r >= 0 ? blockModel.mediaH(sketchFrame.r) : 0
            // Session-only camera. Fit is an ACTION, not a sticky mode: the tab
            // opens at Fit and stage/canvas resizes re-fit only until the user
            // takes the camera (pan/zoom input flips userCam).
            property bool userCam: false

            function fitCamera() {
                if (vw <= 0 || vh <= 0 || width <= 0 || height <= 0) return
                sketchEditCanvas.zoom = Math.min(width / vw, height / vh)
                sketchEditCanvas.panX = (width - vw * sketchEditCanvas.zoom) / 2
                sketchEditCanvas.panY = (height - vh * sketchEditCanvas.zoom) / 2
                userCam = false
            }
            // Zoom keeping the stage point (cx,cy) fixed; UX range 10%–800%
            // (the canvas clamps harder underneath).
            function zoomAt(cx, cy, nz) {
                nz = Math.max(0.10, Math.min(8.0, nz))
                var z = sketchEditCanvas.zoom
                if (z <= 0) return
                sketchEditCanvas.panX = cx - (cx - sketchEditCanvas.panX) * nz / z
                sketchEditCanvas.panY = cy - (cy - sketchEditCanvas.panY) * nz / z
                sketchEditCanvas.zoom = nz
                userCam = true
            }
            function zoomStep(dir) {   // ⌘+/⌘−: ×√2 about the viewport center
                zoomAt(width / 2, height / 2,
                       sketchEditCanvas.zoom * Math.pow(Math.SQRT2, dir))
            }
            function zoomTo100() { zoomAt(width / 2, height / 2, 1.0) }
            // Frame the signed ink bbox (frame + overflow) — a CAMERA move;
            // the model-side resize lives on the Inspector's Fit-to-ink.
            function fitInkCamera() {
                var b = sketchEditCanvas.contentBoundsNorm
                if (b.width <= 0 || b.height <= 0 || vw <= 0 || vh <= 0) return
                var bw = b.width * vw, bh = b.height * vh          // source px
                var nz = Math.max(0.10, Math.min(8.0,
                    Math.min(width / (bw + 16), height / (bh + 16))))
                sketchEditCanvas.zoom = nz
                sketchEditCanvas.panX = width / 2 - (b.x + b.width / 2) * vw * nz
                sketchEditCanvas.panY = height / 2 - (b.y + b.height / 2) * vh * nz
                userCam = true
            }
            onWidthChanged:  if (!userCam) fitCamera()
            onHeightChanged: if (!userCam) fitCamera()
            onVwChanged:     if (!userCam) fitCamera()
            onVhChanged:     if (!userCam) fitCamera()

            SketchCanvas {
                id: sketchEditCanvas
                anchors.fill: parent
                cameraEnabled: true   // frame floats at pan/zoom; overflow captures
                frameBorderColor: Theme.colors.divider
                fontFamily: Theme.font.body   // chips = Aspekta, the document face (ruling 2026-08-18)
                // Load-bearing revision dep (reactivity rule 1e). Resolved JSON
                // so embedded image srcs are loadable URLs.
                data: sketchFrame.r >= 0 && blockModel.contentRevision >= 0
                    ? blockModel.sketchResolvedJson(sketchFrame.r) : ""
                sourceWidth: sketchStage.vw
                sourceHeight: sketchStage.vh
                tool: (root.activeSketchRow >= 0 && root.inspector
                       && root.inspector.drawTool !== "type") ? root.inspector.drawTool : ""
                color: root.inspector ? root.inspector.drawColor : "#FF0000"
                strokeWidth: root.inspector ? root.inspector.drawWidth : 6
                selectable: true   // no tool armed → click-select / drag-move / Delete
                // Multi-select group gestures span several model calls — the
                // brackets fold them into ONE undo step.
                onGroupCommitBegan: blockModel.beginGroup(sketchFrame.r, sketchFrame.r)
                onGroupCommitEnded: blockModel.endGroup()
                onEdited: (json) => blockModel.sketchSetShapes(sketchFrame.r, json)
                onImageRectChanged: (i, x, y, w, h) => blockModel.sketchSetImageRect(sketchFrame.r, i, x, y, w, h)
                onImageRemoved: (i) => blockModel.sketchRemoveImage(sketchFrame.r, i)
                onTextBoxChanged: (i, x, y, w, s) => blockModel.sketchSetTextBox(sketchFrame.r, i, x, y, w, s)
                onTextRemoved: (i) => blockModel.sketchRemoveText(sketchFrame.r, i)
                editingTextIndex: sketchTextSession.mode === "edit" ? sketchTextSession.index : -1
                onTextCreateRequested: (nx, ny) => sketchTextSession.beginCreate(nx, ny)
                onTextEditRequested: (i) => sketchTextSession.beginEdit(i)
                onUserCameraInput: sketchStage.userCam = true
            }
            Text {   // arm hint on a fresh canvas
                visible: sketchEditCanvas.empty && !sketchEditCanvas.armed
                anchors.centerIn: parent
                text: "Pick a tool in the Draw panel to start"
                color: Theme.colors.textSubtle
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
            }
            Rectangle {   // overflow tick: ghost ink exists beyond the frame
                visible: sketchEditCanvas.hasOverflow
                width: 6; height: 6
                x: sketchEditCanvas.panX + sketchStage.vw * sketchEditCanvas.zoom + 5
                y: sketchEditCanvas.panY - 11
                color: Theme.colors.textSubtle
            }
            ZoomBadge {   // the tab's only zoom chrome (readout + menu)
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                zoomValue: sketchEditCanvas.zoom
                showFitInk: sketchEditCanvas.hasOverflow
                onFitRequested: sketchStage.fitCamera()
                onHundredRequested: sketchStage.zoomTo100()
                onFitInkRequested: sketchStage.fitInkCamera()
            }

            // --- Text-box editing session. The canvas paints COMMITTED text;
            // while a session is open the overlay TextEdit is the only visual
            // (create: no model element exists yet; edit: the element is
            // hidden from paint). Commit on click-away = ONE model txn (one
            // ⌘Z per session); Escape cancels; blank commits delete (model
            // contract); create-then-nothing touches nothing at all. ---
            QtObject {
                id: sketchTextSession
                property string mode: ""        // "" | "create" | "edit"
                property int row: -1            // captured at begin — commit may
                                                // land after a tab switch
                property int index: -1          // edit mode: element index
                property real ex: 0; property real ey: 0; property real ew: 0
                property real esize: 16
                property color ecolor: "#E4E3E2"
                property string origText: ""
                readonly property bool active: mode !== ""
                // Chip padding (derived from size, the helper's 0.4em rule).
                readonly property real padPx: esize * 0.4 * sketchEditCanvas.zoom

                function beginCreate(nx, ny) {
                    commit()                    // a canvas press can race focus-out
                    row = sketchFrame.r
                    esize = root.inspector ? root.inspector.drawTextSize : 16
                    ecolor = root.inspector ? root.inspector.drawColor : "#E4E3E2"
                    ex = nx; ey = ny
                    ew = Math.min(0.5, Math.max(240, 2 * esize) / Math.max(1, sketchStage.vw))
                    index = -1; origText = ""
                    mode = "create"
                }
                function beginEdit(i) {
                    commit()
                    row = sketchFrame.r
                    var t = sketchEditCanvas.textElementAt(i)
                    if (t.text === undefined) return
                    index = i
                    ex = t.x; ey = t.y; ew = t.w
                    esize = t.size; ecolor = t.color
                    origText = t.text
                    mode = "edit"
                }
                function commit() {
                    if (mode === "") return
                    // Close the session BEFORE the model call so the data
                    // round-trip can't re-enter it (Connections below).
                    var m = mode, r = row, i = index, orig = origText
                    var txt = sketchTextEditor.text
                    mode = ""
                    if (m === "create" && txt.trim() !== "")
                        blockModel.sketchAddText(r, ex, ey, ew, txt, esize, "" + ecolor)
                    else if (m === "edit" && txt !== orig)
                        blockModel.sketchSetText(r, i, txt)   // blank ⇒ model deletes
                    root.forceActiveFocus()
                }
                function cancel() {
                    if (mode === "") return
                    mode = ""
                    root.forceActiveFocus()
                }
            }
            Connections {
                // External data change mid-EDIT (another surface, programmatic
                // undo) → the element under the overlay is stale: cancel.
                // Create sessions have no element and just continue.
                target: sketchEditCanvas
                function onDataChanged() {
                    if (sketchTextSession.mode === "edit") sketchTextSession.cancel()
                }
            }
            Connections {
                // ANY tool change commits the session — Inspector buttons are
                // plain MouseAreas that never steal keyboard focus, so without
                // this a mid-edit tool switch leaves the overlay live and
                // typing keeps landing in it.
                target: root.inspector
                function onDrawToolChanged() { sketchTextSession.commit() }
            }
            Rectangle {   // the live CHIP: fill = element color, accent border = session
                visible: sketchTextEditor.visible
                x: sketchEditCanvas.panX + sketchTextSession.ex * sketchStage.vw * sketchEditCanvas.zoom
                y: sketchEditCanvas.panY + sketchTextSession.ey * sketchStage.vh * sketchEditCanvas.zoom
                width: sketchTextSession.ew * sketchStage.vw * sketchEditCanvas.zoom
                height: sketchTextEditor.height + 2 * sketchTextSession.padPx
                color: sketchTextSession.ecolor
                border.width: 1; border.color: Theme.colors.accent
            }
            TextEdit {
                id: sketchTextEditor
                visible: sketchTextSession.active
                x: sketchEditCanvas.panX + sketchTextSession.ex * sketchStage.vw * sketchEditCanvas.zoom
                   + sketchTextSession.padPx
                y: sketchEditCanvas.panY + sketchTextSession.ey * sketchStage.vh * sketchEditCanvas.zoom
                   + sketchTextSession.padPx
                width: Math.max(4, sketchTextSession.ew * sketchStage.vw * sketchEditCanvas.zoom
                                   - 2 * sketchTextSession.padPx)
                // Height implicit: the chip grows downward while typing, same
                // as the committed paint's derived height.
                wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                textMargin: 0
                font.family: Theme.font.body   // == the canvas's fontFamily
                font.pixelSize: Math.max(1, sketchTextSession.esize * sketchEditCanvas.zoom)
                color: sketchEditCanvas.textInkFor(sketchTextSession.ecolor)
                selectByMouse: true
                selectionColor: Theme.colors.divider
                onVisibleChanged: if (visible) {
                    text = sketchTextSession.mode === "edit" ? sketchTextSession.origText : ""
                    cursorPosition = text.length
                    forceActiveFocus()
                }
                onActiveFocusChanged: if (!activeFocus && visible) sketchTextSession.commit()
                // Escape COMMITS (the note-card island precedent — losing typed
                // text to a reflexive Esc is worse; blanking the box deletes).
                Keys.onEscapePressed: sketchTextSession.commit()
            }

            // --- Frame resize handles: 8 edge/corner grips on the canvas
            // border, live in SCREEN space at any zoom. The imgResize pattern:
            // preview outline + one-txn commit on release (the document — and
            // the ink — never move mid-drag; only the frame line does).
            // Growing left/top is the origin shift; the camera compensates on
            // commit so ink stays visually fixed.
            Item {
                id: frameResize
                anchors.fill: parent
                visible: !sketchEditCanvas.armed && !sketchEditCanvas.panMode
                readonly property real fx: sketchEditCanvas.panX
                readonly property real fy: sketchEditCanvas.panY
                readonly property real fw: sketchStage.vw * sketchEditCanvas.zoom
                readonly property real fh: sketchStage.vh * sketchEditCanvas.zoom
                property bool dragging: false
                property int dl: 0; property int dt: 0
                property int dr: 0; property int db: 0
                property int grabEdges: 0     // bitmask 1=L 2=T 4=R 8=B
                property real startX: 0; property real startY: 0
                readonly property int previewW: sketchStage.vw + dl + dr
                readonly property int previewH: sketchStage.vh + dt + db

                function snap(v) { return Math.round(v / 10) * 10 }
                function begin(edges, p) {
                    sketchTextSession.commit()   // a handle grab is a mode change
                    grabEdges = edges; startX = p.x; startY = p.y
                    dl = dt = dr = db = 0
                    dragging = true
                }
                function dragTo(p) {
                    var z = sketchEditCanvas.zoom
                    if (z <= 0) return
                    var dxs = (p.x - startX) / z, dys = (p.y - startY) / z
                    dl = (grabEdges & 1) ? snap(-dxs) : 0
                    dr = (grabEdges & 4) ? snap(dxs)  : 0
                    dt = (grabEdges & 2) ? snap(-dys) : 0
                    db = (grabEdges & 8) ? snap(dys)  : 0
                    // Hard stop at the [64, 8192] frame bounds (source px).
                    var w = sketchStage.vw + dl + dr
                    if (w < 64 || w > 8192) {
                        var cw = Math.max(64, Math.min(8192, w)) - sketchStage.vw
                        if (grabEdges & 1) dl = cw - dr; else dr = cw - dl
                    }
                    var h = sketchStage.vh + dt + db
                    if (h < 64 || h > 8192) {
                        var ch = Math.max(64, Math.min(8192, h)) - sketchStage.vh
                        if (grabEdges & 2) dt = ch - db; else db = ch - dt
                    }
                }
                function commit() {
                    if (!dragging) return
                    dragging = false
                    var _dl = dl, _dt = dt, _dr = dr, _db = db
                    dl = dt = dr = db = 0
                    if (_dl === 0 && _dt === 0 && _dr === 0 && _db === 0) return
                    var z = sketchEditCanvas.zoom
                    // userCam FIRST: the resize bumps vw/vh, whose handlers
                    // would otherwise re-fit the camera mid-commit.
                    sketchStage.userCam = true
                    blockModel.sketchResizeCanvas(sketchFrame.r, _dl, _dt, _dr, _db)
                    sketchEditCanvas.panX -= _dl * z
                    sketchEditCanvas.panY -= _dt * z
                }
                function cancel() { dragging = false; dl = dt = dr = db = 0 }

                component FrameHandle: MouseArea {
                    property int edges: 0
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: edges === 3 || edges === 12 ? Qt.SizeFDiagCursor   // TL / BR
                                 : edges === 6 || edges === 9 ? Qt.SizeBDiagCursor  // TR / BL
                                 : (edges & 5) ? Qt.SizeHorCursor : Qt.SizeVerCursor
                    onPressed: (m) => frameResize.begin(edges, mapToItem(frameResize, m.x, m.y))
                    onPositionChanged: (m) => { if (frameResize.dragging)
                        frameResize.dragTo(mapToItem(frameResize, m.x, m.y)) }
                    onReleased: frameResize.commit()
                    onCanceled: frameResize.cancel()
                }
                // Edges (thin strips straddling the border, corners excluded).
                FrameHandle { edges: 1; x: frameResize.fx - 6; y: frameResize.fy + 8
                              width: 12; height: Math.max(0, frameResize.fh - 16) }
                FrameHandle { edges: 4; x: frameResize.fx + frameResize.fw - 6; y: frameResize.fy + 8
                              width: 12; height: Math.max(0, frameResize.fh - 16) }
                FrameHandle { edges: 2; x: frameResize.fx + 8; y: frameResize.fy - 6
                              width: Math.max(0, frameResize.fw - 16); height: 12 }
                FrameHandle { edges: 8; x: frameResize.fx + 8; y: frameResize.fy + frameResize.fh - 6
                              width: Math.max(0, frameResize.fw - 16); height: 12 }
                // Corners (TL=3, TR=6, BL=9, BR=12).
                FrameHandle { edges: 3;  x: frameResize.fx - 7; y: frameResize.fy - 7; width: 14; height: 14 }
                FrameHandle { edges: 6;  x: frameResize.fx + frameResize.fw - 7; y: frameResize.fy - 7; width: 14; height: 14 }
                FrameHandle { edges: 9;  x: frameResize.fx - 7; y: frameResize.fy + frameResize.fh - 7; width: 14; height: 14 }
                FrameHandle { edges: 12; x: frameResize.fx + frameResize.fw - 7; y: frameResize.fy + frameResize.fh - 7; width: 14; height: 14 }

                // Visible nubs: corners + edge midpoints, divider tone.
                Repeater {
                    model: [[0,0],[1,0],[2,0],[0,1],[2,1],[0,2],[1,2],[2,2]]   // [col,row]
                    delegate: Rectangle {
                        required property var modelData
                        width: 6; height: 6
                        color: Theme.colors.divider
                        border.width: 1; border.color: Theme.colors.border
                        x: frameResize.fx + modelData[0] * frameResize.fw / 2 - 3
                        y: frameResize.fy + modelData[1] * frameResize.fh / 2 - 3
                    }
                }

                // Drag preview: target-frame outline + transient W×H readout.
                Rectangle {
                    visible: frameResize.dragging
                    color: "transparent"
                    border.width: 1; border.color: Theme.colors.textSubtle
                    x: frameResize.fx - frameResize.dl * sketchEditCanvas.zoom
                    y: frameResize.fy - frameResize.dt * sketchEditCanvas.zoom
                    width: frameResize.previewW * sketchEditCanvas.zoom
                    height: frameResize.previewH * sketchEditCanvas.zoom
                }
                Text {
                    visible: frameResize.dragging
                    x: frameResize.fx - frameResize.dl * sketchEditCanvas.zoom + 8
                    y: frameResize.fy - frameResize.dt * sketchEditCanvas.zoom
                       + frameResize.previewH * sketchEditCanvas.zoom + 6
                    text: frameResize.previewW + " × " + frameResize.previewH
                    color: Theme.colors.textSubtle
                    font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
                }
            }
        }
        // Every entry into a sketch tab (or switch between two) starts at Fit.
        // A live text session commits first (its row was captured at begin).
        onRChanged: {
            sketchTextSession.commit()
            if (r >= 0) sketchStage.fitCamera()
        }
    }

    // --- Margin-ink layer (tier 2 annotations). ONE viewport-sized canvas
    // over the whole document: renders every visible anchor's strokes always;
    // accepts mouse only in ink mode (disarmed it refuses events, so the
    // central mouse layer sees everything — the VideoAnnotator pattern).
    // z:45 = above document content, below the drag/media/popup overlays.
    DocInkCanvas {
        id: inkCanvas
        anchors.fill: flick
        // The scrollbar is a CHILD of the Flickable, so this sibling canvas
        // would otherwise cover it — visually and (armed) for clicks. Leave
        // its strip out of the canvas entirely: the bar stays on top and
        // stays draggable, even mid-annotation.
        anchors.rightMargin: Theme.dim.scrollBarWidth
        z: 45
        visible: flick.visible          // hidden in full-frame tabs, like the doc
        model: blockModel
        contentX: flick.contentX
        contentY: flick.contentY
        leftEdgeContent: root.leftEdge
        pageWidth: root.pageWidth
        inkMode: root.inkMode
        // The Inspector Draw trio — the exact studio/sketch binding shape.
        tool: root.inkMode && root.inspector ? root.inspector.drawTool : ""
        color: root.inspector ? root.inspector.drawColor : "#FF0000"
        strokeWidth: root.inspector ? root.inspector.drawWidth : 6
        textSize: root.inspector ? root.inspector.drawTextSize : 16
        textFamily: Theme.font.body   // chips = Aspekta, the document face (ruling 2026-08-18)
        onTextCreateRequested: (r, lx, ly, lw, lsize) => inkTextSession.beginCreate(r, lx, ly, lw, lsize)
        onTextEditRequested: (r, i) => inkTextSession.beginEdit(r, i)
        // Faded only when the user hides annotations (the eye toggle). The
        // old "squeezed page" fade is gone: the page is ALWAYS the full 760
        // measure now (narrow windows scroll horizontally instead of
        // squeezing), so ink geometry always matches its frame.
        opacity: root.inkLayerVisible ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
    }

    // --- Ink text-chip editing session: the sketchTextSession contract on
    // the margin-ink surface. The canvas paints COMMITTED chips; while a
    // session is open the overlay TextEdit is the only visual. Deferred
    // create = one setBlockInk per session; Escape COMMITS; blank deletes.
    // Coordinates are ANCHOR-LOCAL (scroll-invariant) — the geometry binding
    // re-maps to item px through the canvas placement on every scroll/layout
    // change (rule 1e deps below). ---
    QtObject {
        id: inkTextSession
        property string mode: ""        // "" | "create" | "edit"
        property int row: -1            // captured at begin
        property int index: -1
        property real ex: 0; property real ey: 0; property real ew: 0
        property real esize: 16         // anchor-local units (space-converted)
        property color ecolor: "#E4E3E2"
        property string origText: ""
        readonly property bool active: mode !== ""
        // Load-bearing reactivity deps (rule 1e): scroll + layout re-map.
        readonly property var geom: (inkCanvas.contentX, inkCanvas.contentY,
            blockModel.layoutRevision, blockModel.contentRevision,
            active ? inkCanvas.inkTextOverlayGeom(row, ex, ey, ew, esize) : null)

        function beginCreate(r, lx, ly, lw, lsize) {
            commit()                    // a canvas press can race focus-out
            row = r; index = -1
            ex = lx; ey = ly; ew = lw; esize = lsize
            ecolor = root.inspector ? root.inspector.drawColor : "#E4E3E2"
            origText = ""
            mode = "create"
        }
        function beginEdit(r, i) {
            commit()
            var t = inkCanvas.inkTextAt(r, i)
            if (t.text === undefined) return
            row = r; index = i
            ex = t.x; ey = t.y; ew = t.w
            esize = t.size; ecolor = t.color
            origText = t.text
            mode = "edit"
        }
        function commit() {
            if (mode === "") return
            // Close BEFORE the model call: the inkChanged round-trip must not
            // re-enter the session (staleness Connections below).
            var m = mode, r = row, i = index, orig = origText
            var txt = inkTextEditor.text
            mode = ""
            if (m === "create" && txt.trim() !== "")
                inkCanvas.inkAddText(r, ex, ey, ew, txt, esize, "" + ecolor)
            else if (m === "edit" && txt !== orig)
                inkCanvas.inkSetText(r, i, txt)   // blank ⇒ the model deletes
            root.forceActiveFocus()
        }
        function cancel() {
            if (mode === "") return
            mode = ""
            root.forceActiveFocus()
        }
    }
    Connections {
        // External ink change mid-EDIT (undo elsewhere, another surface) →
        // the element under the overlay is stale: cancel. Our own commit
        // closes the session before the model call, so it never lands here.
        target: blockModel
        function onInkChanged() {
            if (inkTextSession.mode === "edit") inkTextSession.cancel()
        }
    }
    Connections {
        // Any tool change commits (Inspector buttons never steal focus).
        target: root.inspector
        function onDrawToolChanged() { inkTextSession.commit() }
    }
    Item {
        anchors.fill: inkCanvas
        z: 46   // above the canvas (45), below the comment pins (56)
        visible: inkCanvas.visible && inkTextSession.active
                 && !!inkTextSession.geom && inkTextSession.geom.valid === true
        readonly property var g: inkTextSession.geom
        Rectangle {   // the live CHIP: fill = element color, accent border = session
            visible: parent.visible
            x: parent.g ? parent.g.x : 0
            y: parent.g ? parent.g.y : 0
            width: parent.g ? parent.g.w : 0
            height: inkTextEditor.height + 2 * (parent.g ? parent.g.padPx : 0)
            color: inkTextSession.ecolor
            border.width: 1; border.color: Theme.colors.accent
        }
        TextEdit {
            id: inkTextEditor
            visible: parent.visible
            x: (parent.g ? parent.g.x + parent.g.padPx : 0)
            y: (parent.g ? parent.g.y + parent.g.padPx : 0)
            width: Math.max(4, (parent.g ? parent.g.w - 2 * parent.g.padPx : 4))
            wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
            textMargin: 0
            font.family: Theme.font.body   // == the canvas's textFamily
            font.pixelSize: Math.max(1, parent.g ? parent.g.sizePx : 16)
            color: inkCanvas.textInkFor(inkTextSession.ecolor)
            selectByMouse: true
            selectionColor: Theme.colors.divider
            onVisibleChanged: if (visible) {
                text = inkTextSession.mode === "edit" ? inkTextSession.origText : ""
                cursorPosition = text.length
                forceActiveFocus()
            }
            onActiveFocusChanged: if (!activeFocus && visible) inkTextSession.commit()
            // Escape COMMITS (the island rule shipped with sketch chips).
            Keys.onEscapePressed: inkTextSession.commit()
        }
    }

    // --- Comment margin pins: one per row carrying comment spans, in the
    // right margin. Root-level + model-driven (never per-delegate), gated to
    // the visible window like the video toolbars — contentRevision dep, NOT
    // layoutRevision (the 1946-1955 rule); y reads take the layout tuple.
    // Clipped so half-scrolled rows' bubbles slide under the editor's top
    // edge instead of painting over the tab rail.
    Item {
    anchors.fill: parent
    clip: true
    z: 56
    Repeater {
        model: (blockModel.contentRevision, blockModel.commentsRevision,
                blockModel.commentPinRows())
        delegate: Rectangle {
            required property var modelData
            readonly property int prow: modelData
            visible: flick.visible && root.rowInView(prow)
            // Pins stay VISIBLE in ink mode (they mark content) but go
            // pass-through — they sit in the right margin, which is exactly
            // where margin ink lands, and must not steal the pen.
            enabled: !root.inkMode
            z: 56
            width: 24; height: 24
            x: root.leftEdge + root.pageWidth + 12 - flick.contentX
            y: (blockModel.layoutRevision, blockModel.yForRow(prow)) - flick.contentY + 2
            // The comment BUBBLE (user ruling 2026-07-12): the margin is real
            // space now, and blue = "a conversation lives here" — one of the
            // few semantic accent uses. A block whose threads are ALL
            // resolved goes quiet grey (2026-08-21): the conversation ended.
            readonly property bool allResolved: {
                var dep = blockModel.commentsRevision + blockModel.contentRevision
                var rs = blockModel.commentRangesForRow(prow)
                if (rs.length === 0) return false
                for (var i = 0; i < rs.length; ++i)
                    if (!rs[i].resolved) return false
                return true
            }
            color: allResolved
                   ? (pinMA.containsMouse ? Theme.colors.surfaceHover : Theme.colors.divider)
                   : (pinMA.containsMouse ? Theme.colors.accentHover : Theme.colors.accent)
            Icon { anchors.centerIn: parent; name: "chat-circle-text"; weight: "fill"; size: 14
                   color: parent.allResolved ? Theme.colors.textMuted : Theme.colors.textBright }
            MouseArea {
                id: pinMA
                anchors.fill: parent; hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                // The bubble toggles its thread's MARGIN CARD (the Inspector
                // stays the all-threads overview).
                onClicked: {
                    var rs = blockModel.commentRangesForRow(prow)
                    if (rs.length === 0) return
                    root.openThreadId = (root.openThreadId === rs[0].id) ? "" : rs[0].id
                }
            }
        }
    }
    }

    // --- Margin thread card: click a comment bubble → the conversation opens
    // IN the margin, anchored to its block (it rides the scroll — a margin
    // note, not a dialog). One card at a time; click-away or Esc-in-field
    // closes; the Inspector remains the all-threads overview.
    property string openThreadId: ""
    function closeThreadCard() {
        if (openThreadId !== "") { openThreadId = ""; forceActiveFocus() }
    }
    Item {
        anchors.fill: parent
        clip: true
        z: 58
        visible: root.openThreadId !== "" && flick.visible
        Rectangle {
            id: threadCard
            // Arithmetic dep (elision-proof) — re-resolve the thread on any
            // comment/content change; null when the thread vanished.
            readonly property var info: {
                var dep = blockModel.commentsRevision + blockModel.contentRevision
                if (root.openThreadId === "") return null
                var ts = blockModel.commentThreads()
                for (var i = 0; i < ts.length; ++i)
                    if (ts[i].id === root.openThreadId) return ts[i]
                return null
            }
            readonly property int arow: info ? info.row : -1
            // Hidden in ink mode: a card left open would swallow pen strokes
            // over a 300px patch of the margin (pins are pass-through there).
            visible: info !== null && arow >= 0 && !root.inkMode
            // Beside the bubble; clamped into the viewport when the window is
            // narrower than the margin (the card may float over the page edge).
            x: Math.min(root.leftEdge + root.pageWidth + 12 - flick.contentX,
                        root.width - width - Theme.dim.scrollBarWidth - 40)
            y: (blockModel.layoutRevision, arow >= 0 ? blockModel.yForRow(arow) : 0)
               - flick.contentY + 2
            width: 300
            height: cardCol.implicitHeight + 20
            color: Theme.colors.surfaceRaised
            border.width: 1; border.color: Theme.colors.border
            MouseArea { anchors.fill: parent }   // swallow — clicks stay in the card

            Column {
                id: cardCol
                x: 10; y: 10; width: parent.width - 20
                spacing: 8
                Item {   // header: anchored excerpt + the rail's block address
                    width: parent.width; height: 16
                    Text {
                        width: parent.width - 64
                        elide: Text.ElideRight
                        text: threadCard.info
                              ? "“" + threadCard.info.excerpt + "”" : ""
                        color: Theme.colors.textMuted
                        font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                        font.italic: true
                    }
                    Text {
                        anchors.right: parent.right
                        text: threadCard.arow >= 0 ? qsTr("block %1").arg(threadCard.arow + 1) : ""
                        color: Theme.colors.textSubtle
                        font.family: Theme.font.mono; font.pixelSize: 11
                    }
                }
                Flickable {   // messages — own scroll when the thread runs long
                    width: parent.width
                    height: Math.min(msgCol.implicitHeight, 260)
                    contentHeight: msgCol.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    Column {
                        id: msgCol
                        width: parent.width
                        spacing: 6
                        Repeater {
                            // Revision read is LOAD-BEARING (rule 1e).
                            model: blockModel.commentsRevision >= 0 && root.openThreadId !== ""
                                   ? blockModel.commentMessages(root.openThreadId) : []
                            delegate: Column {
                                required property var modelData
                                width: msgCol.width
                                Text {
                                    width: parent.width; wrapMode: Text.Wrap
                                    text: modelData.body
                                    color: Theme.colors.text
                                    font.family: Theme.font.family
                                    font.pixelSize: Theme.font.sizeSmall
                                }
                                Text {
                                    text: modelData.created > 0
                                          ? new Date(modelData.created)
                                                .toLocaleString(Qt.locale(), "yyyy-MM-dd hh:mm")
                                          : ""
                                    color: Theme.colors.textSubtle
                                    font.family: Theme.font.family; font.pixelSize: 11
                                }
                            }
                        }
                    }
                }
                Rectangle {   // reply — input = recessed well, the family rule
                    width: parent.width
                    height: Math.max(34, replyEdit.implicitHeight + 12)
                    color: Theme.colors.surfaceRecess
                    TextEdit {
                        id: replyEdit
                        anchors.fill: parent; anchors.margins: 6
                        wrapMode: TextEdit.Wrap
                        color: Theme.colors.text
                        selectionColor: Theme.colors.selectionBg
                        selectByMouse: true
                        font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                        Keys.onEscapePressed: root.closeThreadCard()
                        // Enter submits; Shift+Enter = newline (user ruling
                        // 2026-08-21 — the chat-composer convention).
                        Keys.onPressed: (e) => {
                            if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter)
                                && !(e.modifiers & Qt.ShiftModifier)) {
                                if (replyEdit.text.trim().length > 0) {
                                    blockModel.addCommentMessage(root.openThreadId, replyEdit.text.trim())
                                    replyEdit.text = ""
                                }
                                e.accepted = true
                            }
                        }
                        Text {
                            visible: replyEdit.text.length === 0 && !replyEdit.activeFocus
                            text: qsTr("Reply…")
                            color: Theme.colors.textSubtle
                            font: replyEdit.font
                        }
                    }
                }
                Row {
                    spacing: 6
                    anchors.right: parent.right
                    FlatButton {
                        text: threadCard.info && threadCard.info.resolved
                              ? qsTr("Reopen") : qsTr("Resolve")
                        padding: 8; labelSize: Theme.font.sizeSmall
                        onClicked: blockModel.setThreadResolved(
                                       root.openThreadId,
                                       !(threadCard.info && threadCard.info.resolved))
                    }
                    FlatButton {
                        text: qsTr("Submit"); variant: "primary"
                        padding: 8; labelSize: Theme.font.sizeSmall
                        enabled_: replyEdit.text.trim().length > 0
                        onClicked: {
                            blockModel.addCommentMessage(root.openThreadId, replyEdit.text.trim())
                            replyEdit.text = ""
                        }
                    }
                }
            }
        }
    }

    // --- Block-number ruler: a quiet instrument rail at the viewport's right
    // edge (inside the vertical scrollbar) — one mono number per visible
    // block, the margin's shared address system ("block 14"). Display-only.
    // Pool-math delegates (stable per index, like the block pool) so
    // scrolling repositions instead of recreating.
    Item {
        id: blockRuler
        visible: flick.visible
        z: 44
        // Clip: rows half-scrolled off the top otherwise paint their numbers
        // at negative y, over the tab rail above the editor.
        clip: true
        anchors { top: parent.top; bottom: parent.bottom; right: parent.right
                  rightMargin: Theme.dim.scrollBarWidth }
        width: 34
        // YIELDS TO THE PAGE: in a narrow window this viewport-anchored rail
        // slides over the left-anchored text column and prints numbers on the
        // prose. When its slot would cross the page's right edge (viewport
        // coords — panning right re-clears it), fade out and release the
        // mouse so the invisible drag handles can't eat clicks on text.
        // (Riding over the DESK and wide tables' paper stays by design.)
        readonly property bool clearOfPage:
            x >= root.leftEdge + root.pageWidth - flick.contentX + 8
        opacity: clearOfPage ? 1 : 0
        enabled: clearOfPage
        Behavior on opacity { NumberAnimation { duration: 150 } }
        // (No backing, no own stripes: the rail rides transparently on the
        // desk's zebra — wide tables passing beneath carry their own paper.)
        // The numbers live in CONTENT coordinates inside one item translated by -contentY: one binding
        // per scroll frame instead of 480 (2026-09-15: every number re-ran its y and setY per frame).
        Item {
            width: parent.width
            y: -flick.contentY
        Repeater {
            model: root.noRail ? 0 : poolModel   // grows by insertion: never a full regenerate
            delegate: Item {
                id: rnum
                required property int index
                readonly property int prow: (root.slotRev, viewSlots.rowForSlot(index))
                visible: prow >= 0 && prow < blockModel.count
                         && (blockModel.contentRevision, blockModel.laneForRow(prow)) < 0   // top entries only
                         && (root.frameLo < 0 || (prow >= root.frameLo && prow <= root.frameHi))
                         && !(blockModel.layoutRevision, blockModel.rowHidden(prow))                // T4 filter
                width: blockRuler.width
                height: Math.max(16, (blockModel.layoutRevision, blockModel.heightForRow(prow)))
                y: (blockModel.layoutRevision, blockModel.yForRow(prow))
                // Being dragged → the rail chip is the block's body; its slot dims.
                opacity: root.blockDragging && rnum.prow >= root.blockDragRow
                         && rnum.prow < root.blockDragRow + root.blockDragCount ? 0.3 : 1
                // A DOT, not a number (2026-09-15 walk): with cells as blocks the numbering counted
                // every cell, and a Text per rail slot laid out and built glyph nodes on every rebind.
                Rectangle {
                    x: parent.width - 8 - width; y: 6
                    width: 6; height: 6; radius: 3
                    color: rnum.prow === cursor.focusRow ? Theme.colors.textMuted : Theme.colors.textSubtle
                    opacity: rnum.prow === cursor.focusRow ? 1.0 : 0.55
                }
                MouseArea {   // the number is the block's HANDLE — drag to
                              // reorder (the left gutter's twin; reuses the
                              // whole blockDrag lifecycle incl. auto-scroll)
                    anchors.fill: parent
                    // NOT hoverEnabled (2026-09-15): 480 hover areas made every mouse and wheel event
                    // a hit-test over all of them. The cursor shape needs no hover; the drag needs none.
                    hoverEnabled: false
                    cursorShape: root.blockDragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    property real pressY: 0
                    onPressed: (m) => { pressY = m.y }
                    onPositionChanged: (m) => {
                        if (!pressed) return
                        var vy = mapToItem(root, m.x, m.y).y
                        if (!root.blockDragging) {
                            if (Math.abs(m.y - pressY) < 4) return   // click ≠ drag
                            // A number inside the selection drags the whole run.
                            if (cursor.hasSel && rnum.prow >= cursor.loRow && rnum.prow <= cursor.hiRow) {
                                root.blockDragRow = cursor.loRow
                                root.blockDragCount = cursor.hiRow - cursor.loRow + 1
                            } else {
                                // A split row's number carries the whole split row; a table header
                                // row's, its whole table (SR-4 S7b).
                                const run = blockModel.typeForRow(rnum.prow) === 10 ? root.dragRunFor(rnum.prow) : [rnum.prow, 1]
                                root.blockDragRow = run[0]
                                root.blockDragCount = run[1]
                            }
                            root.blockDragging = true
                        }
                        root.blockDragViewY = vy
                        root.blockDragX = mapToItem(root, m.x, m.y).x + flick.contentX
                        root.aimBlockDrag(root.blockDragX, vy + flick.contentY)
                    }
                    onReleased: if (root.blockDragging) root.commitBlockDrag()
                }
            }
        }
        }   // the -contentY translation
        Rectangle {   // the drag's BODY: a zebra-toned chip riding the rail
                      // under the cursor (the content area shows only the
                      // drop-line locator — no ghosted content).
            visible: root.blockDragging
            z: 2
            width: parent.width; height: 22
            y: root.blockDragViewY - height / 2
            color: Theme.colors.bgAlt
            border.width: 1; border.color: Theme.colors.accent
            Text {
                anchors.centerIn: parent
                text: root.blockDragCount > 1 ? root.blockDragCount + " blocks" : "block"
                color: Theme.colors.textBright
                font.family: Theme.font.mono; font.pixelSize: 11
            }
        }
    }

    // --- Block-drag overlays (viewport-fixed, on top of the document) ---
    // Drop-indicator line at the insertion gap — FULL WIDTH, page through
    // desk to the rail: the insertion is a document-wide event, and the line
    // meets the drag chip riding the ruler.
    // Lane gesture previews (SR-3 S7b): the divider being dragged, spanning its aligned
    // chain, and the divider a pull would make. Previews only — the model commits on release.
    Rectangle {
        readonly property int topRec: root.dividerDragChain.length >= 2 ? root.dividerDragChain[0] : -1
        readonly property int lastRec: root.dividerDragChain.length >= 2
                                       ? root.dividerDragChain[root.dividerDragChain.length - 2] : -1
        visible: root.dividerDragging && topRec >= 0
        x: root.tableX(blockModel.tableHeadOf(root.dividerDragRecord)) + root.dividerPreviewX - 1 - flick.contentX
        y: (blockModel.layoutRevision, topRec >= 0 ? blockModel.yForRow(topRec) : 0) - flick.contentY
        width: 2
        height: (blockModel.layoutRevision, lastRec >= 0
                 ? blockModel.yForRow(lastRec) + blockModel.heightForRow(lastRec) - blockModel.yForRow(topRec) : 0)
        color: Theme.colors.accent
        z: 50
    }
    Rectangle {
        visible: root.pulling && Math.abs(root.pullPreviewX - root.pullPressX) >= 12
        x: root.leftEdge + root.pullPreviewX - 1 - flick.contentX
        y: (blockModel.layoutRevision, root.pullLo >= 0 ? blockModel.yForRow(root.pullLo) : 0) - flick.contentY
        width: 2
        height: (blockModel.layoutRevision, root.pullHi >= 0
                 ? blockModel.yForRow(root.pullHi) + blockModel.heightForRow(root.pullHi) - blockModel.yForRow(root.pullLo) : 0)
        color: Theme.colors.accent
        z: 50
    }
    Rectangle {
        visible: root.blockDragging && root.dropGap >= 0 && !root.dropGapIsNoop(root.dropGap)
        // A top-level gap spans the field; a lane gap only its lane (SR-3 S7c).
        readonly property var geom: (blockModel.layoutRevision, blockModel.contentRevision,
                                     root.dropLineGeom(root.dropGap, root.dropLane))
        x: geom.x - flick.contentX
        width: geom.w >= 0 ? geom.w : root.width - x
        height: 2; radius: 0
        y: geom.y - flick.contentY - 1
        color: Theme.colors.accent
        z: 50
    }
    Rectangle {   // side-edge drop (SR-3 S7c): a bar on the edge the dropped block or file will sit beside
        readonly property int row: root.blockDragging ? root.dropBesideRow
                                 : root.imageDropActive ? root.imageDropBesideRow : -1
        readonly property int side: root.blockDragging ? root.dropBesideSide : root.imageDropBesideSide
        visible: row >= 0
        x: (row >= 0 ? root.columnX(row) + (side === 0 ? root.laneOf(row).w - 3 : 0) : 0) - flick.contentX
        y: (blockModel.layoutRevision, row >= 0 ? blockModel.yForRow(row) : 0) - flick.contentY
        width: 3
        height: (blockModel.layoutRevision, row >= 0 ? blockModel.heightForRow(row) : 0)
        color: Theme.colors.accent
        z: 50
    }
    // (The content-area floating ghost is GONE — user ruling 2026-07-12:
    // content shows only the drop-line locator above; the drag's "body"
    // lives on the ruler as a rail chip, over the zebra.)

    // Image-drop insertion indicator — a pulsing accent line at the snapped gap,
    // with an expanding ring on a solid dot, so it's obvious where a dragged image
    // will land. Follows the cursor between blocks as you drag.
    Item {
        visible: root.dropIndicatorActive && root.dropIndicatorGap >= 0
        z: 55
        // A lane gap draws at its lane (SR-3 S7c); a top-level gap across the page.
        readonly property var geom: (blockModel.layoutRevision, blockModel.contentRevision,
            root.dropLineGeom(root.dropIndicatorGap, root.imageDropActive ? root.imageDropLane : -1))
        readonly property real lineY: geom.y - flick.contentY
        readonly property real lineX: (geom.w >= 0 ? geom.x : root.leftEdge) - flick.contentX
        readonly property real lineW: geom.w >= 0 ? geom.w : root.pageWidth

        Rectangle {   // insertion line
            // (was `root.textWidth` — an undefined property; the line had no
            // width since forever. Surfaced by the left-anchor survey.)
            x: parent.lineX; width: parent.lineW; height: 3; radius: 0
            y: parent.lineY - 1.5
            Behavior on y { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
            color: Theme.colors.accent
            SequentialAnimation on opacity {
                running: root.dropIndicatorActive; loops: Animation.Infinite
                NumberAnimation { from: 1.0; to: 0.45; duration: 550; easing.type: Easing.InOutQuad }
                NumberAnimation { from: 0.45; to: 1.0; duration: 550; easing.type: Easing.InOutQuad }
            }
        }
        Rectangle {   // solid dot at the left end
            x: parent.lineX - 4; y: parent.lineY - 4; width: 8; height: 8; radius: 4
            color: Theme.colors.accent
            Behavior on y { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
        }
        Rectangle {   // expanding ring emanating from the dot
            id: dropRing
            x: parent.lineX - 4; y: parent.lineY - 4; width: 8; height: 8; radius: 4
            Behavior on y { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
            color: "transparent"; border.width: 2; border.color: Theme.colors.accent
            transformOrigin: Item.Center
            SequentialAnimation on scale {
                running: root.dropIndicatorActive; loops: Animation.Infinite
                NumberAnimation { from: 0.7; to: 2.6; duration: 900; easing.type: Easing.OutQuad }
            }
            SequentialAnimation on opacity {
                running: root.dropIndicatorActive; loops: Animation.Infinite
                NumberAnimation { from: 0.8; to: 0.0; duration: 900; easing.type: Easing.OutQuad }
            }
        }
    }

    // --- Inline video: ONE rendering surface over the ACTIVE player, plus a
    // persistent transport toolbar under EVERY visible video (a player-card
    // look — the toolbar is always up, even before first play). Both are root
    // overlays above the central mouse layer (z:56) so the controls take
    // clicks; every video block reserves videoTransportH so the toolbar sits in
    // real layout space, not over following content. ---
    Item {
        id: videoSurfaceOverlay
        visible: root.videoVisible && root._videoSurfaceReady
        z: 56
        readonly property int r: root.videoPlayingRow
        readonly property real measure: r >= 0 ? root.measureForRow(r) : root.pageWidth
        readonly property int vw: r >= 0 ? blockModel.mediaW(r) : 0
        readonly property int vh: r >= 0 ? blockModel.mediaH(r) : 0
        readonly property real dispW: vw > 0 ? Math.min(measure, vw) : measure
        readonly property real dispH: (vw > 0 && vh > 0) ? Math.round(dispW * vh / vw)
                                                         : Math.round(dispW * 0.5)
        x: (r >= 0 ? root.columnX(r) : root.leftEdge) - flick.contentX   // the block's lane column
        y: (blockModel.layoutRevision, r >= 0 ? blockModel.yForRow(r) : 0) - flick.contentY + 6
        width: dispW; height: dispH

        VideoSurfaceItem {
            id: videoSurface
            anchors.fill: parent
            videoDecoder: videoDec
            fillColor: Qt.rgba(Theme.colors.bgAlt.r, Theme.colors.bgAlt.g,
                               Theme.colors.bgAlt.b, 1.0)   // letterbox = the page field tone
        }
        // No click-to-play on the frame — the toolbar is the sole transport.

        // ---- QCView note presentation (read-only; editing lives in the studio) ----
        readonly property int curFrame: videoDec.currentFrame
        // The caption's note, or null. Notes pin to ONE frame by design, so
        // during playback/shuttle/scrub the TEXT lingers ~2.5 s after crossing
        // the frame (a one-frame flash is unreadable) — while a paused
        // frame-step tracks exactly. Strokes stay strictly per-frame.
        property var capNote: null
        function _noteAt(f) {
            var a = root.videoNoteArr
            for (var i = 0; i < a.length; ++i) if (a[i].frame === f) return a[i]
            return null
        }
        function _updateCaption() {
            var n = _noteAt(curFrame)
            if (n) { capNote = n; capLinger.restart() }
            else if (!(videoDec.isPlaying || root._scrubAudioActive
                       || root._vidFastSeekDir !== 0)) {
                capNote = null; capLinger.stop()
            }
        }
        onCurFrameChanged: _updateCaption()
        onRChanged: { capNote = null; capLinger.stop() }
        readonly property var _noteDep: root.videoNoteArr   // note edits/deletes refresh the caption
        on_NoteDepChanged: _updateCaption()
        Timer {
            id: capLinger; interval: 2500
            onTriggered: if (!videoSurfaceOverlay._noteAt(videoSurfaceOverlay.curFrame))
                             videoSurfaceOverlay.capNote = null
        }

        // The stroke layer: the studio's exact overlay (same normalized
        // space, same painter) but DISABLED — it never takes mouse.
        VideoAnnotator {
            anchors.fill: parent
            visible: !root.annotationsHidden
            enabled: false
            notes: vnotes
            sourceWidth: videoSurfaceOverlay.vw
            frame: videoSurfaceOverlay.curFrame
            tool: ""
        }

        // Note caption: subtitle-style strip on the frame's bottom edge —
        // timecode + text, single line (the studio filmstrip holds the long
        // form). Addressed notes get the muted check.
        Rectangle {
            visible: !root.annotationsHidden && videoSurfaceOverlay.capNote !== null
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 26
            color: Qt.rgba(0, 0, 0, 0.62)
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8; anchors.rightMargin: 8
                spacing: 8
                Icon {
                    visible: videoSurfaceOverlay.capNote !== null
                             && videoSurfaceOverlay.capNote.addressed === true
                    name: "check"; size: 12; color: Theme.colors.textMuted
                }
                Text {
                    text: videoSurfaceOverlay.capNote ? videoSurfaceOverlay.capNote.timecode : ""
                    color: Theme.colors.textMuted
                    font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
                }
                Text {
                    Layout.fillWidth: true
                    text: videoSurfaceOverlay.capNote ? videoSurfaceOverlay.capNote.text : ""
                    color: Theme.colors.textBright
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
                    elide: Text.ElideRight
                }
            }
        }
    }

    // Persistent transport toolbar for EVERY video (all built on load — see
    // allVideoRows). `live` = this row is the active player (controls bound to
    // the decoder); otherwise the bar shows the static state and any control
    // activates the video first. Only viewport-near toolbars are shown; the rest
    // stay instantiated (no scroll churn) but hidden.
    Repeater {
        model: root.allVideoRows
        delegate: Item {
            id: vbar
            required property int modelData
            readonly property int row: modelData
            readonly property bool live: row === root.videoPlayingRow
            // Fade the toolbar back while this block is selected (focused, in a
            // range, or the open context menu's target) so the selection/menu
            // highlight reads clearly and the bright bar doesn't dominate above it
            // — but stay full while it's the live player (you're using the controls).
            readonly property bool blockSelected: (cursor.hasSel
                ? (row >= cursor.loRow && row <= cursor.hiRow)
                : (row === cursor.focusRow))
                || (blockMenu.visible && row === root.menuRow)
            // Ink mode: the bar stays VISIBLE (it sits under the canvas, so
            // the pen can't hit it — z handles the routing) but is disabled
            // and dimmed: no playback/scrub state changes mid-annotation.
            opacity: root.inkMode ? 0.5 : ((blockSelected && !live) ? 0.35 : 1.0)
            enabled: !root.inkMode
            Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutQuad } }
            visible: root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0
                     && dispW >= root.transportMinW   // T5: no transport in a narrow cell (a click opens the review view)
                     && root.rowInView(row)
            readonly property real measure: root.measureForRow(row)
            readonly property int vw: (blockModel.contentRevision, blockModel.mediaW(row))
            readonly property int vh: blockModel.mediaH(row)
            readonly property real dispW: vw > 0 ? Math.min(measure, vw) : measure
            readonly property real dispH: (vw > 0 && vh > 0) ? Math.round(dispW * vh / vw)
                                                             : Math.round(dispW * 0.5)

            // BELOW the ink canvas (45): committed strokes render over the bar
            // in regular mode too, so ink never shifts or vanishes between
            // modes (and matches the HTML export, which has no bars). Clicks
            // still reach the bar — the disarmed canvas refuses mouse.
            z: 44
            x: root.columnX(row) - flick.contentX
            y: (blockModel.layoutRevision, blockModel.yForRow(row)) + 6 + dispH - flick.contentY
            width: dispW
            height: root.videoTransportH

            VideoTransport {
                anchors.fill: parent
                editor: root; dec: videoDec; audio: videoAudio
                row: vbar.row; live: vbar.live
            }
        }
    }

    // Page-nav strip for every inline PDF (built on load like the video bars,
    // positioned in the reserved kPdfNav space just below the page). Prev/Next
    // change this block's current page; the page indicator sits on the right.
    Repeater {
        model: root.allPdfRows
        delegate: Rectangle {
            id: pbar
            required property int modelData
            readonly property int row: modelData
            readonly property bool blockSelected: (cursor.hasSel
                ? (row >= cursor.loRow && row <= cursor.hiRow)
                : (row === cursor.focusRow))
                || (blockMenu.visible && row === root.menuRow)
            // Ink mode: visible-but-disabled like the video bars (z routes the
            // pen to the canvas above) — Prev/Next must not flip the page
            // UNDER a block-pinned annotation mid-draw; the dim says "paused".
            opacity: root.inkMode ? 0.5 : (blockSelected ? 0.35 : 1.0)
            enabled: !root.inkMode
            Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutQuad } }
            visible: root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0
                     && root.rowInView(row)
            readonly property real measure: root.measureForRow(row)
            readonly property int vw: (blockModel.contentRevision, blockModel.mediaW(row))
            readonly property int vh: blockModel.mediaH(row)
            readonly property real dispH: (vw > 0 && vh > 0) ? Math.round(measure * vh / vw)
                                                             : Math.round(measure * 1.3)
            readonly property int pages: (blockModel.contentRevision, blockModel.mediaPdfPages(row))
            readonly property int page: (root.pdfPageRev, root.pdfPageFor(row))

            // z:44 like the video bars — ink paints over the strip in regular
            // mode (never occluded, matches export); the bar stays clickable.
            z: 44
            x: root.columnX(row) - flick.contentX
            y: (blockModel.layoutRevision, blockModel.yForRow(row)) + 6 + dispH - flick.contentY
            width: measure
            height: root.pdfNavH
            color: Theme.colors.surfaceRaised      // raised bar — tone separates from the page above (border diet)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6; anchors.rightMargin: 10
                spacing: 0
                FlatButton { iconName: "caret-left"; tooltip: qsTr("Previous page"); tooltipSide: "top"
                    enabled_: pbar.page > 0; onClicked: root.pdfStep(pbar.row, -1) }
                FlatButton { iconName: "caret-right"; tooltip: qsTr("Next page"); tooltipSide: "top"
                    enabled_: pbar.page < pbar.pages - 1; onClicked: root.pdfStep(pbar.row, 1) }
                Item { Layout.fillWidth: true }
                Text {
                    text: "Page " + (pbar.page + 1) + " of " + pbar.pages
                    color: Theme.colors.textMuted
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                }
            }
        }
    }

    // Image resize affordances (redesigned 2026-09-15, user ruling: "a traditional outline with 4
    // corners"): a 1px outline around the hovered / selected image with a square handle at each
    // corner. Drag ANY corner to resize proportionally — the image stays anchored at its lane's
    // left edge, so a left-corner drag grows the image the same way a right one does (outward =
    // bigger). Double-click a corner: fit the lane width; again: back to the intrinsic size.
    // Root overlays (above the central mouse layer); images only (kind "image"); Document view only.
    Item {
        id: imgResize
        // Show while resizing, while hovering the image, OR while the image is the selected block
        // — so a click (which selects it) can't make the handles vanish.
        readonly property int row: root.imageResizing ? root.imageResizeRow
            : (root.imgHandleRow >= 0 ? root.imgHandleRow
               : (root._isResizableMediaRow(cursor.focusRow) ? cursor.focusRow : -1))
        // Also hidden in ink mode: a still-selected image's handles would sit
        // above the canvas and let the pen RESIZE the layout under the ink.
        visible: row >= 0 && !root.inkMode
                 && root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0
        // A cell's media sits inside the cell inset (BlockView's colLeft: 8 px); the fit width is the
        // lane minus both insets.
        readonly property real inset: (blockModel.contentRevision, row >= 0 && blockModel.tableColumnOf(row) >= 0) ? 8 : 0
        readonly property real imgX: (row >= 0 ? root.columnX(row) : root.leftEdge) + inset - flick.contentX
        readonly property real imgTopV: row >= 0
            ? (blockModel.layoutRevision, blockModel.yForRow(row)) + 6 - flick.contentY : 0
        readonly property real imgW: row >= 0
            ? (blockModel.layoutRevision, blockModel.mediaDispWidth(row)) : 0
        readonly property real imgH: row >= 0
            ? (blockModel.layoutRevision, blockModel.mediaDisplayHeight(row)) : 0
        z: 57

        Rectangle {   // the outline (the ghost below takes over during a drag)
            visible: !root.imageResizing
            x: imgResize.imgX; y: imgResize.imgTopV; width: imgResize.imgW; height: imgResize.imgH
            color: "transparent"
            border.width: 1; border.color: Theme.colors.accent
        }
        Repeater {   // four corner handles: 0 TL, 1 TR, 2 BR, 3 BL
            model: 4
            Item {
                id: corner
                required property int index
                readonly property bool onLeft: index === 0 || index === 3
                readonly property bool onTop: index < 2
                width: 18; height: 18   // hit area; the visible square is 9 px
                x: imgResize.imgX + (onLeft ? 0 : imgResize.imgW) - width / 2
                y: imgResize.imgTopV + (onTop ? 0 : imgResize.imgH) - height / 2
                Rectangle {
                    visible: !root.imageResizing
                    anchors.centerIn: parent
                    width: 9; height: 9; radius: 0
                    color: cornerMA.containsMouse ? Theme.colors.textBright : Theme.colors.accent   // accent at rest (user ruling: not bright)
                    border.width: 1; border.color: Theme.colors.accent
                }
                MouseArea {
                    id: cornerMA
                    anchors.fill: parent; hoverEnabled: true; preventStealing: true
                    cursorShape: (corner.onLeft === corner.onTop) ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
                    onPressed: (m) => {
                        // Capture the target row + start geometry BEFORE flipping imageResizing —
                        // imgResize.row depends on it, so setting it first would re-evaluate row to -1.
                        root.imageResizeRow = imgResize.row
                        root._imgResizePressX = m.x
                        root._imgResizeStartW = imgResize.imgW
                        root._imgResizeSign = corner.onLeft ? -1 : 1   // a left corner grows leftward
                        root.imageResizeW = imgResize.imgW
                        root.imageResizeAspect = imgResize.imgW > 0 ? imgResize.imgH / imgResize.imgW : 1
                        root.imageResizing = true
                    }
                    onPositionChanged: (m) => {
                        if (!root.imageResizing) return
                        // No page cap (user ruling): the drag can take an image past the 760
                        // measure — the reachable screen is the practical limit, and the page
                        // h-scroll holds the rest.
                        root.imageResizeW = Math.max(80,
                            root._imgResizeStartW + root._imgResizeSign * (m.x - root._imgResizePressX))
                    }
                    onReleased: {
                        if (root.imageResizing) {
                            blockModel.setMediaWidth(root.imageResizeRow, Math.round(root.imageResizeW))
                            root.imageResizing = false; root.imageResizeRow = -1
                        }
                    }
                    onCanceled: { root.imageResizing = false; root.imageResizeRow = -1 }
                    onDoubleClicked: {   // fit the lane (the page at top level); at the fit already → intrinsic
                        const fit = root.fitWidthForRow(imgResize.row)
                        blockModel.setMediaWidth(imgResize.row, Math.abs(imgResize.imgW - fit) < 1 ? 0 : fit)
                    }
                }
            }
        }
    }

    // Resize ghost: a target-size outline that follows the drag WITHOUT reflowing
    // the document (committed on release) — so indecisive dragging never stutters.
    Rectangle {
        visible: root.imageResizing
        z: 58
        x: (root.imageResizeRow >= 0 ? root.columnX(root.imageResizeRow) : root.leftEdge) + imgResize.inset - flick.contentX
        y: (blockModel.layoutRevision, root.imageResizeRow >= 0
            ? blockModel.yForRow(root.imageResizeRow) : 0) + 6 - flick.contentY
        width: root.imageResizeW
        height: root.imageResizeW * root.imageResizeAspect
        color: "transparent"
        border.width: 1; border.color: Theme.colors.accent
        radius: 0
        Repeater {   // the four corners travel with the ghost
            model: 4
            Rectangle {
                required property int index
                x: (index === 0 || index === 3 ? 0 : parent.width) - 4.5
                y: (index < 2 ? 0 : parent.height) - 4.5
                width: 9; height: 9; radius: 0
                color: Theme.colors.accent
                border.width: 1; border.color: Theme.colors.accent
            }
        }
        Rectangle {
            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 10
            width: dimLabel.width + 10; height: dimLabel.height + 6; radius: 0
            color: Qt.rgba(0, 0, 0, 0.7)
            Text {
                id: dimLabel; anchors.centerIn: parent
                text: Math.round(root.imageResizeW) + " × " + Math.round(root.imageResizeW * root.imageResizeAspect)
                color: Theme.colors.textBright; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
            }
        }
    }

    // Context-menu target highlight (whole block) — tints the block the hovered
    // menu item will act on (red for destructive). Column/row scopes are drawn
    // inside the table itself. Document view only (the menu opens there).
    // Full FIELD like the focus fill and the multi-block selection band (one
    // row treatment); covers the whole selected run when the menu row is in it.
    Rectangle {
        visible: root.menuHiScope === "block" && root.menuRow >= 0 && root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0
        x: -flick.contentX
        y: (blockModel.layoutRevision, blockModel.yForRow(blockMenu.runLo)) - flick.contentY
        width: Math.max(flick.width, root.contentSpan)
        height: (blockModel.layoutRevision, blockModel.yForRow(blockMenu.runHi) + blockModel.heightForRow(blockMenu.runHi)
                 - blockModel.yForRow(blockMenu.runLo))
        z: 45
        readonly property color _c: root.menuHiDanger ? Theme.colors.error : Theme.colors.accent
        color: Qt.rgba(_c.r, _c.g, _c.b, 0.10)
        border.width: 1; border.color: Qt.rgba(_c.r, _c.g, _c.b, 0.55)
        radius: Theme.dim.radius
    }

    // A column grip drag's drop line (SR-4 S7b): the gap it would land at, down the whole table.
    Rectangle {
        readonly property int head: root.tableColDragging ? root.tableGripPressHead : -1
        readonly property var recs: head >= 0 ? (blockModel.contentRevision, blockModel.tableRecords(head)) : []
        readonly property int lastRec: recs.length ? recs[recs.length - 1] : -1
        readonly property int gap: root.tableColGap
        visible: lastRec >= 0 && gap >= 0 && gap !== root.tableGripPressIndex && gap !== root.tableGripPressIndex + 1
        x: root.tableX(head) - flick.contentX - 1.5 + (blockModel.layoutRevision, lastRec < 0 || gap < 0 ? 0
            : gap >= blockModel.tableColumnCount(head) ? blockModel.tableWidth(head) : blockModel.tableColumnLeft(head, gap))
        y: (blockModel.layoutRevision, lastRec >= 0 ? blockModel.yForRow(head) + blockModel.tablePadTop(head) : 0) - flick.contentY
        width: 3
        height: (blockModel.layoutRevision, lastRec >= 0
                 ? blockModel.yForRow(lastRec) + blockModel.heightForRow(lastRec) - blockModel.tablePadBottom(lastRec)
                   - blockModel.yForRow(head) - blockModel.tablePadTop(head) : 0)
        color: Theme.colors.accent
        z: 50
    }
    // + row / + column beside the caret's derived table (SR-4 S7b; the Table block's tableAdd twin).
    // Root overlays: the document mouse layer stacks over every delegate.
    Item {
        id: tableAdd
        readonly property int head: (blockModel.contentRevision,
            flick.visible && root.activeFrameId === "" && !root.inkMode ? blockModel.tableHeadOf(cursor.focusRow) : -1)
        readonly property var recs: head >= 0 ? (blockModel.contentRevision, blockModel.tableRecords(head)) : []
        readonly property int lastRec: recs.length ? recs[recs.length - 1] : -1
        readonly property real topC: (blockModel.layoutRevision, lastRec >= 0 ? blockModel.yForRow(head) + blockModel.tablePadTop(head) : 0)
        readonly property real bottomC: (blockModel.layoutRevision, lastRec >= 0
            ? blockModel.yForRow(lastRec) + blockModel.heightForRow(lastRec) - blockModel.tablePadBottom(lastRec) : 0)
        readonly property real tw: (blockModel.layoutRevision, blockModel.contentRevision, lastRec >= 0 ? blockModel.tableWidth(head) : 0)
        readonly property real xV: root.tableX(head) - flick.contentX
        visible: lastRec >= 0
        z: 40
        Rectangle {   // + row, under the last row
            x: tableAdd.xV; y: tableAdd.bottomC - flick.contentY + 4
            width: tableAdd.tw; height: 14; radius: 0
            color: tableAddRowMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.surfaceHover
            border.width: 1; border.color: Theme.colors.border
            Text { anchors.centerIn: parent; text: "+"; color: Theme.colors.textMuted; font.pixelSize: Theme.font.sizeChrome }
            MouseArea {
                id: tableAddRowMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: { root.forceActiveFocus(); root.tableAddRow(tableAdd.head) }
            }
        }
        Rectangle {   // + column, right of the last column
            x: tableAdd.xV + tableAdd.tw + 4; y: tableAdd.topC - flick.contentY
            width: 14; height: Math.max(0, tableAdd.bottomC - tableAdd.topC); radius: 0
            color: tableAddColMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.surfaceHover
            border.width: 1; border.color: Theme.colors.border
            Text { anchors.centerIn: parent; text: "+"; color: Theme.colors.textMuted; font.pixelSize: Theme.font.sizeChrome }
            MouseArea {
                id: tableAddColMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: { root.forceActiveFocus(); root.tableAddColumn(tableAdd.head) }
            }
        }
    }

    // Grace period so the pointer can travel from the link text up onto the pill.
    Timer { id: linkTipHide; interval: 500; repeat: false; onTriggered: root.hoverLinkUrl = "" }

    // Link-hover tooltip: a clickable pill (root overlay, above the central mouse
    // layer so it gets the click) that opens the URL externally. Caret editing is
    // never disturbed — this is the only click affordance for links.
    Rectangle {
        id: linkTip
        visible: root.hoverLinkUrl.length > 0
        z: 60
        width: Math.min(380, tipRow.implicitWidth + 16)
        height: 26
        x: Math.max(6, Math.min(root.width - width - 6, root.hoverLinkX))
        y: Math.max(6, root.hoverLinkViewY - height - 3)
        color: Theme.colors.surfaceHover
        border.width: 1; border.color: Theme.colors.border
        Row {
            id: tipRow
            anchors.centerIn: parent; spacing: 6
            Text { text: "↗"; color: Theme.colors.accent; anchors.verticalCenter: parent.verticalCenter
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
            Text {
                text: root.hoverLinkUrl
                color: Theme.colors.text; elide: Text.ElideMiddle
                width: Math.min(340, implicitWidth); anchors.verticalCenter: parent.verticalCenter
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
        }
        MouseArea {
            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
            // Keep the pill alive on EVERY move over it, not just on enter: the
            // central layer's onExited can re-arm the hide timer right after our
            // onEntered, so a one-shot stop would still let it vanish under us.
            onEntered: linkTipHide.stop()
            onPositionChanged: linkTipHide.stop()
            onExited: linkTipHide.restart()
            onClicked: { Qt.openUrlExternally(root.hoverLinkUrl); root.hoverLinkUrl = ""; linkTipHide.stop() }
        }
    }

    // One row of a hand-rolled menu (matches the app's flat dark style rather
    // than the default Controls Menu chrome). `danger` tints destructive items.
    // One context-menu row. `scope` (block|column|row) drives the live target
    // highlight on hover; danger rows render in the error colour. Compact (small
    // font, 24px) so the multi-column table menu stays tidy.
    component MenuRow: Rectangle {
        property alias text: menuRowLabel.text
        property bool danger: false
        property bool inert: false       // true = an informational row (dimmed, not clickable)
        property string scope: "block"   // what this item targets: block | column | row
        property color hoverColor: Theme.colors.surfaceHover   // the spell well hovers in its own tone
        property bool menuRowLabelBright: false                // filled rows: bright bold label
        signal activated()
        width: 168; height: 24
        color: !inert && menuRowMA.containsMouse ? hoverColor : "transparent"
        Text {
            id: menuRowLabel
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: 10
            anchors.right: parent.right; anchors.rightMargin: 10
            elide: Text.ElideRight                 // keep long labels (a URL) inside the menu
            color: parent.inert ? Theme.colors.textMuted : parent.danger ? Theme.colors.error
                 : parent.menuRowLabelBright ? Theme.colors.textBright : Theme.colors.text
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall; font.bold: parent.menuRowLabelBright
        }
        MouseArea {
            id: menuRowMA
            enabled: !parent.inert
            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
            // Hovering an item highlights its target scope on the document/table.
            onContainsMouseChanged: if (containsMouse) {
                root.menuHiScope = parent.scope; root.menuHiDanger = parent.danger
            }
            // Close via root: a row instantiated by a Repeater (the spell
            // suggestions) can't resolve the popup's id from inside this
            // inline component, while `root` resolves everywhere.
            onClicked: { parent.activated(); root.closeBlockMenu() }
        }
    }

    // Column header (Block / Column / Row) for the multi-column table menu.
    component MenuHeader: Text {
        color: Theme.colors.textMuted
        font.family: Theme.font.family; font.pixelSize: 11; font.bold: true
        leftPadding: 10; topPadding: 6; bottomPadding: 4
    }

    // A labelled row of small icon buttons (used to compress Align L/C/R and
    // Sort asc/desc into one row each instead of five separate rows).
    component MenuIconBtn: Rectangle {
        property string icon: ""
        property bool on: false
        signal activated()
        width: 26; height: 22
        color: on ? Theme.colors.divider : (mibMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
        Icon { anchors.centerIn: parent; name: parent.icon; size: 14
               color: parent.on ? Theme.colors.textBright : Theme.colors.textMuted }
        MouseArea {
            id: mibMA
            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
            onContainsMouseChanged: if (containsMouse) { root.menuHiScope = "column"; root.menuHiDanger = false }
            onClicked: { parent.activated(); blockMenu.close() }
        }
    }
    component MenuTextBtn: Rectangle {   // a small text segment (the timecode column's frame rates)
        property string text: ""
        property bool on: false
        signal activated()
        width: Math.max(26, mtbLabel.implicitWidth + 8); height: 22
        color: on ? Theme.colors.divider : (mtbMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
        Text { id: mtbLabel; anchors.centerIn: parent; text: parent.text; font.family: Theme.font.family; font.pixelSize: 11
               color: parent.on ? Theme.colors.textBright : Theme.colors.textMuted }
        MouseArea {
            id: mtbMA
            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
            onContainsMouseChanged: if (containsMouse) { root.menuHiScope = "column"; root.menuHiDanger = false }
            onClicked: { parent.activated(); blockMenu.close() }
        }
    }
    component MenuSegRow: Item {
        property string label: ""
        default property alias content: segRow.data
        width: 168; height: 24
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: 10
            text: parent.label; color: Theme.colors.textMuted
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }
        Row {
            id: segRow
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right; anchors.rightMargin: 6
            spacing: 2
        }
    }

    // --- Choice option picker (root overlay above the mouse layer) ---
    // Inline chip variant (DT-2): a span address.
    function openInlineChoicePicker(brow, s, vx, vy) {
        choicePicker.tableHead = -1
        choicePicker.srow = brow; choicePicker.sstart = s
        root.choiceX = vx; root.choiceY = vy
        choicePicker.open()
    }
    // A table's choice cell (SR-4 S6b): the picker under the cell, the add field
    // prefilled with `text` (the typed character that opened it, or "").
    function openGridChoicePicker(head, r, c, text) {
        choicePicker.srow = -1; choicePicker.sstart = -1
        choicePicker.tableHead = head; choicePicker.tableR = r; choicePicker.tableC = c
        const b = blockModel.tableCellAt(head, r, c)
        const cell = b >= 0 ? root.cellForRow(b) : null
        if (cell && cell.teItem) {
            const pt = cell.teItem.mapToItem(root, 0, cell.teItem.height + 4)
            root.choiceX = pt.x; root.choiceY = pt.y
        }
        choicePicker.prefill(text)
        choicePicker.open()
    }
    ChoicePicker {
        id: choicePicker
        z: 60
        x: Math.max(8, Math.min(root.choiceX, root.width - width - 8))
        y: Math.max(8, Math.min(root.choiceY, root.height - height - 8))
        onClosed: root.forceActiveFocus()
        onEditOptions: {
            if (choicePicker.tableMode) choiceEditor.open2Grid(choicePicker.tableHead, choicePicker.tableC)
            else if (choicePicker.spanMode) choiceEditor.open2Span(choicePicker.srow, choicePicker.sstart)
        }
    }
    // Modal editor for a choice column's option set (opened from the column menu or
    // the picker's "Edit options…"). Centres itself in the editor.
    ChoiceColumnEditor {
        id: choiceEditor
        z: 70
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        onClosed: root.forceActiveFocus()
    }

    // --- Block context menu (right-click a block / its grip) ---
    Popup {
        id: blockMenu
        readonly property bool isCode: !menuInSel && root.menuRow >= 0
            && (blockModel.contentRevision, blockModel.typeForRow(root.menuRow) === 2)
        readonly property bool isMedia: !menuInSel && root.menuRow >= 0
            && (blockModel.contentRevision, blockModel.typeForRow(root.menuRow) === 3)
        readonly property bool isPdf: isMedia
            && (blockModel.contentRevision, blockModel.mediaKind(root.menuRow)) === "pdf"
        readonly property bool isVideo: isMedia
            && (blockModel.contentRevision, blockModel.mediaKind(root.menuRow)) === "video"
        readonly property bool isSketch: isMedia
            && (blockModel.contentRevision, blockModel.mediaKind(root.menuRow)) === "sketch"
        readonly property bool isImage: isMedia
            && (blockModel.contentRevision, blockModel.mediaKind(root.menuRow)) === "image"
        // In a full-frame tab (PDF/video/sketch) the menu is a view INTO one block, so
        // document-structural block ops (add/duplicate/copy block) don't belong.
        readonly property bool inFrameTab: root.activePdfRow >= 0 || root.activeVideoRow >= 0 || root.activeSketchRow >= 0
        // The menu row lies inside a multi-block selection → the menu is a RUN
        // menu: block ops (add above/below, duplicate, copy, paste, move,
        // insert below, delete) act on the whole run and the target highlight
        // covers it; single-block rows (open, media, code, chip, link, table
        // columns) are withheld — the clicked row isn't apparent under a run
        // highlight (isMedia/isCode read false here).
        readonly property bool menuInSel: cursor.hasSel && cursor.loRow !== cursor.hiRow
                                          && root.menuRow >= cursor.loRow && root.menuRow <= cursor.hiRow
        readonly property int runLo: menuInSel ? cursor.loRow : root.menuRow
        readonly property int runHi: menuInSel ? cursor.hiRow : root.menuRow
        // Lanes (SR-3 S7b): the split row under the menu, and what it allows.
        readonly property int laneRecord: menuInSel || root.menuRow < 0 ? -1
            : (blockModel.contentRevision, blockModel.splitRowOf(root.menuRow))
        readonly property bool canSplit: root.menuRow >= 0 && (blockModel.contentRevision,
            blockModel.typeForRow(runLo) !== 10
            && (!menuInSel || (blockModel.laneForRow(runLo) < 0 && blockModel.laneForRow(runHi) < 0)))
        readonly property bool canAlign: laneRecord >= 0 && (blockModel.contentRevision,
            blockModel.splitRowLast(laneRecord) - laneRecord > blockModel.laneCount(laneRecord))
        readonly property int mergeBelow: laneRecord < 0 ? -1
            : (blockModel.contentRevision, root.mergeTargetBelow(laneRecord))
        // Derived tables (SR-4 S7a): the table cell under the menu.
        readonly property int tableHead: menuInSel || root.menuRow < 0 ? -1
            : (blockModel.contentRevision, blockModel.tableHeadOf(root.menuRow))
        readonly property int tableC: tableHead >= 0 ? (blockModel.contentRevision, blockModel.tableColumnOf(root.menuRow)) : -1
        readonly property bool tableHeaderRow: tableHead >= 0 && (blockModel.contentRevision, blockModel.isHeaderRow(root.menuRow))
        readonly property int tableKind: tableC >= 0 && !tableHeaderRow ? (blockModel.contentRevision, blockModel.tableColumnKind(tableHead, tableC)) : 0
        readonly property int tableAlign: tableC >= 0 ? (blockModel.contentRevision, blockModel.tableColAlign(tableHead, tableC)) : 0
        readonly property int tableCols: tableHead >= 0 ? (blockModel.contentRevision, blockModel.tableColumnCount(tableHead)) : 0
        readonly property int tableHeaders: tableHead >= 0 ? (blockModel.contentRevision, blockModel.headerCount(tableHead)) : 0
        readonly property bool tableOn: !inFrameTab && tableHead >= 0
        // A grip-picked set of two or more holding the clicked cell (S7b): its ops replace the single-cell rows.
        readonly property var tableSetHit: {
            const dep = blockModel.contentRevision
            const s = root.tableSet
            if (!tableOn || !s || !root.tableSetLive() || s.head !== tableHead || s.items.length < 2) return null
            return s.items.indexOf(s.kind === "row" ? blockModel.tableRowOf(root.menuRow) : tableC) >= 0 ? s : null
        }
        readonly property bool tableOne: tableOn && tableSetHit === null
        readonly property int tableR: tableHead >= 0 ? (blockModel.contentRevision, blockModel.tableRowOf(root.menuRow)) : -1
        readonly property int tableRows: tableHead >= 0 ? (blockModel.contentRevision, blockModel.tableRowCount(tableHead)) : 0
        readonly property bool tableSortable: tableRows - tableHeaders > 1
        readonly property bool tableBodyDeletable: !tableHeaderRow && tableRows - tableHeaders > 1
        readonly property int tableColKind: tableC >= 0 ? (blockModel.contentRevision, blockModel.tableColumnKind(tableHead, tableC)) : 0   // header rows too
        readonly property bool tableSetCols: tableSetHit !== null && tableSetHit.kind === "col"
        readonly property string tableSetNoun: tableSetHit === null ? ""
            : tableSetHit.items.length + (tableSetHit.kind === "row" ? " rows" : " columns")
        // The right-clicked issue, re-read live: a background-pass issue has no
        // suggestions until the worker's follow-up lands (spell.revision bumps).
        readonly property var liveIssue: {
            var dep = spell.revision
            if (!root.menuIssue) return null
            var it = spell.issueAt(root.menuRow, root.menuIssue.s)
            return it.ruleId !== undefined ? it : root.menuIssue
        }
        // A grip-picked set → ONE compact menu column (user ruling 2026-08-21: the full
        // three-column menu is noise when the target is the selection).
        readonly property bool bulkMode: tableSetHit !== null
        // Tallest of the visible columns — the inter-column dividers stretch to it.
        readonly property real bodyH: bulkMode ? bulkColMenu.implicitHeight
            : tableOne
            ? Math.max(blockColMenu.implicitHeight, tableColMenu.implicitHeight, tableRowMenu.implicitHeight)
            : blockColMenu.implicitHeight
        padding: 4; z: 60
        // Reactive on-screen clamp: re-evaluates as the menu's height settles after
        // open (so a long menu is positioned right on the FIRST trigger, not the 2nd).
        x: Math.max(8, Math.min(root.menuX, root.width - width - 8))
        y: Math.max(8, Math.min(root.menuY, root.height - height - 8))
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside | Popup.CloseOnReleaseOutside
        onClosed: { root.menuHiScope = ""; root.forceActiveFocus() }
        background: Rectangle { color: Theme.colors.surfaceRaised; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        // Columns side by side: Block always; Column + Row appear for tables (no
        // submenus). Inter-column dividers stretch to the tallest column (bodyH).
        contentItem: Row {
            spacing: 0

            // --- Block column (the right-clicked block as a whole) ---
            Column {
                id: blockColMenu
                visible: !blockMenu.bulkMode
                spacing: 1
                MenuHeader { visible: blockMenu.menuInSel; text: (blockMenu.runHi - blockMenu.runLo + 1) + " blocks" }
                // Spell / grammar (0.5.0): the issue under the click as a
                // tinted WELL in the underline's colour (red = spelling, the
                // quote-bar blue = grammar) with a 2 px bar on the left — the
                // menu section reads as the same object as the squiggle. Its
                // message, up to five suggestions, then the escape hatches.
                // Withheld in a run menu like every single-block row.
                Item {
                    id: spellWell
                    readonly property var issue: (!blockMenu.menuInSel) ? blockMenu.liveIssue : null
                    // Every fixable issue in the target (the block, or the clicked
                    // cell) — the "Fix all" count. Re-read as suggestions land.
                    readonly property int fixable: {
                        var dep = spell.revision
                        if (blockMenu.menuInSel || root.menuRow < 0) return 0
                        var list = spell.issuesForRow(root.menuRow)
                        var n = 0
                        for (var i = 0; i < list.length; ++i) if (list[i].suggestions.length > 0) ++n
                        return n
                    }
                    readonly property bool showFixAll: fixable >= 2 || (!issue && fixable >= 1)
                    readonly property color tone: issue && issue.kind === 1 ? Theme.colors.quoteBar : Theme.colors.error
                    // Hover inside the well steps up in the SAME tone (not the
                    // neutral surfaceHover): the section stays one object.
                    readonly property color hover: Qt.rgba(tone.r, tone.g, tone.b, 0.30)
                    visible: !!issue || showFixAll
                    width: 168; height: visible ? spellRows.implicitHeight : 0
                    Rectangle { anchors.fill: parent; radius: 0
                                color: Qt.rgba(spellWell.tone.r, spellWell.tone.g, spellWell.tone.b, 0.12) }
                    Rectangle { width: 2; height: parent.height; color: spellWell.tone }
                    Column {
                        id: spellRows
                        width: parent.width
                        MenuHeader { width: 168; elide: Text.ElideRight; color: spellWell.tone
                                     text: spellWell.issue ? (spellWell.issue.kind === 1 ? spellWell.issue.message : "Spelling") : "" }
                        Repeater {
                            model: spellWell.issue ? Math.min(5, spellWell.issue.suggestions.length) : 0
                            MenuRow { required property int index; hoverColor: spellWell.hover
                                      text: spellWell.issue ? spellWell.issue.suggestions[index] : ""
                                      onActivated: root.applySpellSuggestion(spellWell.issue.suggestions[index]) }
                        }
                        MenuRow { visible: !!spellWell.issue && spellWell.issue.suggestions.length === 0
                                  inert: true
                                  text: (spellWell.issue && !spellWell.issue.suggestionsComputed) ? "Looking up…" : "No suggestions" }
                        MenuRow { visible: !!spellWell.issue && spellWell.issue.kind === 0; hoverColor: spellWell.hover
                                  text: "Add to dictionary"; onActivated: spell.addToDictionary(root.menuIssueWord()) }
                        MenuRow { visible: !!spellWell.issue && spellWell.issue.kind === 0; hoverColor: spellWell.hover
                                  text: "Ignore"; onActivated: spell.ignoreWord(root.menuIssueWord()) }
                        MenuRow { visible: !!spellWell.issue && spellWell.issue.kind === 1; hoverColor: spellWell.hover
                                  text: "Ignore rule: " + (spellWell.issue ? spellWell.issue.ruleName : "")
                                  onActivated: spell.ignoreRule(spellWell.issue.ruleId) }
                        // The section's one FILLED row: apply the top suggestion of
                        // every issue in the block / cell as ONE undo step.
                        Rectangle { visible: spellWell.showFixAll; width: 168; height: 1
                                    color: Qt.rgba(spellWell.tone.r, spellWell.tone.g, spellWell.tone.b, 0.45) }
                        MenuRow { visible: spellWell.showFixAll
                                  hoverColor: Qt.rgba(spellWell.tone.r, spellWell.tone.g, spellWell.tone.b, 0.55)
                                  text: "Fix all in block (" + spellWell.fixable + ")"
                                  onActivated: root.applyAllSuggestions()
                                  Rectangle { anchors.fill: parent; z: -1; radius: 0
                                              color: Qt.rgba(spellWell.tone.r, spellWell.tone.g, spellWell.tone.b, 0.28) }
                                  Component.onCompleted: { menuRowLabelBright = true } }
                    }
                }
                Rectangle { visible: spellWell.visible; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: root.menuLinkUrl.length > 0 && !blockMenu.menuInSel
                          text: "Open " + root.truncUrl(root.menuLinkUrl)
                          onActivated: Qt.openUrlExternally(root.menuLinkUrl) }
                Rectangle { visible: root.menuLinkUrl.length > 0 && !blockMenu.menuInSel; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: blockMenu.isPdf && root.activePdfRow < 0; text: "Open in tab"; onActivated: root.setActiveTab(blockModel.idForRow(root.menuRow)) }
                MenuRow { visible: blockMenu.isVideo && root.activeVideoRow < 0; text: "Open in studio"; onActivated: root.setActiveTab(blockModel.idForRow(root.menuRow)) }
                MenuRow { visible: blockMenu.isSketch && root.activeSketchRow < 0; text: "Open in tab"; onActivated: root.setActiveTab(blockModel.idForRow(root.menuRow)) }
                MenuRow { visible: !blockMenu.inFrameTab && !blockMenu.isMedia && !blockMenu.menuInSel
                          text: "Insert choice chip"
                          onActivated: { cursor.setCaret(root.menuRow, cursor.focusRow === root.menuRow ? cursor.focusCol : blockModel.contentForRow(root.menuRow).length); root.insertChoiceChip() } }
                MenuRow { visible: !blockMenu.inFrameTab; text: "Add block above"; onActivated: root.addBlockAbove(blockMenu.runLo) }
                MenuRow { visible: !blockMenu.inFrameTab; text: "Add block below"; onActivated: root.addBlockBelow(blockMenu.runHi) }
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.canSplit && blockMenu.tableHead < 0; text: "Split into columns"
                          onActivated: root.splitMenu(blockMenu.runLo, blockMenu.runHi) }
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.canAlign && blockMenu.tableHead < 0; text: "Align lanes"
                          onActivated: blockModel.alignLanes(blockMenu.laneRecord) }
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.mergeBelow >= 0 && blockMenu.tableHead < 0; text: "Merge with the row below"
                          onActivated: blockModel.mergeRowsIntoLanes(blockMenu.laneRecord, blockMenu.mergeBelow) }
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.laneRecord >= 0 && blockMenu.tableHead < 0; text: "Delete lane"; danger: true
                          onActivated: root.deleteLane(root.menuRow) }
                // Derived tables (SR-4 S7a). A split row takes the header role; a table cell gets the table's ops.
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.laneRecord >= 0 && (blockMenu.tableHead < 0 || !blockMenu.tableHeaderRow)
                          text: "Assign as header"
                          onActivated: blockModel.setHeaderRole(blockMenu.laneRecord, 1) }
                // The table as a whole; its column and row get their own menu columns (tableColMenu / tableRowMenu).
                MenuHeader { visible: blockMenu.tableOne; text: "Table" }
                MenuRow { visible: blockMenu.tableOne && blockMenu.tableHeaders < blockMenu.tableRows - 1; scope: "table"
                          text: "Add a header row"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.setHeaderRole(h, blockModel.headerCount(h) + 1); return null }) }
                MenuRow { visible: blockMenu.tableOne && blockMenu.tableHeaders > 1; scope: "table"; text: "Remove a header row"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.setHeaderRole(h, blockModel.headerCount(h) - 1); return null }) }
                MenuRow { visible: blockMenu.tableOne && blockMenu.tableHeaderRow; scope: "table"; text: "Unassign header"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.setHeaderRole(h, 0); return null }) }
                MenuRow { visible: blockMenu.tableOne && (clipboard.hasBlocks() || clipboard.hasHtml() || clipboard.readText().length > 0)
                          text: "Paste into cells"
                          onActivated: root.pasteIntoCellsAt(root.menuRow) }
                MenuRow { visible: blockMenu.tableOne && (blockModel.contentRevision, root.firstGroupColOf(blockMenu.tableHead)) >= 0
                          scope: "table"; text: "View as board"
                          onActivated: root.openBoard(blockMenu.tableHead, (blockMenu.tableColKind === 1 || blockMenu.tableColKind === 2)
                                                                          ? blockMenu.tableC : root.firstGroupColOf(blockMenu.tableHead)) }
                MenuRow { visible: blockMenu.tableOne; scope: "table"; text: "Delete table"; danger: true
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.deleteTable(h); return null }) }
                Rectangle { visible: blockMenu.tableOne; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: !blockMenu.inFrameTab; text: blockMenu.menuInSel ? "Duplicate blocks" : "Duplicate block"
                          onActivated: root.duplicateRun(blockMenu.runLo, blockMenu.runHi) }
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.runLo > 0
                          text: blockMenu.menuInSel ? "Move blocks up" : "Move up"; onActivated: root.moveMenuRow(-1) }
                MenuRow { visible: !blockMenu.inFrameTab && (blockModel.contentRevision, blockMenu.runHi < blockModel.count - 1)
                          text: blockMenu.menuInSel ? "Move blocks down" : "Move down"; onActivated: root.moveMenuRow(1) }
                MenuRow { visible: !blockMenu.inFrameTab
                          text: blockMenu.menuInSel ? "Copy blocks" : "Copy"
                          onActivated: { if (blockMenu.menuInSel) { var ce = cursor.effectiveRange(); root.copyRange(ce.lR, ce.lC, ce.hR, ce.hC) }
                                         else root.copyBlock(root.menuRow) } }
                MenuRow { visible: !blockMenu.inFrameTab; text: "Paste"
                          onActivated: root.pasteAtBlock(root.menuRow) }
                MenuRow { visible: blockMenu.isImage
                          text: "Copy image"
                          onActivated: { clipboard.writeImageFromFile(blockModel.mediaUrl(root.menuRow))
                                         Toasts.show(qsTr("Image copied")) } }
                // Image size (the corner double-click's discoverable twin): fit the lane / the original.
                MenuRow { visible: blockMenu.isImage && !blockMenu.inFrameTab; text: "Fit width"
                          onActivated: blockModel.setMediaWidth(root.menuRow, root.fitWidthForRow(root.menuRow)) }
                MenuRow { visible: blockMenu.isImage && !blockMenu.inFrameTab; text: "Original size"
                          onActivated: blockModel.setMediaWidth(root.menuRow, 0) }
                MenuRow { visible: blockMenu.isMedia && !blockMenu.isSketch   // sketch has no backing file
                          text: Qt.platform.os === "windows" ? "Show in Explorer" : "Reveal in Finder"
                          onActivated: blockModel.revealMedia(root.menuRow) }
                MenuRow { visible: blockMenu.isMedia && !blockMenu.isSketch; text: "Open in ufb"
                          onActivated: blockModel.openMediaInUfb(root.menuRow) }
                MenuRow { text: "Insert table below"; onActivated: root.insertTableAt(blockMenu.runHi) }
                MenuRow { visible: !blockMenu.inFrameTab; text: "Insert sketch below"; onActivated: root.insertSketchAt(blockMenu.runHi) }
                // comment on the current (single-row) text selection
                MenuRow {
                    visible: !blockMenu.inFrameTab && cursor.hasSel && cursor.loRow === cursor.hiRow
                    text: "Add comment"
                    onActivated: root.addCommentOnSelection()
                }
                // text-only transform (withheld for a run — the clicked row isn't apparent)
                Rectangle { visible: !blockMenu.isMedia && !blockMenu.menuInSel; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow {
                    visible: !blockMenu.isMedia && !blockMenu.menuInSel
                    text: blockMenu.isCode ? "Change language…" : "Make code block"
                    onActivated: blockMenu.isCode ? root.openLangPopupForRow(root.menuRow)
                                                  : root.makeCodeAt(root.menuRow)
                }
                Rectangle { visible: !blockMenu.inFrameTab; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: !blockMenu.inFrameTab; text: blockMenu.menuInSel ? "Delete blocks" : "Delete block"; danger: true
                          onActivated: root.deleteRun(blockMenu.runLo, blockMenu.runHi) }
            }

            // --- Tables (SR-4): the right-clicked cell's column and row ---
            Rectangle { visible: blockMenu.tableOne; width: 1; height: blockMenu.bodyH; color: Theme.colors.divider }
            Column {
                id: tableColMenu
                visible: blockMenu.tableOne
                spacing: 1
                MenuHeader { text: "Column" }
                MenuRow { scope: "column"; text: "Select column"
                          onActivated: root.tableGripClick(blockMenu.tableHead, "col", blockMenu.tableC, 0) }
                MenuRow { scope: "column"; text: "Insert column left"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableInsertColumn(h, c); return [r, c] }) }
                MenuRow { scope: "column"; text: "Insert column right"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableInsertColumn(h, c + 1); return [r, c + 1] }) }
                MenuRow { visible: blockMenu.tableC > 0; scope: "column"; text: "Move column left"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableMoveColumn(h, c, c - 1); return [r, c - 1] }) }
                MenuRow { visible: blockMenu.tableC < blockMenu.tableCols - 1; scope: "column"; text: "Move column right"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableMoveColumn(h, c, c + 1); return [r, c + 1] }) }
                MenuRow { scope: "column"; text: "Duplicate column"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableDuplicateColumn(h, c); return [r, c + 1] }) }
                Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
                MenuSegRow {
                    label: "Align"
                    MenuIconBtn { icon: "text-align-left";   on: blockMenu.tableAlign === 0
                                  onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableSetColAlign(h, c, 0); return null }) }
                    MenuIconBtn { icon: "text-align-center"; on: blockMenu.tableAlign === 1
                                  onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableSetColAlign(h, c, 1); return null }) }
                    MenuIconBtn { icon: "text-align-right";  on: blockMenu.tableAlign === 2
                                  onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableSetColAlign(h, c, 2); return null }) }
                }
                Rectangle { visible: blockMenu.tableSortable; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuSegRow {
                    visible: blockMenu.tableSortable
                    label: "Sort"
                    MenuIconBtn { icon: "sort-ascending"
                                  onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableSortByColumn(h, c, true); return null }) }
                    MenuIconBtn { icon: "sort-descending"
                                  onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableSortByColumn(h, c, false); return null }) }
                }
                Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: blockMenu.tableColKind !== 1; scope: "column"; text: "Make choice column"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableSetColumnKind(h, c, 1); return [r, c] }) }
                MenuRow { visible: blockMenu.tableColKind !== 2; scope: "column"; text: "Make checkmark column"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableSetColumnKind(h, c, 2); return [r, c] }) }
                MenuRow { visible: blockMenu.tableColKind === 1; scope: "column"; text: "Edit options…"
                          onActivated: choiceEditor.open2Grid(blockMenu.tableHead, blockMenu.tableC) }
                MenuRow { visible: blockMenu.tableColKind !== 3; scope: "column"; text: "Make timecode column"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableSetColumnKind(h, c, 3); return [r, c] }) }
                MenuSegRow {   // T6: the timecode column's frame rate
                    visible: blockMenu.tableColKind === 3
                    label: "Frame rate"
                    readonly property real fps: blockMenu.tableColKind === 3 ? (blockModel.contentRevision, blockModel.tableColumnFps(blockMenu.tableHead, blockMenu.tableC)) : 0
                    Repeater {
                        model: [23.976, 24, 25, 29.97, 30, 60]
                        MenuTextBtn { required property var modelData; text: "" + modelData; on: Math.abs(parent.parent.fps - modelData) < 0.01
                                      onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableSetColumnFps(h, c, modelData); return null }) }
                    }
                }
                MenuRow { visible: blockMenu.tableColKind !== 0; scope: "column"; text: "Make text column"; danger: true
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableSetColumnKind(h, c, 0); return [r, c] }) }
                Rectangle { visible: blockMenu.tableCols > 1; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: blockMenu.tableCols > 1; scope: "column"; text: "Delete column"; danger: true
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableDeleteColumn(h, c); return [r, Math.max(0, c - 1)] }) }
            }
            Rectangle { visible: blockMenu.tableOne; width: 1; height: blockMenu.bodyH; color: Theme.colors.divider }
            Column {
                id: tableRowMenu
                visible: blockMenu.tableOne
                spacing: 1
                MenuHeader { text: "Row" }
                MenuRow { scope: "row"; text: "Select row"
                          onActivated: root.tableGripClick(blockMenu.tableHead, "row", blockMenu.tableR, 0) }
                MenuRow { scope: "row"; text: "Insert row above"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableInsertRow(h, r); return [r, c] }) }
                MenuRow { scope: "row"; text: "Insert row below"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableInsertRow(h, r + 1); return [r + 1, c] }) }
                // Reorder / duplicate / delete: body rows only (a header row's place is the table's; Delete table is under Table).
                MenuRow { visible: blockMenu.tableR > blockMenu.tableHeaders; scope: "row"; text: "Move row up"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableMoveRow(h, r, r - 1); return [r - 1, c] }) }
                MenuRow { visible: !blockMenu.tableHeaderRow && blockMenu.tableR < blockMenu.tableRows - 1; scope: "row"; text: "Move row down"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableMoveRow(h, r, r + 1); return [r + 1, c] }) }
                MenuRow { visible: !blockMenu.tableHeaderRow; scope: "row"; text: "Duplicate row"
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableDuplicateRow(h, r); return [r + 1, c] }) }
                Rectangle { visible: blockMenu.tableBodyDeletable; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: blockMenu.tableBodyDeletable; scope: "row"; text: "Delete row"; danger: true
                          onActivated: root.tableMenuOp(function(h, r, c) { blockModel.tableDeleteRow(h, r); return [r, c] }) }
            }

            // --- Bulk column: the ONE compact menu when the right-click
            // targets a multi-selection (rows set / columns set / cell rect).
            Column {
                id: bulkColMenu
                visible: blockMenu.bulkMode
                spacing: 1
                MenuHeader { text: blockMenu.tableSetNoun }
                // A derived table's grip-picked set (SR-4 S7b)
                MenuRow { visible: blockMenu.tableSetHit !== null; scope: "set"; text: "Clear contents"
                          onActivated: root.clearGridSet() }
                MenuSegRow {
                    visible: blockMenu.tableSetCols
                    label: "Align"
                    MenuIconBtn { icon: "text-align-left"
                                  onActivated: root.tableSetMenuOp(function(h, items) { blockModel.tableSetColsAlign(h, items, 0) }) }
                    MenuIconBtn { icon: "text-align-center"
                                  onActivated: root.tableSetMenuOp(function(h, items) { blockModel.tableSetColsAlign(h, items, 1) }) }
                    MenuIconBtn { icon: "text-align-right"
                                  onActivated: root.tableSetMenuOp(function(h, items) { blockModel.tableSetColsAlign(h, items, 2) }) }
                }
                MenuRow { visible: blockMenu.tableSetCols; scope: "set"; text: "Make choice columns"
                          onActivated: root.tableSetMenuOp(function(h, items) { blockModel.tableSetColumnsKind(h, items, 1) }) }
                MenuRow { visible: blockMenu.tableSetCols; scope: "set"; text: "Make checkmark columns"
                          onActivated: root.tableSetMenuOp(function(h, items) { blockModel.tableSetColumnsKind(h, items, 2) }) }
                MenuRow { visible: blockMenu.tableSetCols; scope: "set"; text: "Make text columns"; danger: true
                          onActivated: root.tableSetMenuOp(function(h, items) { blockModel.tableSetColumnsKind(h, items, 0) }) }
                // Destructive tail
                Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: blockMenu.tableSetHit !== null; scope: "set"; danger: true
                          text: blockMenu.tableSetHit !== null && blockMenu.tableSetHit.kind === "row"
                                && blockMenu.tableSetHit.items[0] < blockMenu.tableHeaders ? "Delete table" : "Delete " + blockMenu.tableSetNoun
                          onActivated: root.tableSetMenuOp(function(h, items, kind) {
                              if (kind === "row") blockModel.tableDeleteRows(h, items)
                              else blockModel.tableDeleteColumns(h, items)
                          }) }
            }
        }
    }

    // --- Code-block language picker (shared; positioned where the menu or the
    // block's language chip opened it). The FULL KSyntax definition list,
    // filtered by the field as you type; Enter still applies any lenient tag
    // ("js", "bash"…). "Plain text" heads the list; the current pick carries
    // a check. Applies to langPopup.targetRow via blockModel.setCodeLanguage.
    Popup {
        id: langPopup
        property int targetRow: -1
        readonly property string current: targetRow >= 0
            ? (blockModel.contentRevision, blockModel.codeLanguageName(targetRow)) : ""
        readonly property var allLangs: blockModel.documentOpen ? blockModel.codeLanguages() : []
        readonly property var shown: {
            var f = langField.text.trim().toLowerCase()
            var out = []
            if (f === "" || "plain text".indexOf(f) >= 0) out.push("")   // "" = plain sentinel
            for (var i = 0; i < allLangs.length; ++i)
                if (f === "" || allLangs[i].toLowerCase().indexOf(f) >= 0) out.push(allLangs[i])
            return out
        }
        width: 250; padding: 8; focus: true; z: 60
        x: Math.max(8, Math.min(root.menuX, root.width - width - 8))
        y: Math.max(8, Math.min(root.menuY, root.height - height - 8))
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: { targetRow = -1; root.forceActiveFocus() }
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        function apply(lang) {
            if (langPopup.targetRow >= 0)
                blockModel.setCodeLanguage(langPopup.targetRow, (lang || "").trim())
            langPopup.close()
        }
        contentItem: Column {
            spacing: 8
            // Plain TextInput (not a Controls TextField, which the native macOS
            // style refuses to theme) in a themed frame, with a placeholder overlay.
            Rectangle {
                width: parent.width; height: 30; radius: 0
                color: Theme.colors.codeBg; border.width: 1; border.color: Theme.colors.border
                TextInput {
                    id: langField
                    anchors.fill: parent
                    anchors.leftMargin: 8; anchors.rightMargin: 8
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true; selectByMouse: true
                    color: Theme.colors.text; selectionColor: Theme.colors.selectionBg
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                    onAccepted: langPopup.apply(text)
                    Keys.onEscapePressed: langPopup.close()
                    Text {
                        anchors.fill: parent; verticalAlignment: Text.AlignVCenter
                        visible: langField.text.length === 0
                        text: "filter — or type any tag (js, bash…)"
                        color: Theme.colors.textSubtle; font: langField.font
                        elide: Text.ElideRight
                    }
                }
            }
            ListView {
                width: parent.width
                height: Math.min(280, contentHeight)
                clip: true
                model: langPopup.shown
                ScrollBar.vertical: MnScrollBar {}
                delegate: Rectangle {
                    required property string modelData
                    readonly property bool isPlain: modelData === ""
                    readonly property bool isCurrent: isPlain ? langPopup.current === ""
                                                              : modelData === langPopup.current
                    width: ListView.view.width; height: 24; radius: 0
                    color: langRowHover.hovered ? Theme.colors.surfaceHover : "transparent"
                    HoverHandler { id: langRowHover }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter; x: 8
                        width: parent.width - 30; elide: Text.ElideRight
                        text: parent.isPlain ? "Plain text" : parent.modelData
                        color: parent.isPlain ? Theme.colors.textMuted : Theme.colors.text
                        font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
                    }
                    Text {   // check on the current language
                        visible: parent.isCurrent
                        anchors.verticalCenter: parent.verticalCenter
                        x: parent.width - 20
                        text: "✓"; color: Theme.colors.accent
                        font.pixelSize: Theme.font.sizeChrome
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: langPopup.apply(parent.modelData)
                    }
                }
            }
        }
    }

    // URL editor for the link button / Cmd+K. Anchored at the caret; commits the
    // snapshotted target range via root.commitLink (blank URL removes the link).
    Popup {
        id: linkPopup
        property int tRow0: 0; property int tCol0: 0
        property int tRow1: 0; property int tCol1: 0
        property bool insertMode: false
        property string prefill: ""
        property real px: 0; property real py: 0
        width: 320; padding: 8; focus: true; z: 60
        x: px; y: py
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: root.forceActiveFocus()
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        function openAtCaret() {
            var ax = root.width / 2 - width / 2, ay = 80
            var c = root.cellForRow(cursor.focusRow)
            if (c && c.teItem) {
                var rr = c.teItem.positionToRectangle(cursor.focusCol)
                var p = c.teItem.mapToItem(root, rr.x, rr.y + rr.height + 4)
                ax = p.x; ay = p.y
            }
            px = Math.max(8, Math.min(ax, root.width - width - 8))
            py = Math.max(8, Math.min(ay, root.height - height - 8))
            linkField.text = prefill
            open()
            linkField.selectAll(); linkField.forceActiveFocus()
        }
        contentItem: Column {
            spacing: 6
            Rectangle {
                width: parent.width; height: 30; radius: 0
                color: Theme.colors.codeBg; border.width: 1; border.color: Theme.colors.border
                TextInput {
                    id: linkField
                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true; selectByMouse: true
                    color: Theme.colors.text; selectionColor: Theme.colors.selectionBg
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                    onAccepted: { root.commitLink(text); linkPopup.close() }
                    Keys.onEscapePressed: linkPopup.close()
                    Text {
                        anchors.fill: parent; verticalAlignment: Text.AlignVCenter
                        visible: linkField.text.length === 0
                        text: "https://…  (blank removes the link)"
                        color: Theme.colors.textSubtle; font: linkField.font
                        elide: Text.ElideRight
                    }
                }
            }
            Text {
                text: "↵ apply  ·  esc cancel"
                color: Theme.colors.textSubtle
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
        }
    }
}
