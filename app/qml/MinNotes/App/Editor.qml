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
    Component.onCompleted: { forceActiveFocus(); cursor.setCaret(0, 0); _recomputeVideoRows(); _recomputePdfRows(); blockModel.setContentWidth(pageWidth) }
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
    // The document's CONTENT width: the viewport, grown to hold the widest
    // measured block (wide tables — blockModel.maxContentWidth) so the PAGE
    // scrolls horizontally, plus a right breathing margin. In ink mode wide
    // enough for the locked page + gutters, so the Flickable pans natively
    // (the kanban board's 2D-pan pattern).
    readonly property real contentSpan: inkMode
        ? Math.max(flick.width, leftEdge + pageWidth + inkGutter)
        : Math.max(flick.width,
                   leftEdge + Math.max(pageWidth, blockModel.maxContentWidth) + 16)
    // Left-anchored: a fixed margin, NOT centered. ALWAYS the ink gutter
    // (user ruling 2026-08-18): margin annotations stay visible in writing
    // mode, and the page no longer jumps 72px when annotation mode toggles.
    readonly property real leftEdge: inkGutter
    // The SHEET's width: page + equal margins — but never narrower than the
    // widest content plus the same trailing margin (user ruling 2026-08-20:
    // an uncapped wide table extends the sheet rather than being cropped by
    // the desk tone at the page boundary).
    readonly property real sheetSpan:
        leftEdge * 2 + Math.max(pageWidth, blockModel.maxContentWidth)
    function measureForType(t) { return pageWidth }
    function measureForRow(row) { return laneOf(row).w }   // a lane block measures its lane
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
            return { x: blockModel.tableColumnLeft(head, lane), w: blockModel.tableColumnWidth(head, lane) }
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

    // Active table-tab: "" = the Document view; otherwise a table block's id shown
    // full-frame (see Tabs B). Reactive row of that table, -1 if it's gone.
    property string activeTableId: ""
    readonly property int activeTableRow: (blockModel.layoutRevision, blockModel.contentRevision,
        activeTableId === "" ? -1 : blockModel.rowForId(activeTableId))
    // If the active table is deleted, fall back to the Document tab.
    onActiveTableRowChanged: if (activeTableId !== "" && activeTableRow < 0) activeTableId = ""
    // Opening a table tab pins the caret into that table so tcur drives editing.
    onActiveTableIdChanged: {
        forceActiveFocus()
        if (activeTableId === "") return
        var r = blockModel.rowForId(activeTableId)
        if (r >= 0) { cursor.setCaret(r, 0); tcur.place(0, 0, 0) }
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
    readonly property string activeFrameId: activeTableId !== "" ? activeTableId
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
            activeTableId = ""; activePdfId = ""; activeVideoId = ""; activeSketchId = ""; return
        }
        var r = blockModel.rowForId(id)
        if (blockModel.typeForRow(r) === 7) {
            // Any non-Type tool drops here (2026-08-20, overturns the
            // "Select may stay armed" parenthetical): tables select with
            // the cell cursor, so Select is as dead as the draw tools.
            if (t !== "type") inspector.drawTool = "type"
            activePdfId = ""; activeVideoId = ""; activeSketchId = ""; activeTableId = id
            var pc = boardPref(id)               // this table's remembered view
            if (pc >= 0) { boardCol = pc; boardMode = true }
        }
        else if (blockModel.mediaKind(r) === "video") {
            if (t === "text") inspector.drawTool = "select"
            activeTableId = ""; activePdfId = ""; activeSketchId = ""; activeVideoId = id
        }
        else if (blockModel.mediaKind(r) === "sketch") { activeTableId = ""; activePdfId = ""; activeVideoId = ""; activeSketchId = id }
        else { activeTableId = ""; activeVideoId = ""; activeSketchId = ""; activePdfId = id }
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
        saveBoardPref(activeTableId, -1)
    }
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
        if (activeTableRow < 0) return -1
        var rev = blockModel.contentRevision
        for (var c = 0; c < blockModel.tableColumns(activeTableRow); ++c) {
            var k = blockModel.tableColumnKind(activeTableRow, c)
            if (k === 1 || k === 2) return c
        }
        return -1
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
    property int  menuCellR: 0     // right-clicked table cell (for table menu ops)
    property int  menuCellC: 0
    property real choiceX: 0       // anchor for the choice-cell option picker
    property real choiceY: 0
    property string menuLinkUrl: ""  // link URL under the right-click (for "Open …")
    property var  menuIssue: null      // spell/grammar issue under the right-click ({s,e,kind,…}) or null
    property bool menuIssueInCell: false
    // Link-hover tooltip: the URL under the pointer + where to anchor the pill.
    property string hoverLinkUrl: ""
    property real   hoverLinkX: 0
    property real   hoverLinkViewY: 0
    // Context-menu target highlight: the scope of the hovered menu item ("" none,
    // "block" whole block, "column"/"row" within a table) + danger (red) tint.
    property string menuHiScope: ""
    property bool   menuHiDanger: false

    // Table mouse-drag state: anchor cell/char captured on press, so drag extends
    // an in-cell text selection (same cell) or a rectangular cell range (across).
    property bool tableDragging: false
    property int  tableAnchorR: 0
    property int  tableAnchorC: 0
    property int  tableAnchorPos: 0
    // Column-resize drag state.
    property bool tableResizing: false
    property int  resizeRow: -1
    property int  resizeColIdx: -1
    property int  resizeW: 0
    property bool tableOverBorder: false   // hover near a column border → resize cursor

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

    // Inline table grips (multi-select 2026-08-21): hover bands strictly
    // OUTSIDE the grid rect (top band in the block's 32px top margin, left
    // band in the page margin) — click = select row/column (Shift span /
    // Cmd toggle), drag = reorder, the full-frame grip strips' semantics.
    // ALL state on root: delegates are pooled and may recycle mid-drag.
    property int    gripTableRow: -1     // logical row of the table under the grips
    property string gripKind: ""         // hover: "" | "col" | "row"
    property int    gripIndex: -1
    property bool   gripDragging: false
    property string gripDragKind: ""
    property int    gripFrom: -1
    property int    gripDropGap: -1
    property bool   gripMoved: false
    property real   gripPressX: 0        // content coords
    property real   gripPressY: 0
    property int    gripPressMods: 0

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
    // Cell-image resize (the same affordance scoped to a focused table cell that
    // holds an image; Document view only — the full-frame tab keeps just the tint).
    property bool cellImgResizing: false
    property real cellResizeW: 0
    property real cellResizeAspect: 1
    property real _cellResizePressX: 0
    property real _cellResizeStartW: 0
    readonly property var _cellImgBt: (tcur.active && cellForRow(tcur.row)) ? cellForRow(tcur.row).tableItem : null
    readonly property bool cellImgActive: tcur.active && activeTableRow < 0 && activePdfRow < 0 && activeVideoRow < 0 && activeSketchRow < 0
        && _cellImgBt && (blockModel.contentRevision,
                          blockModel.tableCellMedia(tcur.row, tcur.cr, tcur.cc) !== "")
    // The focused cell image's rect in viewport coords (re-evaluated on scroll /
    // table h-scroll / layout / cell change, then mapped from the BlockTable).
    readonly property rect cellImgRect: {
        var dep = flick.contentY + flick.contentX
                + blockModel.layoutRevision + blockModel.contentRevision
                + (_cellImgBt ? _cellImgBt.scrollX : 0) + tcur.cr + tcur.cc + tcur.pos
        if (!cellImgActive || !_cellImgBt) return Qt.rect(0, 0, 0, 0)
        var r = _cellImgBt.cellImageRect(tcur.cr, tcur.cc)
        var tl = _cellImgBt.mapToItem(root, r.x, r.y)
        return Qt.rect(tl.x, tl.y, r.width, r.height)
    }
    // Column inner width for the focused cell (the resize upper bound).
    readonly property real cellImgMaxW: (cellImgActive && _cellImgBt) ? _cellImgBt.colW(tcur.cc) - 16 : 800

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
    function laneDropAim(cx, cy, excludeLo, excludeCount) {
        const top = { gap: gapForY(cy), lane: -1, besideRow: -1, besideSide: -1 }
        const pageX = cx - leftEdge
        if (pageX < 0 || pageX > pageWidth) return top
        const hit = blockModel.blockAt(pageX, Math.max(0, cy))
        if (hit < 0 || (hit >= excludeLo && hit < excludeLo + excludeCount)) return top
        const t = blockModel.typeForRow(hit)
        if (t !== 7 && t !== 10) {
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
        let carries = false                        // split rows move whole, between top-level rows only
        for (let k = blockDragRow; k < blockDragRow + blockDragCount; ++k)
            if (blockModel.typeForRow(k) === 10) carries = true
        const aim = carries ? { gap: gapForY(cy), lane: -1, besideRow: -1, besideSide: -1 }
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
    // left/right margins (leftEdge each side), stretched by wide content
    // (sheetSpan) — keeps the field tone; the area beyond it drops to the
    // window-shell tone, so the document reads as a constrained shape that
    // follows the width setting. Tracks the pan and the ruler's live width
    // preview (pageWidth includes previewWidth).
    Rectangle {
        readonly property real sheetRight: root.sheetSpan - flick.contentX
        visible: flick.visible && width > 0
        x: sheetRight
        width: Math.max(0, parent.width - sheetRight)
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
        && activeTableRow < 0 && activePdfRow < 0   // not in a full-frame tab
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
    // Drop-onto-a-cell target (an image dragged over a table cell); −1 = none. When
    // set, the block-insertion gap line is suppressed and the cell is highlighted.
    property int dropTableRow: -1
    property int dropCellR: -1
    property int dropCellC: -1
    function clearDropState() {
        imageDropGap = -1; dropTableRow = -1; dropCellR = -1; dropCellC = -1
        imageDropLane = -1; imageDropBesideRow = -1; imageDropBesideSide = -1
    }
    // Aim a drag at a content point: a table cell wins (→ image into cell), else a
    // block-insertion gap. Mutually exclusive, so the affordances don't both show.
    function aimDrop(cx, cy) {
        var th = root.tableHitAt(cx, cy)
        imageDropLane = -1; imageDropBesideRow = -1; imageDropBesideSide = -1
        if (th) { dropTableRow = th.row; dropCellR = th.r; dropCellC = th.c; imageDropGap = -1; return }
        dropTableRow = -1; dropCellR = -1; dropCellC = -1
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
            // A drop lands the caret on the new media (or table cell) → leaving the
            // edited text block, so consume its inline md first (commit doesn't move
            // rows, so the drop-target indices below stay valid).
            blockModel.commitMarkdown(cursor.focusRow)
            // Over a table cell → drop the (first loadable) image into that cell.
            if (root.dropTableRow >= 0) {
                var tr = root.dropTableRow, cr = root.dropCellR, cc = root.dropCellC, ok = false
                for (var j = 0; j < drop.urls.length && !ok; ++j)
                    if (blockModel.tableSetCellImageFromUrl(tr, cr, cc, drop.urls[j].toString())) ok = true
                root.clearDropState()
                if (ok) { cursor.setCaret(tr, 0); tcur.place(cr, cc, 0); root.ensureVisible(tr) }
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
        readonly property int loRow: anchorFirst ? anchorRow : focusRow
        readonly property int loCol: anchorFirst ? anchorCol : focusCol
        readonly property int hiRow: anchorFirst ? focusRow : anchorRow
        readonly property int hiCol: anchorFirst ? focusCol : anchorCol
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
        // Opaque blocks (table/media/divider) hold non-prose content (a table's is
        // JSON) — a cross-block text merge would spill it. Never merge across one.
        function opaque(r) { var t = blockModel.typeForRow(r); return t === 7 || t === 3 || t === 6 }
        // Is the CARET on a media/divider? (Tables route to tcur, never reach these
        // text ops.) These blocks have no text caret, so text ops must not edit them.
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

    // --- Table sub-cursor. Active only while the main caret sits on a table
    // block (cursor.focusRow). Tracks the active cell (cr,cc) and an in-cell text
    // caret/selection (pos/anchorPos). Edits go through BlockModel's table seam;
    // navigation off the grid edge hands back to the main cursor (exitTable).
    QtObject {
        id: tcur
        property int cr: 0
        property int cc: 0
        property int pos: 0
        property int anchorPos: 0
        // Rectangular cell-range selection (−1 = none); set by cross-cell drag.
        property int rangeR0: -1
        property int rangeC0: -1
        property int rangeR1: -1
        property int rangeC1: -1
        function clearRange() { rangeR0 = -1; rangeC0 = -1; rangeR1 = -1; rangeC1 = -1 }
        function setRange(r0, c0, r1, c1) { rangeR0 = r0; rangeC0 = c0; rangeR1 = r1; rangeC1 = c1 }
        // Shift+arrow: grow/shrink the range by moving its head; the anchor
        // stays at the focused cell (rendering normalises the corners).
        function extendRange(dr, dc) {
            clearSets()
            if (rangeR0 < 0) { rangeR0 = cr; rangeC0 = cc; rangeR1 = cr; rangeC1 = cc }
            rangeR1 = Math.max(0, Math.min(rows() - 1, rangeR1 + dr))
            rangeC1 = Math.max(0, Math.min(cols() - 1, rangeC1 + dc))
            cursor.sync()
        }
        // --- Selection SETS (multi-select, 2026-08-21): homogeneous — a set
        // of whole rows OR a set of whole columns OR the cell rect above,
        // never mixed. JS array contents don't notify, so every writer
        // reassigns a fresh array AND bumps selRev (the BlockTable bindings
        // key on it). lastSelR/C = the Shift-span anchors (last plain pick).
        property var selRows: []
        property var selCols: []
        property int selRev: 0
        property int lastSelR: -1
        property int lastSelC: -1
        function clearSets() {
            if (!selRows.length && !selCols.length && lastSelR < 0 && lastSelC < 0) return
            selRows = []; selCols = []; lastSelR = -1; lastSelC = -1; selRev++
        }
        function clearAll() { clearRange(); clearSets() }
        readonly property bool hasSel: rangeR0 >= 0 || selRows.length > 0 || selCols.length > 0
        function setRowSel(a, anchor) {
            clearRange(); selCols = []; lastSelC = -1
            selRows = a; lastSelR = anchor; selRev++; cursor.sync()
        }
        function setColSel(a, anchor) {
            clearRange(); selRows = []; lastSelR = -1
            selCols = a; lastSelC = anchor; selRev++; cursor.sync()
        }
        function toggleRow(r) {
            var a = selRows.slice(); var i = a.indexOf(r)
            i >= 0 ? a.splice(i, 1) : a.push(r)
            setRowSel(a, r)
        }
        function toggleCol(c) {
            var a = selCols.slice(); var i = a.indexOf(c)
            i >= 0 ? a.splice(i, 1) : a.push(c)
            setColSel(a, c)
        }
        function extendRowsTo(r) {
            var a0 = lastSelR >= 0 ? lastSelR : cr, a = []
            for (var i = Math.min(a0, r); i <= Math.max(a0, r); ++i)
                if (i >= blockModel.tableHeaderRows(row)) a.push(i)
            setRowSel(a, a0)   // the anchor stays put across repeated shift-clicks
        }
        function extendColsTo(c) {
            var a0 = lastSelC >= 0 ? lastSelC : cc, a = []
            for (var i = Math.min(a0, c); i <= Math.max(a0, c); ++i) a.push(i)
            setColSel(a, a0)
        }
        // Shift+click in a CELL: rect from the anchor (spreadsheet standard).
        function extendTo(r, c) {
            clearSets()
            if (rangeR0 < 0) { rangeR0 = cr; rangeC0 = cc }
            rangeR1 = r; rangeC1 = c
            cursor.sync()
        }
        // The op-targeting switch for menus/keys.
        function selKind() {
            if (selRows.length) return "rows"
            if (selCols.length) return "cols"
            if (rangeR0 >= 0) return "rect"
            return "none"
        }
        readonly property int row: cursor.focusRow
        // Active only when the document selection is COLLAPSED on a table row
        // (anchor and focus on the same table; cols are always 0 there). A
        // document range that merely passes through a table stays a document
        // range — ⌘C/⌘V/arrows keep their document meaning (2026-09-09).
        readonly property bool active: (blockModel.layoutRevision, blockModel.contentRevision,
                                        blockModel.typeForRow(cursor.focusRow) === 7
                                        && cursor.anchorRow === cursor.focusRow)

        function rows() { return Math.max(1, blockModel.tableRows(row)) }
        function cols() { return Math.max(1, blockModel.tableColumns(row)) }
        function text() { return blockModel.tableCell(row, cr, cc) }
        function clampPos() { pos = Math.max(0, Math.min(pos, text().length)) }

        // Place the caret at cell (r,c), char `p` (default end), collapsing selection.
        function place(r, c, p) {
            cr = Math.max(0, Math.min(r, rows() - 1))
            cc = Math.max(0, Math.min(c, cols() - 1))
            pos = (p === undefined) ? text().length : p
            clampPos(); anchorPos = pos
            clearAll()
            cursor.sync()
        }

        // Text mutations go through the span-aware model ops so inline formatting
        // stays glued to its characters (tableCellInsert/Delete shift the spans).
        function delSel() {
            var lo = Math.min(pos, anchorPos), hi = Math.max(pos, anchorPos)
            blockModel.tableCellDelete(row, cr, cc, lo, hi)
            pos = lo; anchorPos = lo
        }
        function type(ch) {
            if (pos !== anchorPos) delSel()
            // Never type INSIDE a chip (atomic, DT-2) — snap to its far edge.
            var tr = blockModel.tableChoiceRangeAt(row, cr, cc, pos)
            if (tr.length === 2 && pos > tr[0]) { pos = tr[1]; anchorPos = pos }
            blockModel.tableCellInsert(row, cr, cc, pos, ch)
            pos += ch.length; anchorPos = pos; cursor.sync()
        }
        function backspace() {
            if (pos !== anchorPos) { delSel(); cursor.sync(); return }
            if (pos > 0) {
                // A chip is atomic: a backspace touching ANY of it removes the
                // whole chip (label + span) — the block-chip rule.
                var br = blockModel.tableChoiceRangeAt(row, cr, cc, pos - 1)
                if (br.length === 2) {
                    blockModel.tableRemoveChoiceAt(row, cr, cc, br[0])
                    pos = br[0]; anchorPos = pos; cursor.sync(); return
                }
                blockModel.tableCellDelete(row, cr, cc, pos - 1, pos); pos--; anchorPos = pos
            }
            // At the cell start, Backspace removes the cell's image (it sits above
            // the text), mirroring backspace-at-block-start.
            else if (blockModel.tableCellMedia(row, cr, cc) !== "") blockModel.tableClearCellMedia(row, cr, cc)
            cursor.sync()
        }
        function forwardDelete() {
            if (pos !== anchorPos) { delSel(); cursor.sync(); return }
            if (pos < text().length) {
                var fr = blockModel.tableChoiceRangeAt(row, cr, cc, pos)
                if (fr.length === 2) {
                    blockModel.tableRemoveChoiceAt(row, cr, cc, fr[0])
                    pos = fr[0]; anchorPos = pos; cursor.sync(); return
                }
                blockModel.tableCellDelete(row, cr, cc, pos, pos + 1)
            }
            // Forward-delete in an empty cell also clears its image.
            else if (text().length === 0 && blockModel.tableCellMedia(row, cr, cc) !== "")
                blockModel.tableClearCellMedia(row, cr, cc)
            cursor.sync()
        }
        function left(shift) {
            if (pos > 0) {
                // Stepping INTO a chip hops to its near edge (atomic).
                var lr = blockModel.tableChoiceRangeAt(row, cr, cc, pos - 1)
                pos = (lr.length === 2) ? lr[0] : pos - 1
            }
            else if (cc > 0) { cc--; pos = text().length }
            else if (cr > 0) { cr--; cc = cols() - 1; pos = text().length }
            if (!shift) anchorPos = pos
            cursor.sync()
        }
        function right(shift) {
            if (pos < text().length) {
                var rr = blockModel.tableChoiceRangeAt(row, cr, cc, pos)
                pos = (rr.length === 2) ? rr[1] : pos + 1
            }
            else if (cc < cols() - 1) { cc++; pos = 0 }
            else if (cr < rows() - 1) { cr++; cc = 0; pos = 0 }
            if (!shift) anchorPos = pos
            cursor.sync()
        }
        function up() {
            if (cr > 0) { cr--; clampPos(); anchorPos = pos; cursor.sync() }
            else root.exitTable(-1)
        }
        function down() {
            if (cr < rows() - 1) { cr++; clampPos(); anchorPos = pos; cursor.sync() }
            else root.exitTable(1)
        }
        function tab(shift) {
            if (shift) {
                if (cc > 0) cc--
                else if (cr > 0) { cr--; cc = cols() - 1 }
            } else {
                if (cc < cols() - 1) cc++
                else {
                    if (cr >= rows() - 1) blockModel.tableInsertRow(row, rows())   // grow off the end
                    cr++; cc = 0
                }
            }
            pos = 0; anchorPos = text().length     // select the cell (Excel-style)
            cursor.sync()
        }
        function enter(shift) {
            if (shift) { type("\n"); return }       // newline within the cell
            if (cr < rows() - 1) cr++
            else { blockModel.tableInsertRow(row, rows()); cr++ }
            pos = 0; anchorPos = 0; cursor.sync()
        }
    }

    // Move the main caret out of the focused table (dir<0 up, dir>0 down).
    function exitTable(dir) {
        var r = cursor.focusRow, n = blockModel.count
        if (dir < 0 && r > 0) cursor.setCaret(r - 1, blockModel.contentForRow(r - 1).length)
        else if (dir > 0 && r < n - 1) cursor.setCaret(r + 1, 0)
        root.ensureVisible(cursor.focusRow)
    }
    // Enter a table at `row` from an adjacent block: top-left from above, bottom-
    // left from below; `edge` is -1 (came from below) or +1 (came from above).
    function enterTable(row, fromAbove) {
        cursor.setCaret(row, 0)
        tcur.place(fromAbove ? 0 : tcur.rows() - 1, 0, 0)
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
        var row = blockModel.blockAt(cx - root.leftEdge, Math.max(0, cy))
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
        var row = blockModel.blockAt(cx - root.leftEdge, Math.max(0, cy))
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
        var row = blockModel.blockAt(cx - root.leftEdge, Math.max(0, cy))
        if (blockModel.typeForRow(row) !== 2) return -1
        var cell = cellForRow(row)
        if (!cell || !cell.langChip || !cell.langChip.visible) return -1
        var p = cell.langChip.mapFromItem(mouse, cx, cy)
        var pad = 2
        if (p.x >= -pad && p.x <= cell.langChip.width + pad
            && p.y >= -pad && p.y <= cell.langChip.height + pad) return row
        return -1
    }
    // (cx, cy) in CONTENT coordinates → {row, r, c, pos} if over a table, else
    // null. Delegates to the table's own BlockTable.cellAtPoint (its delegate
    // can't own a MouseArea — the document mouse layer sits above it).
    function tableHitAt(cx, cy) {
        var row = blockModel.blockAt(cx - root.leftEdge, Math.max(0, cy))
        if (blockModel.typeForRow(row) !== 7) return null
        var dcell = cellForRow(row)
        var bt = dcell ? dcell.tableItem : null
        if (!bt) return null
        var p = bt.mapFromItem(mouse, cx, cy)
        var hit = bt.cellAtPoint(p.x, p.y)
        return { row: row, r: hit.r, c: hit.c, pos: hit.pos }
    }

    // Shared table mouse interaction (used by both the document central handler and
    // the full-frame tab view). `bt` is the BlockTable; (lx,ly) are bt-local coords.
    function beginTableInteraction(bt, row, lx, ly, mods) {
        var bc = bt.columnBorderAt(lx)
        if (bc >= 0) {                                       // near a border → resize
            root.tableResizing = true; root.resizeRow = row; root.resizeColIdx = bc
            root.resizeW = bt.widthForDrag(bc, lx)
            return
        }
        // Shift+click: SAME cell with no live rect → extend the in-cell TEXT
        // selection to the click point (the multi-word gesture); any other
        // cell (or a live rect) → extend the cell rect from the anchor
        // (spreadsheet standard). Return WITHOUT arming tableDragging — a
        // same-cell jitter in updateTableInteraction would collapse the rect.
        if ((mods & Qt.ShiftModifier) && tcur.active && row === cursor.focusRow) {
            var sh = bt.cellAtPoint(lx, ly)
            if (sh.r === tcur.cr && sh.c === tcur.cc && tcur.rangeR0 < 0
                && !tcur.selRows.length && !tcur.selCols.length) {
                tcur.pos = sh.pos; tcur.clampPos()   // anchorPos stays — text extend
                cursor.sync()
            } else {
                tcur.extendTo(sh.r, sh.c)
            }
            return
        }
        var hit = bt.cellAtPoint(lx, ly)
        // Inline chip in a text cell (2026-08-21) → the picker, not a caret
        // placement; the press never arms dragging (the block-chip rule).
        var chipRng = blockModel.tableChoiceRangeAt(row, hit.r, hit.c, hit.pos)
        if (chipRng.length === 2) {
            if (row !== cursor.focusRow) blockModel.commitMarkdown(cursor.focusRow)
            cursor.setCaret(row, 0)
            tcur.place(hit.r, hit.c, chipRng[1])   // park after the chip
            var cpt = bt.mapToItem(root, lx, ly + 14)
            root.openCellChoicePicker(row, hit.r, hit.c, chipRng[0], cpt.x, cpt.y)
            return
        }
        if (row !== cursor.focusRow) blockModel.commitMarkdown(cursor.focusRow)
        cursor.setCaret(row, 0)
        tcur.place(hit.r, hit.c, hit.pos)
        root.tableDragging = true
        root.tableAnchorR = hit.r; root.tableAnchorC = hit.c; root.tableAnchorPos = hit.pos
    }
    function updateTableInteraction(bt, lx, ly) {
        if (root.tableResizing) { root.resizeW = bt.widthForDrag(root.resizeColIdx, lx); return }
        if (root.tableDragging) {
            var hit = bt.cellAtPoint(lx, ly)
            if (hit.r === root.tableAnchorR && hit.c === root.tableAnchorC) {
                tcur.clearAll(); tcur.cr = hit.r; tcur.cc = hit.c
                tcur.anchorPos = root.tableAnchorPos; tcur.pos = hit.pos
            } else { tcur.clearSets(); tcur.setRange(root.tableAnchorR, root.tableAnchorC, hit.r, hit.c) }
            cursor.sync()
        }
    }
    function endTableInteraction() {
        if (root.tableResizing) {
            blockModel.tableSetColWidth(root.resizeRow, root.resizeColIdx, root.resizeW)
            root.tableResizing = false; root.resizeColIdx = -1
        }
        root.tableDragging = false
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
            if (blockModel.typeForRow(nx) === 7) root.enterTable(nx, true)
            else cursor.move(nx, 0, shift)
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
            if (blockModel.typeForRow(pv) === 7) root.enterTable(pv, false)
            else cursor.move(pv, blockModel.contentForRow(pv).length, shift)
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
        if (pt === 7) {                           // table before: step into its last cell
            if (!repeat && blockModel.contentForRow(row).length === 0 && blockModel.count > 1)
                blockModel.removeBlock(row)       // drop the empty trailing block
            root.enterTable(prev, false); root.ensureVisible(prev); return
        }
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
        if (nt === 7) { root.enterTable(next, true); return }                                   // step into the table
        if (nt === 3 || nt === 6) { cursor.setCaret(next, 0); root.ensureVisible(next); return } // select first
        const col = cursor.focusCol
        blockModel.deleteRange(row, blockModel.contentForRow(row).length, next, 0)   // pull the next block up
        cursor.setCaret(row, col)
    }
    // --- Lane gestures (SR-3 S7b) ---
    // The lane gap under a page-relative x in the split row holding `row`: {record, index}, or null.
    function dividerAt(row, pageX) {
        const rec = blockModel.splitRowOf(row)
        if (rec < 0) return null
        const head = blockModel.tableHeadOf(rec)
        if (head >= 0) {   // SR-4 A6: a table column's right border (the last column's too) → its px width
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
        if (t === 7 || t === 10) return -1
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
        if (head >= 0) {   // px, no fraction snaps; a column never goes under 48
            dividerPreviewX = Math.max(blockModel.tableColumnLeft(head, dividerDragIndex) + 48, pageX)
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
            const r = blockModel.gridRowOf(pullRow), c = blockModel.gridColumnOf(pullRow)
            const at = pullSide === 0 ? c + 1 : c
            if (blockModel.gridInsertColumn(head, at)) root.landInCell(head, r, at)
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
        const cells = blockModel.gridCellCount(head, r)
        if (cells <= 0) return
        const blocks = blockModel.gridCellRows(head, r, Math.max(0, Math.min(c, cells - 1)))
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
        const r = blockModel.gridRowOf(row), c = blockModel.gridColumnOf(row)
        const rows = blockModel.gridRowCount(head)
        if (r < rows - 1) { landInCell(head, r + 1, c); return }
        if (repeat) return
        if (r >= blockModel.headerCount(head) && blockModel.gridRowIsEmpty(head, r)) {
            const p = blockModel.gridExitRow(head)
            if (p >= 0) { cursor.setCaret(p, 0); root.ensureVisible(p) }
            return
        }
        if (blockModel.gridInsertRow(head, rows)) landInCell(head, rows, c)
    }
    // A7 (SR-4 S6c): a selection whose ends sit in different cells of one table is a cell
    // rectangle {head, r0, c0, r1, c1}; null otherwise (inside one cell it's blocks/characters).
    readonly property var cellRect: {
        const dep = blockModel.contentRevision
        if (!cursor.hasSel) return null
        const ha = blockModel.tableHeadOf(cursor.anchorRow), hf = blockModel.tableHeadOf(cursor.focusRow)
        if (ha < 0 || ha !== hf) return null
        const ca = blockModel.gridColumnOf(cursor.anchorRow), cf = blockModel.gridColumnOf(cursor.focusRow)
        if (ca < 0 || cf < 0) return null
        const ra = blockModel.gridRowOf(cursor.anchorRow), rf = blockModel.gridRowOf(cursor.focusRow)
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
        if (o.kind === "table") blockModel.gridDeleteTable(o.head)
        else blockModel.gridDeleteRow(o.head, o.r)
        let land = Math.min(o.head, blockModel.count - 1)
        if (blockModel.tableHeadOf(land) >= 0 && blockModel.headerCount(land) > 0 && o.kind === "row") {
            const b = blockModel.gridCellAt(land, Math.min(o.r, blockModel.gridRowCount(land) - 1), 0)
            if (b >= 0) land = b
        }
        if (land >= 0 && blockModel.typeForRow(land) === 10) land = blockModel.nextLeaf(land - 1)
        cursor.setCaret(Math.max(0, land), 0)
        root.ensureVisible(Math.max(0, land))
    }
    // Block menu → a derived-table op on the right-clicked cell (SR-4 S7a). `op(head, r, c)` returns
    // the [r, c] to land the caret in, or null; when the table is gone the caret takes the nearest block.
    function gridMenuOp(op) {
        const h = blockModel.tableHeadOf(root.menuRow)
        if (h < 0) return
        const land = op(h, blockModel.gridRowOf(root.menuRow), blockModel.gridColumnOf(root.menuRow))
        if (blockModel.headerCount(h) > 0 && blockModel.tableHeadOf(h) === h) {
            if (land) root.landInCell(h, Math.min(land[0], blockModel.gridRowCount(h) - 1), Math.max(0, land[1]))
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
        const r = blockModel.gridRowOf(row), c = blockModel.gridColumnOf(row)
        if (blockModel.gridInsertRow(head, r + 1)) landInCell(head, r + 1, c)
    }
    // The caret's typed-cell kind: 1 choice, 2 check, 0 otherwise (header rows are text).
    function typedCellHere() {
        const row = cursor.focusRow, head = blockModel.tableHeadOf(row)
        if (head < 0 || blockModel.isHeaderRow(row)) return 0
        const c = blockModel.gridColumnOf(row)
        return c < 0 ? 0 : blockModel.gridColumnKind(head, c)
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
                const head = blockModel.tableHeadOf(cursor.focusRow), rows = blockModel.gridRowCount(head)
                if (blockModel.gridInsertRow(head, rows)) root.landInCell(head, rows, 0)
                return
            }
            if (t < 0) return
            const tt = blockModel.typeForRow(t)
            if (tt === 7) { root.enterTable(t, !back); return }
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
            if (blockModel.typeForRow(below) === 7) root.enterTable(below, true)
            else cursor.move(below, colAtGoalX(below, 2), shift)
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
            if (blockModel.typeForRow(above) === 7) { root.enterTable(above, false); return }
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
        function ok(r) { var t = blockModel.typeForRow(r); return t !== 3 && t !== 6 && t !== 7 && t !== 10 }   // never a record
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
        if (tcur.active) { applyCellFormat(kind); return }   // table: format the cell selection
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
    // Bold/italic/underline/strike inside a table. A multi-cell range formats every
    // cell whole (one undo step); otherwise toggle the span over the in-cell text
    // selection (cells have no armed-toggle path, so a caret-only does nothing).
    function applyCellFormat(kind) {
        if (tcur.rangeR0 >= 0) {
            var r0 = Math.min(tcur.rangeR0, tcur.rangeR1), r1 = Math.max(tcur.rangeR0, tcur.rangeR1)
            var c0 = Math.min(tcur.rangeC0, tcur.rangeC1), c1 = Math.max(tcur.rangeC0, tcur.rangeC1)
            var allOn = true, rr, cc, len
            for (rr = r0; rr <= r1 && allOn; ++rr)
                for (cc = c0; cc <= c1; ++cc) {
                    len = blockModel.tableCell(tcur.row, rr, cc).length
                    if (len > 0 && !blockModel.tableCellHasFormat(tcur.row, rr, cc, 0, len, kind)) { allOn = false; break }
                }
            blockModel.beginGroup(tcur.row, tcur.row)
            for (rr = r0; rr <= r1; ++rr)
                for (cc = c0; cc <= c1; ++cc) {
                    len = blockModel.tableCell(tcur.row, rr, cc).length
                    if (len > 0) blockModel.tableSetCellFormat(tcur.row, rr, cc, 0, len, kind, !allOn)
                }
            blockModel.endGroup()
            cursor.sync()
            return
        }
        var lo = Math.min(tcur.pos, tcur.anchorPos), hi = Math.max(tcur.pos, tcur.anchorPos)
        if (lo === hi) return
        var on = !blockModel.tableCellHasFormat(tcur.row, tcur.cr, tcur.cc, lo, hi, kind)
        blockModel.tableSetCellFormat(tcur.row, tcur.cr, tcur.cc, lo, hi, kind, on)
        cursor.sync()
    }
    function clearCellFormatting() {
        // A cell range → strip every selected cell's inline formatting (one undo).
        if (tcur.rangeR0 >= 0) {
            var r0 = Math.min(tcur.rangeR0, tcur.rangeR1), r1 = Math.max(tcur.rangeR0, tcur.rangeR1)
            var c0 = Math.min(tcur.rangeC0, tcur.rangeC1), c1 = Math.max(tcur.rangeC0, tcur.rangeC1)
            blockModel.beginGroup(tcur.row, tcur.row)
            for (var rr = r0; rr <= r1; ++rr)
                for (var cc = c0; cc <= c1; ++cc)
                    blockModel.tableClearCellFormat(tcur.row, rr, cc, 0, blockModel.tableCell(tcur.row, rr, cc).length)
            blockModel.endGroup()
            cursor.sync()
            return
        }
        var lo = Math.min(tcur.pos, tcur.anchorPos), hi = Math.max(tcur.pos, tcur.anchorPos)
        if (lo === hi) { hi = blockModel.tableCell(tcur.row, tcur.cr, tcur.cc).length; lo = 0 }
        blockModel.tableClearCellFormat(tcur.row, tcur.cr, tcur.cc, lo, hi)
        cursor.sync()
    }
    // Revert-to-default colour: strip fg + bg from the selection — table cell(s)
    // (cell-level colours) or selected text (colour spans).
    function revertColors() {
        if (tcur.active) {
            var fr = cursor.focusRow
            blockModel.beginGroup(fr, fr)   // fg+bg clear = ONE undo entry (was two)
            if (tcur.selRows.length) {
                blockModel.tableSetRowsColor(fr, tcur.selRows, true, "")
                blockModel.tableSetRowsColor(fr, tcur.selRows, false, "")
            } else if (tcur.selCols.length) {
                blockModel.tableSetColsColor(fr, tcur.selCols, true, "")
                blockModel.tableSetColsColor(fr, tcur.selCols, false, "")
            } else {
                var r0, c0, r1, c1
                if (tcur.rangeR0 >= 0) { r0 = tcur.rangeR0; c0 = tcur.rangeC0; r1 = tcur.rangeR1; c1 = tcur.rangeC1 }
                else { r0 = tcur.cr; c0 = tcur.cc; r1 = tcur.cr; c1 = tcur.cc }
                blockModel.tableSetCellColor(fr, r0, c0, r1, c1, true, "")    // clear fg
                blockModel.tableSetCellColor(fr, r0, c0, r1, c1, false, "")   // clear bg
            }
            blockModel.endGroup()
            cursor.sync()
            return
        }
        if (!cursor.hasSel) return
        blockModel.beginGroup(cursor.loRow, cursor.hiRow)
        for (var r = cursor.loRow; r <= cursor.hiRow; ++r) {
            blockModel.setTextColor(r, rowSelStart(r), rowSelEnd(r), "")
            blockModel.setHighlight(r, rowSelStart(r), rowSelEnd(r), "")
        }
        blockModel.endGroup()
        cursor.sync()
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
        // In a table the DOC cursor is always collapsed (cursor.hasSel is
        // never true there) — tcur.active is the table's "has a target":
        // the selection when live, else the focused cell.
        if (cursor.hasSel || tcur.active) applyColorToSelection(true, "" + hex, true)
        forceActiveFocus()
    }
    // Highlight mirrors the text pen, plus a rail toggle. pickHighlight arms +
    // applies (palette Highlight tab). toggleHighlight (the rail button) flips it:
    // on → arm + highlight the selection; off → unarm + clear it from the selection.
    readonly property bool highlightArmed: cursor.armedBg !== ""
    function pickHighlight(hex) {
        cursor.armedBg = "" + hex
        if (cursor.hasSel || tcur.active) applyColorToSelection(false, "" + hex, true)
        forceActiveFocus()
    }
    function toggleHighlight(hex) {
        if (cursor.armedBg !== "") {                       // currently on → off
            if (cursor.hasSel || tcur.active) applyColorToSelection(false, "", false)   // "" removes it
            cursor.armedBg = ""
            forceActiveFocus()
        } else {
            pickHighlight("" + hex)
        }
    }
    function applyColorToSelection(isFg, hex, coalesce) {
        if (tcur.active) { applyTableColor(isFg, hex); return }   // table: colour the cell(s)
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
    // Right-rail colour applied to a table: fg = text colour, else cell background.
    // A cell range is coloured if one is selected, otherwise just the focused cell.
    function applyTableColor(isFg, hex) {
        var fr = cursor.focusRow
        if (tcur.selRows.length)
            blockModel.tableSetRowsColor(fr, tcur.selRows, isFg, hex)
        else if (tcur.selCols.length)
            blockModel.tableSetColsColor(fr, tcur.selCols, isFg, hex)
        else if (tcur.rangeR0 >= 0)
            blockModel.tableSetCellColor(fr, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1, isFg, hex)
        else
            blockModel.tableSetCellColor(fr, tcur.cr, tcur.cc, tcur.cr, tcur.cc, isFg, hex)
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
        // Table: paste INTO the clicked cell (the same thing ⌘V does there).
        if (t === 7 && r !== undefined && r >= 0) {
            if (cursor.focusRow !== row) blockModel.commitMarkdown(cursor.focusRow)
            cursor.setCaret(row, 0)
            tcur.place(r, c, 0)
            doPaste()
            return
        }
        var textish = !(t === 3 || t === 6 || t === 7)   // Media / Divider / Table
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
    // Table context-menu ops — act on the right-clicked block (menuRow) + cell.
    function tblInsRowAbove() { blockModel.tableInsertRow(menuRow, menuCellR) }
    function tblInsRowBelow() { blockModel.tableInsertRow(menuRow, menuCellR + 1) }
    function tblInsColLeft()  { blockModel.tableInsertColumn(menuRow, menuCellC) }
    function tblInsColRight() { blockModel.tableInsertColumn(menuRow, menuCellC + 1) }
    // Menu op targeting (multi-select 2026-08-21): the SELECTION when the
    // right-clicked cell sits inside a multi-member set, else null (single).
    function menuSelRows() {
        return (tcur.selRows.length > 1 && tcur.selRows.indexOf(menuCellR) >= 0)
               ? tcur.selRows : null
    }
    function menuSelCols() {
        return (tcur.selCols.length > 1 && tcur.selCols.indexOf(menuCellC) >= 0)
               ? tcur.selCols : null
    }
    function tblDelRow() {
        var s = menuSelRows()
        if (s) { tcur.clearAll(); blockModel.tableDeleteRows(menuRow, s) }
        else blockModel.tableDeleteRow(menuRow, menuCellR)
    }
    function tblDelCol() {
        var s = menuSelCols()
        if (s) { tcur.clearAll(); blockModel.tableDeleteColumns(menuRow, s) }
        else blockModel.tableDeleteColumn(menuRow, menuCellC)
    }
    function tblClearRows() { var s = menuSelRows(); if (s) blockModel.tableClearRows(menuRow, s) }
    function tblClearCols() { var s = menuSelCols(); if (s) blockModel.tableClearColumns(menuRow, s) }
    function tblCopyRows() {
        var s = menuSelRows(); if (!s) return
        clipboard.writeTable(blockModel.tableRowsTSV(menuRow, s),
                             blockModel.tableRowsHtml(menuRow, s))
    }
    function tblCopyCols() {
        var s = menuSelCols(); if (!s) return
        clipboard.writeTable(blockModel.tableColsTSV(menuRow, s),
                             blockModel.tableColsHtml(menuRow, s))
    }
    // Cell-rect ops + promotion to whole-row/column sets ("grab a couple of
    // cells, then Select columns" — the rect's span becomes the set).
    function tblSelectRectRows() {
        var lo = Math.min(tcur.rangeR0, tcur.rangeR1), hi = Math.max(tcur.rangeR0, tcur.rangeR1)
        var hdr = blockModel.tableHeaderRows(menuRow), a = []
        for (var r = Math.max(lo, hdr); r <= hi; ++r) a.push(r)
        if (a.length) tcur.setRowSel(a, a[0])
    }
    function tblSelectRectCols() {
        var lo = Math.min(tcur.rangeC0, tcur.rangeC1), hi = Math.max(tcur.rangeC0, tcur.rangeC1)
        var a = []
        for (var c = lo; c <= hi; ++c) a.push(c)
        tcur.setColSel(a, lo)
    }
    function tblCopyRect() {
        clipboard.writeTable(blockModel.tableRangeTSV(menuRow, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1),
                             blockModel.tableRangeHtml(menuRow, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1))
    }
    function tblClearRect() {
        blockModel.tableClearRange(menuRow, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1)
        tcur.clearAll(); cursor.sync()
    }
    // Double-click in a table cell: select the word under the pointer (the
    // document word-select, spoken in cell coordinates).
    function tableWordSelect(bt, row, lx, ly) {
        var hit = bt.cellAtPoint(lx, ly)
        if (row !== cursor.focusRow) blockModel.commitMarkdown(cursor.focusRow)
        cursor.setCaret(row, 0)
        tcur.place(hit.r, hit.c, hit.pos)
        var t = tcur.text()
        var s = hit.pos, e = hit.pos
        while (s > 0 && /\w/.test(t.charAt(s - 1))) s--
        while (e < t.length && /\w/.test(t.charAt(e))) e++
        tcur.anchorPos = s; tcur.pos = e
        cursor.sync()
    }
    function tblToggleHeader(){ blockModel.tableSetHeaderRows(menuRow, blockModel.tableHeaderRows(menuRow) > 0 ? 0 : 1) }
    function tblMoveRow(d)    { blockModel.tableMoveRow(menuRow, menuCellR, menuCellR + d) }
    function tblMoveCol(d)    { blockModel.tableMoveColumn(menuRow, menuCellC, menuCellC + d) }
    function tblDupRow()      { blockModel.tableDuplicateRow(menuRow, menuCellR) }
    function tblDupCol()      { blockModel.tableDuplicateColumn(menuRow, menuCellC) }
    function tblSort(asc)     { blockModel.tableSortByColumn(menuRow, menuCellC, asc) }
    // ⌘D/⌘R: fill the selected cell-range from its top row / left column; with no
    // range, fill the focused cell from the cell above / to its left.
    function tblFill(right) {
        if (!tcur.active) return
        var r0, c0, r1, c1
        if (tcur.rangeR0 >= 0) {
            r0 = Math.min(tcur.rangeR0, tcur.rangeR1); r1 = Math.max(tcur.rangeR0, tcur.rangeR1)
            c0 = Math.min(tcur.rangeC0, tcur.rangeC1); c1 = Math.max(tcur.rangeC0, tcur.rangeC1)
        } else if (right) {
            if (tcur.cc === 0) return
            r0 = tcur.cr; r1 = tcur.cr; c0 = tcur.cc - 1; c1 = tcur.cc
        } else {
            if (tcur.cr === 0) return
            r0 = tcur.cr - 1; r1 = tcur.cr; c0 = tcur.cc; c1 = tcur.cc
        }
        if (right) blockModel.tableFillRight(tcur.row, r0, c0, r1, c1)
        else blockModel.tableFillDown(tcur.row, r0, c0, r1, c1)
    }
    // Whole-row / whole-column selection (the tcur rectangular range) — from the
    // context menu, or a click (not a drag) on a full-frame reorder grip.
    // Whole-row/column selection is a SET now (multi-select 2026-08-21):
    // plain = replace, Shift = contiguous span from the last plain pick,
    // Cmd (Qt.ControlModifier on macOS) = toggle membership. Focus moves to
    // the picked row/column WITHOUT place() — place() clears the sets.
    function gripSelectRow(row, r, mods) {
        if (cursor.focusRow !== row) { blockModel.commitMarkdown(cursor.focusRow); cursor.setCaret(row, 0) }
        if (mods & Qt.ControlModifier)                            tcur.toggleRow(r)
        else if ((mods & Qt.ShiftModifier) && tcur.selRows.length) tcur.extendRowsTo(r)
        else                                                       tcur.setRowSel([r], r)
        tcur.cr = r; tcur.cc = 0; tcur.pos = 0; tcur.anchorPos = 0
        cursor.sync()
    }
    function gripSelectCol(row, c, mods) {
        if (cursor.focusRow !== row) { blockModel.commitMarkdown(cursor.focusRow); cursor.setCaret(row, 0) }
        if (mods & Qt.ControlModifier)                            tcur.toggleCol(c)
        else if ((mods & Qt.ShiftModifier) && tcur.selCols.length) tcur.extendColsTo(c)
        else                                                       tcur.setColSel([c], c)
        tcur.cr = 0; tcur.cc = c; tcur.pos = 0; tcur.anchorPos = 0
        cursor.sync()
    }
    function selectTableRow(row, r)    { gripSelectRow(row, r, 0) }
    function selectTableColumn(row, c) { gripSelectCol(row, c, 0) }
    // Header sort: clicking the right edge of a header cell sorts by that column,
    // toggling direction on repeat. Session-visual state only (the sort itself is
    // a one-shot undoable mutation; nothing persists).
    property int  lastSortRow: -1
    property int  lastSortCol: -1
    property bool lastSortAsc: true
    // bt-local point → inline grip zone. Bands sit OUTSIDE the table rect, so
    // they can never collide with the resize border (±5px), the sort zone
    // (22px) or widget hits — all of which live INSIDE the grid.
    function tableGripAt(bt, lx, ly) {
        if (ly >= -18 && ly < -2 && lx >= 0 && lx < bt.width) {
            var c = bt.colIndexAt(lx)
            return c >= 0 ? { kind: "col", index: c } : null
        }
        if (lx >= -18 && lx < -2 && ly >= 0 && ly < bt.height) {
            var r = bt.bodyRowAt(ly)                 // headers: no grip
            return r >= 0 ? { kind: "row", index: r } : null
        }
        return null
    }
    function commitInlineGripDrag() {
        var row = root.gripTableRow
        if (!root.gripMoved && root.gripFrom >= 0) {
            if (root.gripDragKind === "col") root.gripSelectCol(row, root.gripFrom, root.gripPressMods)
            else                             root.gripSelectRow(row, root.gripFrom, root.gripPressMods)
        } else if (root.gripDropGap >= 0 && root.gripFrom >= 0) {
            var to = root.gripDropGap > root.gripFrom ? root.gripDropGap - 1 : root.gripDropGap
            if (to !== root.gripFrom) {
                if (root.gripDragKind === "col") blockModel.tableMoveColumn(row, root.gripFrom, to)
                else                             blockModel.tableMoveRow(row, root.gripFrom, to)
            }
        }
        root.gripDragging = false; root.gripDragKind = ""; root.gripFrom = -1
        root.gripDropGap = -1; root.gripKind = ""; root.gripIndex = -1
        root.gripTableRow = -1; root.gripPressMods = 0
    }
    function headerSortHit(bt, row, r, c, lx) {     // lx = bt-local x
        if (r !== 0 || blockModel.tableHeaderRows(row) < 1) return false   // glyph lives on the first header row
        var right = bt.columnLeftX(c) + bt.colW(c) - bt.scrollX
        return lx > right - 22 && lx <= right
    }
    function headerSort(row, c) {
        var asc = !(lastSortRow === row && lastSortCol === c && lastSortAsc)
        tcur.clearAll()   // a live selection over re-sorted rows is a lie
        blockModel.tableSortByColumn(row, c, asc)
        lastSortRow = row; lastSortCol = c; lastSortAsc = asc
    }
    function tblAlign(a) {
        var s = menuSelCols()
        if (s) blockModel.tableSetColsAlign(menuRow, s, a)
        else blockModel.tableSetColAlign(menuRow, menuCellC, a)
    }
    function tblColKind()      { return (blockModel.contentRevision, blockModel.tableColumnKind(menuRow, menuCellC)) }
    function tblMakeChoiceCol(){ var s = menuSelCols()
                                 if (s) { blockModel.tableSetColumnsKind(menuRow, s, 1); return }   // options edited per column
                                 blockModel.tableSetColumnKind(menuRow, menuCellC, 1)
                                 Qt.callLater(root.openChoiceEditor, menuRow, menuCellC) }   // set options right away
    function tblMakeCheckCol() { var s = menuSelCols()
                                 if (s) blockModel.tableSetColumnsKind(menuRow, s, 2)
                                 else blockModel.tableSetColumnKind(menuRow, menuCellC, 2) }
    function tblMakeTextCol()  { var s = menuSelCols()
                                 if (s) blockModel.tableSetColumnsKind(menuRow, s, 0)
                                 else blockModel.tableSetColumnKind(menuRow, menuCellC, 0) }
    function tblRemoveImage() { blockModel.tableClearCellMedia(menuRow, menuCellR, menuCellC) }
    readonly property bool menuCellHasImage: blockMenu.isTable
        && blockModel.tableCellMedia(menuRow, menuCellR, menuCellC) !== ""

    // --- Clipboard (copy / cut / paste), table- and text-aware ---

    function doCopy() {
        if (tcur.active) {
            var fr = cursor.focusRow
            if (tcur.selRows.length) {
                clipboard.writeTable(blockModel.tableRowsTSV(fr, tcur.selRows),
                                     blockModel.tableRowsHtml(fr, tcur.selRows))
            } else if (tcur.selCols.length) {
                clipboard.writeTable(blockModel.tableColsTSV(fr, tcur.selCols),
                                     blockModel.tableColsHtml(fr, tcur.selCols))
            } else if (tcur.rangeR0 >= 0) {
                clipboard.writeTable(blockModel.tableRangeTSV(fr, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1),
                                     blockModel.tableRangeHtml(fr, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1))
            } else {
                var t = blockModel.tableCell(fr, tcur.cr, tcur.cc)
                // No text to copy but the cell holds an image → copy the image.
                if (tcur.pos === tcur.anchorPos && t.length === 0
                    && blockModel.tableCellMedia(fr, tcur.cr, tcur.cc) !== "") {
                    clipboard.writeImageFromFile(blockModel.tableCellMediaUrl(fr, tcur.cr, tcur.cc))
                    return
                }
                clipboard.writeText(tcur.pos !== tcur.anchorPos
                    ? t.slice(Math.min(tcur.pos, tcur.anchorPos), Math.max(tcur.pos, tcur.anchorPos)) : t)
            }
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
    // Rich copy (0.5.0): the x-mnd-blocks payload PLUS the flavours other
    // apps read — plain text (a table's TSV), a lone table's HTML, a lone image
    // block's raster. Opaque rows never leak descriptor JSON.
    function copyRange(lR, lC, hR, hC) {
        var json = blockModel.clipboardPayloadForRange(lR, lC, hR, hC)
        var txt  = blockModel.plainTextForRange(lR, lC, hR, hC)
        var html = "", img = ""
        if (lR === hR) {
            var t = blockModel.typeForRow(lR)
            if (t === 7) html = blockModel.tableRangeHtml(lR, 0, 0, blockModel.tableRows(lR) - 1, blockModel.tableColumns(lR) - 1)
            else if (t === 3 && blockModel.mediaKind(lR) === "image") img = blockModel.mediaUrl(lR)
        }
        clipboard.writeBlocks(json, txt, html, img)
    }
    // Insert an inline choice chip at the caret (DT-2, ⌥⌘C 2026-08-20):
    // default tri-state set, picker opens immediately at the new chip
    // (the applyLink insert-mode shape).
    function insertChoiceChip() {
        if (!blockModel.documentOpen || root.inkMode) return
        // Frame tabs refuse — except the full-frame TABLE studio, whose cells
        // are text and take chips like the inline view (2026-08-21).
        if (root.activeFrameId !== "" && root.activeTableRow < 0) return
        // Table cell (inline or full-frame): the cell-span variant.
        if (tcur.active) {
            var trow = tcur.row
            if (tcur.pos !== tcur.anchorPos) tcur.delSel()
            tcur.clearAll()
            var ts = blockModel.tableInsertChoiceAt(trow, tcur.cr, tcur.cc, tcur.pos)
            if (ts < 0) return
            var trng = blockModel.tableChoiceRangeAt(trow, tcur.cr, tcur.cc, ts)
            if (trng.length === 2) { tcur.pos = trng[1]; tcur.anchorPos = tcur.pos; cursor.sync() }
            var bt = (root.activeTableRow >= 0) ? frameTable
                   : (root.cellForRow(trow) ? root.cellForRow(trow).tableItem : null)
            if (bt) {
                var o = bt.cellOriginInView(tcur.cr, tcur.cc)
                var bpt = bt.mapToItem(root, o.x + 8, o.y + bt.rowHeightAt(tcur.cr))
                root.openCellChoicePicker(trow, tcur.cr, tcur.cc, ts, bpt.x, bpt.y)
            }
            return
        }
        if (root.activeFrameId !== "") return   // table studio but no cell focus
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
    function doPaste() {
        // URLs + raster bytes on the SAME clipboard = the screen-capture-app
        // signature (Finder copies carry URLs only). The URL then points at
        // the app's temp file — force the sidecar copy even if the path
        // looks stable, in every URL branch below.
        var ephemeralUrls = clipboard.hasImage()
        // --- Into an active sketch tab: images (copied from our app or outside)
        // drop onto the canvas as an image element; nothing else has a target. ---
        if (root.activeSketchRow >= 0) {
            var su = clipboard.readUrls()              // copied image file(s)
            if (su.length > 0) {
                var anyS = false
                for (var si = 0; si < su.length; ++si)
                    if (blockModel.sketchAddImageFromUrl(root.activeSketchRow, su[si], ephemeralUrls)) anyS = true
                if (anyS) return
            }
            if (clipboard.hasImage())                  // raster (screenshot / Copy image)
                blockModel.sketchAddImageFromClipboard(root.activeSketchRow)
            return
        }
        // --- Our own blocks flavour (0.5.0): a faithful block run. Not into a
        // table cell (the plain flavour of the same copy is the right thing
        // there) and not into a code block (verbatim text wins below). The
        // paster deletes the selection itself, inside ONE undo entry, and
        // reports back through onPasteFinished (synchronously unless assets
        // must be copied across documents). ---
        if (clipboard.hasBlocks() && !tcur.active
            && !(blockModel.typeForRow(cursor.focusRow) === 2
                 && (!cursor.hasSel || cursor.loRow === cursor.hiRow))) {
            var payload = clipboard.readBlocks()
            if (payload.length > 0) {
                if (cursor.hasSel) {
                    var pe = cursor.effectiveRange()
                    paster.startPaste(blockModel, payload, cursor.focusRow, cursor.focusCol,
                                      pe.lR, pe.lC, pe.hR, pe.hC)
                } else {
                    paster.startPaste(blockModel, payload, cursor.focusRow, cursor.focusCol)
                }
                return
            }
        }
        // --- Into a table cell: a copied file drops into the focused cell; then
        // TEXT (TSV → cells, else typed) beats a raster — Excel for Mac puts a
        // picture of the range beside its TSV; a raster only wins when the
        // clipboard has neither text nor html (screenshot / Copy Image). ---
        if (tcur.active) {
            var cu = clipboard.readUrls()              // copied image file (Finder/Preview)
            if (cu.length > 0 && blockModel.tableSetCellImageFromUrl(cursor.focusRow, tcur.cr, tcur.cc, cu[0], ephemeralUrls)) {
                cursor.sync(); return
            }
            var ct = clipboard.readText()
            if (ct.length > 0) {
                if (ct.indexOf("\t") >= 0 || ct.indexOf("\n") >= 0)
                    blockModel.tablePasteTSV(cursor.focusRow, tcur.cr, tcur.cc, ct)
                else tcur.type(ct)
                return
            }
            if (clipboard.hasHtml()) return            // rich text with no plain form: nothing for a cell
            if (clipboard.hasImage() &&                // raster image (screenshot / Copy Image)
                blockModel.tableSetCellImageFromClipboard(cursor.focusRow, tcur.cr, tcur.cc)) {
                cursor.sync(); return
            }
            return
        }
        // --- Into a CODE block: paste VERBATIM (user-caught 2026-08-21).
        // No HTML flavoring (editors put HTML on the clipboard), no markdown
        // prefix parsing (a "# comment" is not a heading), no TSV table
        // detection (tab-indented code is not a table); newlines and blank
        // lines land in the block exactly as copied. ---
        // (A selection spanning several blocks keeps the normal flow.)
        if (blockModel.typeForRow(cursor.focusRow) === 2
            && (!cursor.hasSel || cursor.loRow === cursor.hiRow)) {
            var codeTxt = clipboard.readText()
            if (codeTxt.length > 0) {
                if (cursor.hasSel) cursor.deleteSelection()
                codeTxt = codeTxt.replace(/\r\n/g, "\n").replace(/\r/g, "\n")
                var ccol = cursor.focusCol
                blockModel.insertText(cursor.focusRow, ccol, codeTxt)
                cursor.setCaret(cursor.focusRow, ccol + codeTxt.length)
                root.ensureVisible(cursor.focusRow)
                return
            }
            // nothing textual on the clipboard → the media branches below
        }
        // --- Rich HTML (Word / Google Docs / Excel / web) → structured blocks:
        // headings/lists/paragraphs + bold/italic/underline/strike/links, tables
        // → Table blocks. Falls through if the HTML yields nothing usable (e.g. a
        // bare image wrapper → handled as media below). ---
        if (clipboard.hasHtml()) {
            var html = clipboard.readHtml()
            // Browser "Copy Image": the HTML is a bare remote <img> and the
            // pixels are right here on the clipboard — take the raster below
            // instead of a background download that may never succeed.
            var bareImg = html && html.length > 0 && clipboard.hasImage()
                          && blockModel.htmlIsBareRemoteImage(html)
            if (html && html.length > 0 && !bareImg) {
                var hg = root.pasteGroupBegin()
                var hc = blockModel.pasteHtml(cursor.focusRow, cursor.focusCol, html)
                root.pasteGroupEnd(hg)
                if (hc && hc.length === 2) { cursor.setCaret(hc[0], hc[1]); root.ensureVisible(hc[0]); return }
            }
        }
        // --- Copied file(s) (Finder / Preview "Copy") → import as media, exactly
        // like a drag-drop: image/video/pdf render, anything else → a file chip.
        // (Preview copies an image as a file URL, not raster bytes — without this
        // it would fall through to pasting the path as text.) ---
        var urls = clipboard.readUrls()
        if (urls.length > 0) {
            // Inserting media moves the caret onto it → leaving the text block, so
            // consume its inline md first (in-place text/HTML paste above must NOT
            // do this — it would shift focusCol; here the caret goes to a new block).
            blockModel.commitMarkdown(cursor.focusRow)
            var afterRow = cursor.focusRow, anyU = false
            for (var i = 0; i < urls.length; ++i) {
                var nrU = blockModel.insertMediaFromUrl(afterRow, urls[i], ephemeralUrls)
                if (nrU >= 0) { afterRow = nrU; anyU = true }
            }
            if (anyU) { cursor.setCaret(afterRow, 0); root.ensureVisible(afterRow); return }
        }
        // --- Raster image on the clipboard (screenshot, "Copy Image") → media block. ---
        if (clipboard.hasImage()) {
            blockModel.commitMarkdown(cursor.focusRow)   // caret moves to the new media → consume inline md
            var imgRow = blockModel.insertImageFromClipboard(cursor.focusRow)
            if (imgRow >= 0) {
                cursor.setCaret(imgRow, 0); root.ensureVisible(imgRow)
                return
            }
        }
        // --- Plain text. ---
        var txt = clipboard.readText()
        if (txt.length === 0) return
        // SR-4 A5: text with tabs or newlines pasted in a table cell fills cells from the anchor (a
        // rectangle's top-left), growing the table as needed — one undo step.
        if (blockModel.tableHeadOf(cursor.focusRow) >= 0 && (txt.indexOf("\t") >= 0 || txt.indexOf("\n") >= 0)) {
            const gh = blockModel.tableHeadOf(cursor.focusRow), rect = root.cellRect
            const gland = blockModel.gridPasteTSV(gh, rect ? rect.r0 : blockModel.gridRowOf(cursor.focusRow),
                                                  rect ? rect.c0 : blockModel.gridColumnOf(cursor.focusRow), txt)
            if (gland >= 0) { cursor.setCaret(gland, blockModel.contentForRow(gland).length); root.ensureVisible(gland) }
            return
        }
        var tg = root.pasteGroupBegin()
        if (root.looksTabular(txt)) {                       // rectangular TSV → table block
            blockModel.commitMarkdown(cursor.focusRow)      // caret moves to the new table → consume inline md
            var tr = blockModel.insertGridFromTSV(cursor.focusRow, txt)   // SR-4 S7a: a derived table
            root.pasteGroupEnd(tg)
            if (tr >= 0) { cursor.setCaret(tr, blockModel.contentForRow(tr).length); root.ensureVisible(tr); return }
            tg = false
        }
        // Smart paste: blocks (blank lines separate) + per-line markdown prefixes +
        // inline **bold**/*italic*/`code`/~~strike~~/[links] + ``` fences, all as
        // one undo step.
        var caret = blockModel.pasteText(cursor.focusRow, cursor.focusCol, txt)
        root.pasteGroupEnd(tg)
        if (caret && caret.length === 2) {
            cursor.setCaret(caret[0], caret[1]); root.ensureVisible(caret[0])
        }
    }
    // Rectangular grid signal for paste→table: every non-empty line carries the
    // SAME number of tabs (>=1), across >=2 rows. The strict equal-column check
    // avoids misreading tab-indented prose/code as a table (HTML from Excel is the
    // precise path, handled later); a single-row tabbed paste stays plain text.
    function looksTabular(txt) {
        if (txt.indexOf("\t") < 0) return false
        var lines = txt.replace(/\r\n/g, "\n").replace(/\r/g, "\n").split("\n")
        while (lines.length && lines[lines.length - 1] === "") lines.pop()
        if (lines.length < 2) return false
        var cols = -1
        for (var i = 0; i < lines.length; ++i) {
            if (lines[i] === "") return false        // a blank interior line → not a grid
            var t = lines[i].split("\t").length - 1
            if (t < 1) return false
            if (cols < 0) cols = t; else if (t !== cols) return false
        }
        return true
    }
    function doCut() {
        doCopy()
        if (tcur.active) {
            if (tcur.selRows.length) { blockModel.tableClearRows(cursor.focusRow, tcur.selRows); tcur.clearAll() }
            else if (tcur.selCols.length) { blockModel.tableClearColumns(cursor.focusRow, tcur.selCols); tcur.clearAll() }
            else if (tcur.rangeR0 >= 0) { blockModel.tableClearRange(cursor.focusRow, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1); tcur.clearRange() }
            else if (tcur.pos !== tcur.anchorPos) tcur.delSel()
            else blockModel.tableSetCell(cursor.focusRow, tcur.cr, tcur.cc, "")
            cursor.sync()
        } else if (cursor.hasSel) cursor.deleteSelection()
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
        var t = root.menuIssueInCell ? blockModel.tableCell(root.menuRow, root.menuCellR, root.menuCellC)
                                     : blockModel.contentForRow(root.menuRow)
        return t.substring(it.s, it.e)
    }
    // Fix all: the top suggestion of every issue in the block (or the clicked
    // cell), applied right-to-left so earlier offsets stay valid — ONE undo.
    function applyAllSuggestions() {
        var row = root.menuRow
        if (row < 0) return
        var inCell = root.menuIssueInCell
        var list = inCell ? spell.issuesForCell(row, root.menuCellR, root.menuCellC) : spell.issuesForRow(row)
        var fixes = []
        for (var i = 0; i < list.length; ++i) if (list[i].suggestions.length > 0) fixes.push(list[i])
        if (fixes.length === 0) return
        fixes.sort(function(a, b) { return b.s - a.s })
        blockModel.beginGroup(row, row)
        for (var k = 0; k < fixes.length; ++k) {
            var f = fixes[k]
            if (inCell) blockModel.tableCellReplace(row, root.menuCellR, root.menuCellC, f.s, f.e, f.suggestions[0])
            else blockModel.replaceText(row, f.s, f.e, f.suggestions[0])
        }
        blockModel.endGroup()
        if (!inCell && cursor.focusRow === row)
            cursor.setCaret(row, Math.min(cursor.focusCol, blockModel.contentForRow(row).length))
        spell.flushRow(row)
        root.menuIssue = null
        Toasts.show(fixes.length === 1 ? qsTr("Fixed 1 issue") : qsTr("Fixed %1 issues").arg(fixes.length))
    }
    function applySpellSuggestion(sug) {
        var it = root.menuIssue
        if (!it || sug === undefined || sug === "") return
        if (root.menuIssueInCell) {
            blockModel.tableCellReplace(root.menuRow, root.menuCellR, root.menuCellC, it.s, it.e, sug)
        } else {
            blockModel.replaceText(root.menuRow, it.s, it.e, sug)
            cursor.setCaret(root.menuRow, it.s + sug.length)
        }
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
        if (tcur.active) { clearCellFormatting(); return }   // in a table → clear the cell(s)
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
        var inTable = tcur.active
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
            else if (root.boardMode && root.activeTableRow >= 0) { root.showGridView() }   // board → grid
            else if (inTable) {
                // Escape peels back one layer: in-cell text selection →
                // cell/row/col selection (pre-existing gap: a live range
                // used to survive the exit) → leave the table.
                if (tcur.pos !== tcur.anchorPos) { tcur.anchorPos = tcur.pos; cursor.sync() }
                else if (tcur.hasSel) { tcur.clearAll(); cursor.sync() }
                else root.exitTable(1)
            }
            else if (cursor.hasSel && root.selectionIsSplitRow()
                     && blockModel.tableHeadOf(cursor.loRow) >= 0
                     && blockModel.gridRowCount(blockModel.tableHeadOf(cursor.loRow)) > 1) {
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
                root.selObject = th >= 0 ? { kind: "row", head: th, r: blockModel.gridRowOf(rec), lo: first, hi: last } : null
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
                 && (root.activeFrameId === "" || root.activeTableRow >= 0)
                 && !root.inkMode) {
            root.insertChoiceChip()   // doc view OR the full-frame table studio
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
        // can't invisibly edit the grid underneath (tcur is still pinned to it).
        else if (root.boardMode && root.activeTableRow >= 0) { event.accepted = true }
        // Table mode: route editing/navigation to the cell sub-cursor.
        // Formatting shortcuts come first (else the generic branch swallows them).
        else if (inTable && cmd && k === Qt.Key_B) { applyFormat("bold"); event.accepted = true }
        else if (inTable && cmd && k === Qt.Key_I) { applyFormat("italic"); event.accepted = true }
        else if (inTable && cmd && k === Qt.Key_U) { applyFormat("underline"); event.accepted = true }
        else if (inTable && cmd && shift && k === Qt.Key_X) { applyFormat("strike"); event.accepted = true }
        else if (inTable && cmd && k === Qt.Key_Backslash) { clearCellFormatting(); event.accepted = true }
        else if (inTable && cmd && k === Qt.Key_D) { tblFill(false); event.accepted = true }
        else if (inTable && cmd && k === Qt.Key_R) { tblFill(true); event.accepted = true }
        else if (inTable && cmd && k === Qt.Key_A) {   // ⌘A ladder: cell text → every cell → document
            var aLen = tcur.text().length
            var aLo = Math.min(tcur.pos, tcur.anchorPos), aHi = Math.max(tcur.pos, tcur.anchorPos)
            var cellAll = aLen > 0 && aLo === 0 && aHi === aLen
            var gridAll = tcur.rangeR0 === 0 && tcur.rangeC0 === 0
                          && tcur.rangeR1 === tcur.rows() - 1 && tcur.rangeC1 === tcur.cols() - 1
            if (gridAll || (aLen === 0 && tcur.hasSel)) root.selectAllDocument()
            else if (aLen > 0 && !cellAll && !tcur.hasSel) { tcur.anchorPos = 0; tcur.pos = aLen; cursor.sync() }
            else { tcur.clearSets(); tcur.setRange(0, 0, tcur.rows() - 1, tcur.cols() - 1); cursor.sync() }
            event.accepted = true
        }
        // Page/Home/End leave the table — they move the document caret off it
        // (tcur deactivates as soon as focusRow is no longer a table row).
        else if (inTable && k === Qt.Key_PageDown) { navPageDown(false); event.accepted = true }
        else if (inTable && k === Qt.Key_PageUp)   { navPageUp(false);   event.accepted = true }
        else if (inTable && k === Qt.Key_Home)     { navHome(false);     event.accepted = true }
        else if (inTable && k === Qt.Key_End)      { navEnd(false);      event.accepted = true }
        else if (inTable) {
            if (cmd) { event.accepted = true; return }   // swallow other Cmd-combos (don't type the letter)
            // Shift+arrows: spreadsheet-style range extension. Left/Right first
            // extend the in-cell TEXT selection; crossing the cell edge promotes
            // to a cell range. Up/Down go straight to cells (there's no vertical
            // text selection inside a cell). With a range live, arrows move its
            // head; the anchor stays at the focused cell.
            if (shift && (k === Qt.Key_Left || k === Qt.Key_Right || k === Qt.Key_Up || k === Qt.Key_Down)) {
                if (tcur.rangeR0 >= 0)
                    tcur.extendRange(k === Qt.Key_Down ? 1 : k === Qt.Key_Up ? -1 : 0,
                                     k === Qt.Key_Right ? 1 : k === Qt.Key_Left ? -1 : 0)
                else if (k === Qt.Key_Up)    tcur.extendRange(-1, 0)
                else if (k === Qt.Key_Down)  tcur.extendRange(1, 0)
                else if (k === Qt.Key_Right) {
                    if (tcur.pos >= tcur.text().length) tcur.extendRange(0, 1)
                    else tcur.right(true)
                } else {                                       // Left
                    if (tcur.pos <= 0) tcur.extendRange(0, -1)
                    else tcur.left(true)
                }
                event.accepted = true
                return
            }
            if (tcur.hasSel) {
                // Backspace/Delete over a selection clears its CONTENTS (one
                // undo step; row/col DELETION stays menu-only — destructive);
                // any other key just collapses the selection.
                if (k === Qt.Key_Backspace || k === Qt.Key_Delete) {
                    if (tcur.selRows.length)
                        blockModel.tableClearRows(cursor.focusRow, tcur.selRows)
                    else if (tcur.selCols.length)
                        blockModel.tableClearColumns(cursor.focusRow, tcur.selCols)
                    else
                        blockModel.tableClearRange(cursor.focusRow, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1)
                    tcur.clearAll(); cursor.sync()
                    event.accepted = true
                    return
                }
                tcur.clearAll()                        // any other key collapses it
            }
            if (k === Qt.Key_Right) tcur.right(shift)
            else if (k === Qt.Key_Left) tcur.left(shift)
            else if (k === Qt.Key_Down) tcur.down()
            else if (k === Qt.Key_Up) tcur.up()
            else if (k === Qt.Key_Backspace) tcur.backspace()
            else if (k === Qt.Key_Delete) tcur.forwardDelete()
            else if (k === Qt.Key_Tab) tcur.tab(false)
            else if (k === Qt.Key_Backtab) tcur.tab(true)
            else if (k === Qt.Key_Return || k === Qt.Key_Enter) tcur.enter(shift)
            else if (event.text.length === 1 && event.text >= " ") tcur.type(event.text)
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
                blockModel.gridCycleCellCheck(head, blockModel.gridRowOf(cursor.focusRow), blockModel.gridColumnOf(cursor.focusRow))
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
                root.openGridChoicePicker(blockModel.tableHeadOf(row), blockModel.gridRowOf(row),
                                          blockModel.gridColumnOf(row), event.text === " " ? "" : event.text)
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
                if (d.gridCol >= 0 && Math.abs(g.x - blockModel.tableColumnLeft(d.tableHead, d.gridCol)) > 0.5)
                    fail("table cell row " + r + " at x " + g.x + ", column " + d.gridCol + " starts at "
                         + blockModel.tableColumnLeft(d.tableHead, d.gridCol))
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
                if (head >= 0 && root.leftEdge + blockModel.tableWidth(head) > flick.contentX) ++tableRows
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
        function next(phaseDone) { if (phaseDone) { ++phase; phaseStep = 0 } else ++phaseStep }
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
                if (ty !== 10 && ty !== 7) {
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
                if (phaseStep === 0 || ft === 7 || ft === 3 || ft === 6) restart()
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
                        if (want >= 0 && blockModel.typeForRow(want) !== 7 && cursor.focusRow !== want)
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
                if (phaseStep === 0 || ft === 7 || blockModel.laneForRow(cursor.focusRow) < 0 && rand(4) === 0) {
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
                if (phaseStep % 2 === 0 && t !== 7 && t !== 10) {
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
                case 1: if (head >= 0) blockModel.setContent(blockModel.gridCellAt(head, 1 + rand(3), rand(3)), "cell ".repeat(1 + rand(12))); break
                case 2: if (head >= 0) blockModel.setTableColumnWidth(head, rand(3), rand(2) ? 0 : 220 + rand(300)); break
                case 3: if (head >= 0) blockModel.gridSetColumnKind(head, rand(3), rand(3)); break
                case 4: if (head >= 0) blockModel.gridSortByColumn(head, rand(3), rand(2) === 0); break
                case 5: if (head >= 0) blockModel.gridInsertColumn(head, rand(3)); break
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
                    const r = blockModel.gridRowOf(row), rows = blockModel.gridRowCount(head)
                    switch (rand(9)) {
                    case 7: {   // A6: drag a column's right border — its px width, never under 48
                        const c = blockModel.gridColumnOf(row)
                        const edge = blockModel.tableColumnLeft(head, c) + blockModel.tableColumnWidth(head, c)
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
                        const rowsBefore = blockModel.gridRowCount(head)
                        const a = blockModel.gridCellAt(head, 0, 0)
                        const z = blockModel.gridCellAt(head, rowsBefore - 1, Math.max(0, blockModel.gridCellCount(head, rowsBefore - 1) - 1))
                        if (a >= 0 && z >= 0 && a !== z) {
                            cursor.setCaret(a, 0)
                            cursor.move(z, blockModel.contentForRow(z).length, true)
                            root.selObject = null
                            cursor.deleteSelection()
                            ++checks
                            if (blockModel.tableHeadOf(cursor.focusRow) !== head || blockModel.gridRowCount(head) !== rowsBefore)
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
                        if (r < rows - 1 && blockModel.gridRowOf(cursor.focusRow) !== r + 1)
                            fail("Enter in table row " + r + " landed in row " + blockModel.gridRowOf(cursor.focusRow))
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
    readonly property var poolRows: (blockModel.contentRevision, blockModel.layoutRevision,
        blockModel.visibleBlocks(Math.max(0, flick.contentY - overscanPx),
                                 flick.contentY + flick.height + overscanPx))
    readonly property int poolSize: Math.min(blockModel.count,
        Math.max(poolRows.length, Math.ceil(root.height / 38) + 2 * overscan + 4))
    readonly property int delegateCount: poolSize
    // Which block each pool slot renders. Blocks that stay in view keep their
    // delegate. sync RETURNS the revision and runs inside this binding, so everything
    // reading slotRev before rowForSlot() sees the updated table.
    readonly property int slotRev: viewSlots.sync(poolRows, poolSize)
    // BlockView (the extracted block renderer) reads the editor's controllers
    // through these — ids don't cross file boundaries.
    readonly property var cursorObj: cursor
    readonly property var tcurObj: tcur
    readonly property var flickItem: flick
    readonly property real barFraction: flick.contentHeight > flick.height
        ? flick.contentY / (flick.contentHeight - flick.height) : 0
    readonly property real trueFraction: barFraction
    readonly property int caretRow: cursor.focusRow
    readonly property bool hasSelection: cursor.hasSel
    // A colour target exists in a table whenever the sub-cursor is live:
    // the selection when there is one, else the focused cell (the palette's
    // Apply button reads this).
    readonly property bool tableFocused: tcur.active
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
        visible: root.activeTableRow < 0 && root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0   // hidden in a full-frame tab
        anchors.fill: parent
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
            x: 0; width: root.leftEdge
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
            readonly property real sheetW: root.sheetSpan   // matches the sheet tint
            Rectangle {
                x: 0; width: Math.min(parent.width, parent.sheetW)
                height: parent.height
                color: Theme.colors.bgAlt2
            }
            Rectangle {
                x: parent.sheetW
                width: Math.max(0, parent.width - parent.sheetW)
                height: parent.height
                color: Theme.colors.bgAlt
            }
        }
        Repeater {   // RULES: a faint hairline at each block's top across the
                     // whole field, text column included — a line that INFORMS
                     // ("a block starts here") and stays put under editing,
                     // unlike zebra parity.
            model: root.poolSize
            delegate: Rectangle {
                required property int index
                readonly property int prow: (root.slotRev, viewSlots.rowForSlot(index))
                // Top entries only: a rule marks where a ROW starts, not a lane block.
                visible: prow >= 0 && prow < blockModel.count
                         && (blockModel.contentRevision, blockModel.laneForRow(prow)) < 0
                z: -1
                x: 0
                width: Math.max(flick.width, root.contentSpan)
                y: (blockModel.layoutRevision, blockModel.yForRow(prow))
                height: 1
                color: Theme.colors.border
                // ROLLING VISIBILITY (user ruling): rules earn their ink when
                // they're load-bearing — full strength bounding the focused
                // block ±2 (local orientation), ALL of them during a block
                // drag (the insertion grid), otherwise INVISIBLE (user: "more
                // obvious — completely invisible except for the neighbors").
                // Distance 0 = the two rules bounding the focus block
                // (a rule at prow is the boundary ABOVE block prow).
                readonly property real dist: cursor.focusRow < 0 ? 99
                    : prow <= cursor.focusRow ? cursor.focusRow - prow
                                              : prow - cursor.focusRow - 1
                opacity: root.blockDragging ? 0.55
                       : dist <= 2 ? 0.55
                       : Math.max(0, 0.55 - (dist - 2) * 0.2)
                Behavior on opacity { NumberAnimation { duration: 150 } }
            }
        }

        Repeater {
            id: pool
            model: root.poolSize
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
            readonly property var rows: {
                const dep = blockModel.layoutRevision + blockModel.contentRevision + flick.contentY + flick.contentX
                if (flick.contentX <= root.leftEdge + 1) return []
                const out = []
                const inView = blockModel.visibleBlocks(flick.contentY, flick.contentY + flick.height)
                for (let i = 0; i < inView.length; ++i) {
                    const r = inView[i]
                    if (blockModel.typeForRow(r) !== 10) continue
                    const head = blockModel.tableHeadOf(r)
                    if (head < 0) continue
                    const tw = blockModel.tableWidth(head)
                    if (root.leftEdge + tw <= flick.contentX) continue    // the whole table is scrolled away
                    const padTop = blockModel.tablePadTop(r)
                    out.push({ head: head, gr: blockModel.gridRowOf(r), header: blockModel.isHeaderRow(r),
                               y: blockModel.yForRow(r) + padTop,
                               h: blockModel.heightForRow(r) - padTop - blockModel.tablePadBottom(r),
                               w: blockModel.tableColumnWidth(head, 0), tw: tw })
                }
                return out
            }
            x: flick.contentX
            z: 2.5
            Repeater {
                model: frozenColumn.rows
                delegate: Rectangle {
                    required property var modelData
                    readonly property string bg: (blockModel.contentRevision, blockModel.gridCellBg(modelData.head, modelData.gr, 0))
                    readonly property string fg: (blockModel.contentRevision, blockModel.gridCellFg(modelData.head, modelData.gr, 0))
                    // Pushed off to the left as the table's right edge arrives (never over its last column).
                    x: Math.min(0, root.leftEdge + modelData.tw - modelData.w - flick.contentX)
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
                        text: (blockModel.contentRevision, blockModel.gridCellText(modelData.head, modelData.gr, 0))
                        color: fg !== "" ? fg : Theme.colors.text
                        font.family: Theme.font.body; font.pixelSize: Theme.font.sizeBody; font.bold: modelData.header
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
        Rectangle {   // the corner: the header's first cell, pinned at the top and the left
            id: frozenCorner
            readonly property int head: stickyHeader.has ? stickyHeader.st.head : -1
            visible: stickyHeader.visible && flick.contentX > root.leftEdge + 1 && head >= 0
                     && root.leftEdge + (blockModel.layoutRevision, blockModel.tableWidth(head)) > flick.contentX
            x: flick.contentX + (head >= 0 ? Math.min(0, root.leftEdge + (blockModel.layoutRevision, blockModel.tableWidth(head))
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
                text: frozenCorner.head >= 0 ? (blockModel.contentRevision, blockModel.gridCellText(frozenCorner.head, 0, 0)) : ""
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
                                       blockModel.tableStickyAt(flick.contentY))
            readonly property bool has: st.head !== undefined
            readonly property real headerH: has ? st.headerBottom - st.headerTop : 0
            visible: has && flick.contentY > st.headerTop && flick.contentY < st.tableBottom - headerH
            x: 0
            y: flick.contentY + (has ? Math.min(0, st.tableBottom - headerH - flick.contentY) : 0)
            z: 3
            width: flick.contentWidth
            height: headerH
            Repeater {
                model: stickyHeader.visible ? stickyHeader.st.headerRows : []
                delegate: Item {
                    id: stickyRow
                    required property var modelData
                    required property int index
                    readonly property int rec: modelData
                    readonly property real pad: index === 0 ? blockModel.tablePadTop(rec) : 0
                    y: (blockModel.layoutRevision, blockModel.yForRow(rec)) + pad - stickyHeader.st.headerTop
                    width: stickyHeader.width
                    height: (blockModel.layoutRevision, blockModel.heightForRow(rec)) - pad
                    Repeater {
                        model: (blockModel.contentRevision, blockModel.tableColumnCount(stickyHeader.st.head))
                        delegate: Rectangle {
                            required property int index
                            readonly property int head: stickyHeader.st.head
                            readonly property string bg: (blockModel.contentRevision, blockModel.gridCellBg(head, stickyRow.index, index))
                            readonly property string fg: (blockModel.contentRevision, blockModel.gridCellFg(head, stickyRow.index, index))
                            x: root.leftEdge + (blockModel.layoutRevision, blockModel.tableColumnLeft(head, index))
                            width: (blockModel.layoutRevision, blockModel.tableColumnWidth(head, index))
                            height: stickyRow.height
                            color: bg !== "" ? bg : Theme.colors.surfaceHover
                            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.colors.border }
                            Rectangle { anchors.bottom: parent.bottom; height: 1; width: parent.width; color: Theme.colors.border }
                            Rectangle { visible: index === 0; width: 1; height: parent.height; color: Theme.colors.border }
                            Text {
                                x: 8; y: 6
                                width: parent.width - 16
                                text: (blockModel.contentRevision, blockModel.gridCellText(head, stickyRow.index, index))
                                color: fg !== "" ? fg : Theme.colors.text
                                font.family: Theme.font.body; font.pixelSize: Theme.font.sizeBody; font.bold: true
                                wrapMode: Text.Wrap
                                horizontalAlignment: {
                                    const a = (blockModel.contentRevision, blockModel.gridColAlign(head, index))
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
                       : root.gripDragging ? Qt.ClosedHandCursor
                       : root.gripKind !== "" ? Qt.OpenHandCursor
                       : (root.dividerDragging || root.pulling
                          || root.dividerHoverRecord >= 0 || root.pullHoverRow >= 0) ? Qt.SplitHCursor
                       : (root.tableResizing || root.tableOverBorder) ? Qt.SplitHCursor
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
                    var trow = blockModel.blockAt(m.x - root.leftEdge, m.y)
                    root.menuLinkUrl = ""; root.menuIssue = null
                    if (blockModel.typeForRow(trow) === 7) {
                        var th = root.tableHitAt(m.x, m.y)
                        root.menuCellR = th ? th.r : 0; root.menuCellC = th ? th.c : 0
                        if (th) {                                   // misspelling under the click?
                            var ci = spell.cellIssueAt(trow, th.r, th.c, th.pos)
                            root.menuIssue = ci.ruleId !== undefined ? ci : null; root.menuIssueInCell = true
                        }
                    } else {
                        var rh = root.hitTest(m.x, m.y)              // link / misspelling under the click?
                        root.menuLinkUrl = blockModel.linkAt(rh.row, rh.col)
                        var bi = spell.issueAt(rh.row, rh.col)
                        root.menuIssue = bi.ruleId !== undefined ? bi : null; root.menuIssueInCell = false
                    }
                    root.openBlockMenu(m.x - flick.contentX, m.y - flick.contentY, trow)
                    return
                }
                // (Block drag-reorder starts from the ruler's number handles
                // now — the left grip gutter is retired.)
                cursor.resetGoalX(); cursor.clearMarks()
                // Lane gestures (SR-3 S7b) start before any caret placement.
                if (root.dividerHoverRecord >= 0) {
                    root.beginDividerDrag(root.dividerHoverRecord, root.dividerHoverIndex, m.x - root.leftEdge,
                                          (m.modifiers & Qt.AltModifier) !== 0)
                    return
                }
                if (root.pullHoverRow >= 0) { root.beginPull(root.pullHoverRow, root.pullHoverSide, m.x - root.leftEdge); return }
                // Click into a table cell → place the table caret; arm drag for
                // in-cell text selection / cross-cell range.
                var th = root.tableHitAt(m.x, m.y)
                if (th) {
                    // Typed body cells: choice → option picker; check → cycle state.
                    // Only fires when the click lands ON the chip/checkbox (not anywhere
                    // in the cell) — otherwise the click just selects/edits the cell.
                    var tk = blockModel.tableColumnKind(th.row, th.c)
                    var tbody = th.r >= blockModel.tableHeaderRows(th.row)
                    var dcell = root.cellForRow(th.row), bt = dcell ? dcell.tableItem : null
                    var lp = bt ? bt.mapFromItem(mouse, m.x, m.y) : null
                    // Grip bands first — they live outside the grid rect, where
                    // cellAtPoint's clamp used to drop the caret at row 0.
                    var g = bt ? root.tableGripAt(bt, lp.x, lp.y) : null
                    if (g) {
                        root.gripDragging = true; root.gripDragKind = g.kind
                        root.gripTableRow = th.row; root.gripFrom = g.index
                        root.gripPressX = m.x; root.gripPressY = m.y
                        root.gripPressMods = m.modifiers; root.gripMoved = false
                        root.gripDropGap = g.kind === "col" ? bt.colGapAt(lp.x) : bt.rowGapAt(lp.y)
                        return
                    }
                    if (bt && (tk === 1 || tk === 2) && tbody && bt.widgetHit(lp.x, lp.y)) {
                        if (tk === 1) root.openChoicePicker(th.row, th.r, th.c, m.x - flick.contentX, m.y - flick.contentY)
                        else          blockModel.tableCycleCellCheck(th.row, th.r, th.c)
                        return
                    }
                    if (bt) {
                        // Header sort zone (a header cell's right edge) beats the caret.
                        if (root.headerSortHit(bt, th.row, th.r, th.c, lp.x)) { root.headerSort(th.row, th.c); return }
                        root.beginTableInteraction(bt, th.row, lp.x, lp.y, m.modifiers)
                    }
                    return
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
                {   // SR-4 §4.14: a typed table cell — a click opens a choice cell's picker; a click on a
                    // check cell's box cycles it. The caret parks at the cell's start.
                    const gh = blockModel.tableHeadOf(h.row)
                    const gc = gh >= 0 && !blockModel.isHeaderRow(h.row) ? blockModel.gridColumnOf(h.row) : -1
                    const gk = gc >= 0 ? blockModel.gridColumnKind(gh, gc) : 0
                    if (gk === 1) {
                        cursor.setCaret(h.row, 0)
                        root.openGridChoicePicker(gh, blockModel.gridRowOf(h.row), gc, "")
                        return
                    }
                    const gcell = gk === 2 ? root.cellForRow(h.row) : null
                    if (gcell && m.x >= gcell.colLeft - 2 && m.x <= gcell.colLeft + 18) {
                        cursor.setCaret(h.row, 0)
                        blockModel.gridCycleCellCheck(gh, blockModel.gridRowOf(h.row), gc)
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
                if (root.dividerDragging) { root.updateDividerDrag(m.x - root.leftEdge); return }
                if (root.pulling) { root.updatePull(m.x - root.leftEdge); return }
                if (root.blockDragging) {
                    root.blockDragViewY = m.y - flick.contentY
                    root.blockDragX = m.x
                    root.aimBlockDrag(m.x, m.y)
                    return
                }
                if (root.gripDragging) {
                    if (Math.abs(m.x - root.gripPressX) + Math.abs(m.y - root.gripPressY) > 4)
                        root.gripMoved = true                    // click ≠ drag (the grip threshold)
                    var gd = root.cellForRow(root.gripTableRow), gbt = gd ? gd.tableItem : null
                    if (gbt) {
                        var glp = gbt.mapFromItem(mouse, m.x, m.y)
                        root.gripDropGap = root.gripDragKind === "col" ? gbt.colGapAt(glp.x)
                                                                       : gbt.rowGapAt(glp.y)
                    }
                    return
                }
                if (root.tableResizing || root.tableDragging) {
                    var ddcell = root.cellForRow(root.tableResizing ? root.resizeRow : cursor.focusRow)
                    var dbt = ddcell ? ddcell.tableItem : null
                    if (dbt) { var dlp = dbt.mapFromItem(mouse, m.x, m.y); root.updateTableInteraction(dbt, dlp.x, dlp.y) }
                    return
                }
                if (root.dragging) {
                    root.dragX = m.x; root.dragViewY = m.y - flick.contentY
                    var h = root.hitTest(m.x, m.y)
                    cursor.move(h.row, h.col, true)
                    return
                }
                // hover (not pressed): grip band → grip affordance; else near a
                // table column border → resize cursor (now y-guarded: the old
                // check showed a stray SplitHCursor in the margin bands).
                root.hoverRow = blockModel.blockAt(m.x - root.leftEdge, m.y)
                var overBorder = false
                var ghit = null
                if (blockModel.typeForRow(root.hoverRow) === 7) {
                    var hd = root.cellForRow(root.hoverRow), hbt = hd ? hd.tableItem : null
                    if (hbt) {
                        var hlp = hbt.mapFromItem(mouse, m.x, m.y)
                        ghit = root.tableGripAt(hbt, hlp.x, hlp.y)
                        overBorder = !ghit && hlp.y >= 0 && hlp.y <= hbt.height
                                     && hbt.columnBorderAt(hlp.x) >= 0
                    }
                }
                root.gripKind = ghit ? ghit.kind : ""
                root.gripIndex = ghit ? ghit.index : -1
                if (!root.gripDragging) root.gripTableRow = ghit ? root.hoverRow : -1
                root.tableOverBorder = overBorder
                // Over an interactive widget (block task checkbox, an inline
                // choice chip, or a table check/choice body cell) → a
                // pointing-hand cursor instead of the I-beam.
                var clk = root.taskCheckboxAt(m.x, m.y) >= 0
                root.codeChipHoverRow = root.codeLangChipAt(m.x, m.y)
                clk = clk || root.codeChipHoverRow >= 0
                if (!clk && !overBorder && blockModel.typeForRow(root.hoverRow) !== 7) {
                    var chh = root.hitTest(m.x, m.y)
                    clk = blockModel.choiceAt(chh.row, chh.col) !== ""
                }
                if (!clk && !overBorder && blockModel.typeForRow(root.hoverRow) === 7) {
                    var ch = root.tableHitAt(m.x, m.y)
                    if (ch) {
                        if (ch.r >= blockModel.tableHeaderRows(ch.row)) {
                            var ck = blockModel.tableColumnKind(ch.row, ch.c)
                            if (ck === 1 || ck === 2) {   // only over the chip/checkbox
                                var hd2 = root.cellForRow(ch.row), hbt2 = hd2 ? hd2.tableItem : null
                                var hlp2 = hbt2 ? hbt2.mapFromItem(mouse, m.x, m.y) : null
                                clk = hbt2 ? hbt2.widgetHit(hlp2.x, hlp2.y) : false
                            } else {   // plain text cell: over an inline chip?
                                var hd3 = root.cellForRow(ch.row), hbt3 = hd3 ? hd3.tableItem : null
                                if (hbt3) {
                                    var hlp3 = hbt3.mapFromItem(mouse, m.x, m.y)
                                    var chit = hbt3.cellAtPoint(hlp3.x, hlp3.y)
                                    clk = blockModel.tableChoiceAt(ch.row, chit.r, chit.c, chit.pos) !== ""
                                }
                            }
                        } else {   // header: pointer over the sort zone
                            var chd = root.cellForRow(ch.row), cbt = chd ? chd.tableItem : null
                            if (cbt) {
                                var clp = cbt.mapFromItem(mouse, m.x, m.y)
                                clk = root.headerSortHit(cbt, ch.row, ch.r, ch.c, clp.x)
                            }
                        }
                    }
                }
                mouse.overClickable = clk
                // Lane gestures (SR-3 S7b): a lane gap → drag its divider; the hot band just
                // inside a block's column edge → pull out a lane. Clickables and borders win.
                const dv = (clk || overBorder) ? null : root.dividerAt(root.hoverRow, m.x - root.leftEdge)
                root.dividerHoverRecord = dv ? dv.record : -1
                root.dividerHoverIndex = dv ? dv.index : -1
                const ps = (clk || overBorder || dv) ? -1 : root.pullSideAt(root.hoverRow, m.x)
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
                var lurl = ""
                if (!overBorder && blockModel.typeForRow(root.hoverRow) !== 7) {
                    var lh = root.hitTest(m.x, m.y)
                    lurl = blockModel.linkAt(lh.row, lh.col)
                }
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
            onExited: { root.hoverRow = -1; root.tableOverBorder = false
                        root.dividerHoverRecord = -1; root.dividerHoverIndex = -1; root.pullHoverRow = -1
                        root.gripKind = ""; root.gripIndex = -1
                        root.codeChipHoverRow = -1
                        if (!root.gripDragging) root.gripTableRow = -1
                        if (root.hoverLinkUrl.length > 0) linkTipHide.restart() }
            onReleased: {
                if (root.dividerDragging) root.commitDividerDrag()
                else if (root.pulling) root.commitPull()
                else if (root.blockDragging) root.commitBlockDrag()
                else if (root.gripDragging) root.commitInlineGripDrag()
                else if (root.tableResizing || root.tableDragging) root.endTableInteraction()
                else root.dragging = false
            }
            onCanceled: {
                root.cancelDividerDrag(); root.cancelPull()
                if (root.blockDragging) { root.blockDragging = false; root.blockDragRow = -1; root.dropGap = -1; root.blockDragCount = 1 }
                else {
                    root.dragging = false; root.tableDragging = false; root.tableResizing = false
                    root.gripDragging = false; root.gripDragKind = ""; root.gripFrom = -1
                    root.gripDropGap = -1; root.gripKind = ""; root.gripIndex = -1; root.gripTableRow = -1
                }
            }
            onDoubleClicked: (m) => {
                // End the press-drag the 2nd press armed, so a tiny mouse jitter
                // before release can't re-extend the selection back to the click
                // point (which collapsed the word to word-start→cursor).
                root.dragging = false; root.tableResizing = false
                // Double-click a file-attachment chip → reveal it in Finder/Explorer.
                var mrow = blockModel.blockAt(m.x - root.leftEdge, m.y)
                if (blockModel.typeForRow(mrow) === 3 && blockModel.mediaKind(mrow) === "file") {
                    blockModel.revealMedia(mrow); return
                }
                // Double-click a table column border → reset that column to auto;
                // otherwise word-select INSIDE the cell (multi-select pass
                // 2026-08-21 — cells now speak the same double-click language).
                var drow = blockModel.blockAt(m.x - root.leftEdge, m.y)
                if (blockModel.typeForRow(drow) === 7) {
                    var dd = root.cellForRow(drow), dbt = dd ? dd.tableItem : null
                    if (dbt) {
                        var dlp = dbt.mapFromItem(mouse, m.x, m.y)
                        var dbc = dbt.columnBorderAt(dlp.x)
                        if (dbc >= 0) { blockModel.tableSetColWidth(drow, dbc, 0); return }
                        if (dlp.x >= 0 && dlp.y >= 0 && dlp.y <= dbt.height)
                            root.tableWordSelect(dbt, drow, dlp.x, dlp.y)
                    }
                    return
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

    // --- Full-frame table view (the active table tab). Fills the editor; vertical
    // scroll for tall tables, the BlockTable handles horizontal internally. Reuses
    // the same tcur edit model (the cursor is pinned to this table row). ---
    Flickable {
        id: tableFrame
        visible: root.activeTableRow >= 0 && !root.boardMode
        anchors.fill: parent
        anchors.topMargin: Theme.dim.toolStripHeight   // room for the tab toolbar
        // The PAGE scrolls horizontally for wide tables (the table itself is
        // uncapped) — same model as the notes view; one scrollbar, no nested one.
        contentWidth: Math.max(width, 20 + frameTable.width + 4 + 14 + 20)
        // Room below the table: 4px gap + the +row strip + a 20px bottom margin.
        contentHeight: frameTable.implicitHeight + 58
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        // Row/column drag-reorder (full-frame only; the doc view reorders via the
        // context menu). Grip strips sit above (columns) / left of (rows) the
        // table; dragging a grip shows the existing hiScope highlight on the
        // source and an accent insertion line at the target gap; release commits
        // ONE undoable tableMoveRow/Column. Header rows are not draggable and
        // body rows can't drop above the header boundary.
        property int  gripC: -1            // hovered column (top strip)
        property int  gripR: -1            // hovered body row (left strip)
        property bool colDragging: false
        property bool rowDragging: false
        property int  dragFrom: -1
        property int  dropGap: -1          // target insertion gap (cols: 0..n, rows: header..n)
        // A grip press that never moves is a CLICK → select the whole row/column.
        property real gripPressX: 0
        property real gripPressY: 0
        property bool gripMoved: false
        // Modifiers captured at PRESS (users release Shift before the button).
        property int  gripPressMods: 0
        readonly property int frameCols: root.activeTableRow >= 0
            ? (blockModel.contentRevision, blockModel.tableColumns(root.activeTableRow)) : 0
        readonly property int frameRows: root.activeTableRow >= 0
            ? (blockModel.contentRevision, blockModel.tableRows(root.activeTableRow)) : 0
        readonly property int frameHdr: root.activeTableRow >= 0
            ? (blockModel.contentRevision, blockModel.tableHeaderRows(root.activeTableRow)) : 0
        function colAt(px) {               // strip x → column index (−1 outside)
            var cx = px + frameTable.scrollX, acc = 0
            for (var c = 0; c < frameCols; ++c) { acc += frameTable.colW(c); if (cx < acc) return c }
            return -1
        }
        function colGapAt(px) {            // strip x → insertion gap 0..cols
            var cx = px + frameTable.scrollX, acc = 0
            for (var c = 0; c < frameCols; ++c) {
                var w = frameTable.colW(c)
                if (cx < acc + w / 2) return c
                acc += w
            }
            return frameCols
        }
        function rowAt(py) {               // strip y → BODY row index (−1 header/outside)
            var acc = 0
            for (var r = 0; r < frameRows; ++r) {
                var h = frameTable.rowHeightAt(r)
                if (py < acc + h) return r >= frameHdr ? r : -1
                acc += h
            }
            return -1
        }
        function rowGapAt(py) {            // strip y → insertion gap header..rows
            var acc = frameTable.rowTopY(frameHdr)
            if (py < acc) return frameHdr
            for (var r = frameHdr; r < frameRows; ++r) {
                var h = frameTable.rowHeightAt(r)
                if (py < acc + h / 2) return r
                acc += h
            }
            return frameRows
        }
        function commitGripDrag() {
            if (!gripMoved && dragFrom >= 0) {           // click, not drag → select
                if (colDragging) root.gripSelectCol(root.activeTableRow, dragFrom, gripPressMods)
                else if (rowDragging) root.gripSelectRow(root.activeTableRow, dragFrom, gripPressMods)
            } else if (dropGap >= 0 && dragFrom >= 0) {
                var to = dropGap > dragFrom ? dropGap - 1 : dropGap
                if (to !== dragFrom) {
                    if (colDragging) blockModel.tableMoveColumn(root.activeTableRow, dragFrom, to)
                    else if (rowDragging) blockModel.tableMoveRow(root.activeTableRow, dragFrom, to)
                }
            }
            cancelGripDrag()
        }
        function cancelGripDrag() {
            colDragging = false; rowDragging = false
            dragFrom = -1; dropGap = -1; gripC = -1; gripR = -1; gripPressMods = 0
        }
        ScrollBar.vertical: MnScrollBar {}
        ScrollBar.horizontal: MnScrollBar {}
        BlockTable {
            id: frameTable
            active: root.activeTableRow >= 0
            logicalRow: root.activeTableRow
            x: 20; y: 20
            uncapped: true                         // natural width; the tab PAGE scrolls
            maxWidth: tableFrame.width - 58        // (unused while uncapped)
            width: implicitWidth
            height: implicitHeight
            focused: root.activeTableRow >= 0
            caretOn: root.caretOn
            focusR: tcur.cr; focusC: tcur.cc
            caretPos: tcur.pos
            selFrom: Math.min(tcur.pos, tcur.anchorPos)
            selTo: Math.max(tcur.pos, tcur.anchorPos)
            // Gated on the frame actually holding the sub-cursor — the
            // pre-existing ungated binding showed a STALE selection when the
            // document caret had moved off this table (the inline instance
            // gates on `focused`; inRange/inSel short-circuit on rangeR0/-sets).
            rangeR0: cursor.focusRow === root.activeTableRow ? tcur.rangeR0 : -1
            rangeC0: tcur.rangeC0; rangeR1: tcur.rangeR1; rangeC1: tcur.rangeC1
            selRows: cursor.focusRow === root.activeTableRow ? tcur.selRows : []
            selCols: cursor.focusRow === root.activeTableRow ? tcur.selCols : []
            selRev: tcur.selRev
            resizeCol: (root.tableResizing && root.resizeRow === root.activeTableRow) ? root.resizeColIdx : -1
            resizeW: root.resizeW
            sortCol: root.lastSortRow === root.activeTableRow ? root.lastSortCol : -1
            sortAsc: root.lastSortAsc
            // Context-menu row/column target highlight (same as the doc view);
            // a grip drag reuses it to mark the dragged column/row.
            hiScope: tableFrame.colDragging ? "column" : tableFrame.rowDragging ? "row"
                   : (root.menuRow === root.activeTableRow
                      && (root.menuHiScope === "column" || root.menuHiScope === "row")) ? root.menuHiScope : ""
            hiIndex: (tableFrame.colDragging || tableFrame.rowDragging) ? tableFrame.dragFrom
                   : root.menuHiScope === "column" ? root.menuCellC : root.menuCellR
            hiDanger: (tableFrame.colDragging || tableFrame.rowDragging) ? false : root.menuHiDanger
        }

        // Full-frame mouse handling — this view is a dedicated mode (no document
        // mouse layer above it), so a direct MouseArea works. Coords are already
        // frameTable-local. Reuses the same begin/update/end helpers.
        MouseArea {
            id: frameMA
            anchors.fill: frameTable
            hoverEnabled: true; preventStealing: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            property bool overClickable: false   // over a check / choice body cell
            cursorShape: (root.tableResizing || root.tableOverBorder) ? Qt.SplitHCursor
                       : overClickable ? Qt.PointingHandCursor : Qt.IBeamCursor
            onPressed: (m) => {
                root.forceActiveFocus()
                // Right-click → the same table context menu as the document view
                // (capture the cell for the row/column ops; coords mapped to root).
                if (m.button === Qt.RightButton) {
                    var hit = frameTable.cellAtPoint(m.x, m.y)
                    root.menuCellR = hit.r; root.menuCellC = hit.c
                    root.menuLinkUrl = ""
                    var fci = spell.cellIssueAt(root.activeTableRow, hit.r, hit.c, hit.pos)
                    root.menuIssue = fci.ruleId !== undefined ? fci : null; root.menuIssueInCell = true
                    var p = frameMA.mapToItem(root, m.x, m.y)
                    root.openBlockMenu(p.x, p.y, root.activeTableRow)
                    return
                }
                // Typed body cells behave the same as the inline view: choice → the
                // option picker; check → cycle the tri-state. Both go through the same
                // mutateTable-backed invokables, so undo is identical here.
                var fhit = frameTable.cellAtPoint(m.x, m.y)
                // Header sort zone beats the caret (m is already frameTable-local).
                if (root.headerSortHit(frameTable, root.activeTableRow, fhit.r, fhit.c, m.x)) {
                    root.headerSort(root.activeTableRow, fhit.c); return
                }
                var ftk = blockModel.tableColumnKind(root.activeTableRow, fhit.c)
                var fbody = fhit.r >= blockModel.tableHeaderRows(root.activeTableRow)
                // Trigger only when the click lands ON the chip/checkbox.
                if ((ftk === 1 || ftk === 2) && fbody && frameTable.widgetHit(m.x, m.y)) {
                    if (ftk === 1) {
                        var fp = frameMA.mapToItem(root, m.x, m.y)
                        root.openChoicePicker(root.activeTableRow, fhit.r, fhit.c, fp.x, fp.y)
                    } else {
                        blockModel.tableCycleCellCheck(root.activeTableRow, fhit.r, fhit.c)
                    }
                    return
                }
                root.beginTableInteraction(frameTable, root.activeTableRow, m.x, m.y, m.modifiers)
            }
            onPositionChanged: (m) => {
                if (root.tableResizing || root.tableDragging) { root.updateTableInteraction(frameTable, m.x, m.y); return }
                var overBorder = frameTable.columnBorderAt(m.x) >= 0
                root.tableOverBorder = overBorder
                // Pointer cursor over a check / choice body cell or a header's
                // sort slot.
                var clk = false
                if (!overBorder) {
                    var fh = frameTable.cellAtPoint(m.x, m.y)
                    if (fh && fh.r >= blockModel.tableHeaderRows(root.activeTableRow)) {
                        var fk = blockModel.tableColumnKind(root.activeTableRow, fh.c)
                        clk = (fk === 1 || fk === 2) && frameTable.widgetHit(m.x, m.y)
                        if (!clk && fk === 0)   // inline chip in a plain text cell
                            clk = blockModel.tableChoiceAt(root.activeTableRow, fh.r, fh.c, fh.pos) !== ""
                    } else if (fh) {
                        clk = root.headerSortHit(frameTable, root.activeTableRow, fh.r, fh.c, m.x)
                    }
                }
                frameMA.overClickable = clk
            }
            onExited: { root.tableOverBorder = false; frameMA.overClickable = false }
            onReleased: root.endTableInteraction()
            onCanceled: { root.tableResizing = false; root.tableDragging = false }
            onDoubleClicked: (m) => {
                var bc = frameTable.columnBorderAt(m.x)
                if (bc >= 0) blockModel.tableSetColWidth(root.activeTableRow, bc, 0)
            }
        }

        DropArea {   // image file → the cell under the pointer. The document
                     // view's DropArea doesn't cover this dedicated mode.
            anchors.fill: parent
            onPositionChanged: (drag) => {
                var lp = frameTable.mapFromItem(tableFrame, drag.x, drag.y)
                if (drag.hasUrls && lp.x >= 0 && lp.x <= frameTable.width
                                 && lp.y >= 0 && lp.y <= frameTable.height) {
                    var h = frameTable.cellAtPoint(lp.x, lp.y)
                    frameTable.dropR = h.r; frameTable.dropC = h.c
                } else { frameTable.dropR = -1; frameTable.dropC = -1 }
            }
            onExited: { frameTable.dropR = -1; frameTable.dropC = -1 }
            onDropped: (drop) => {
                var r = frameTable.dropR, c = frameTable.dropC
                frameTable.dropR = -1; frameTable.dropC = -1
                if (r < 0 || !drop.hasUrls) return
                for (var i = 0; i < drop.urls.length; ++i)
                    if (blockModel.tableSetCellImageFromUrl(root.activeTableRow, r, c, drop.urls[i].toString())) {
                        tcur.place(r, c, 0)
                        drop.accept()
                        return
                    }
            }
        }

        // Full-frame affordances: +column (right), horizontal scrollbar + +row
        // (bottom). Direct children — no mouse-layer conflict in this view.
        Rectangle {   // + column
            x: 20 + frameTable.width + 4; y: 20
            width: 14; height: frameTable.height; radius: 0
            color: fAddColMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.surfaceHover
            border.width: 1; border.color: Theme.colors.border
            Text { anchors.centerIn: parent; text: "+"; color: Theme.colors.textMuted; font.pixelSize: Theme.font.sizeChrome }
            MouseArea { id: fAddColMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: blockModel.tableInsertColumn(root.activeTableRow, blockModel.tableColumns(root.activeTableRow)) }
        }
        Rectangle {   // + row
            x: 20; y: 20 + frameTable.height + 4
            width: frameTable.width; height: 14; radius: 0
            color: fAddRowMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.surfaceHover
            border.width: 1; border.color: Theme.colors.border
            Text { anchors.centerIn: parent; text: "+"; color: Theme.colors.textMuted; font.pixelSize: Theme.font.sizeChrome }
            MouseArea { id: fAddRowMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: blockModel.tableInsertRow(root.activeTableRow, blockModel.tableRows(root.activeTableRow)) }
        }

        // --- Row/column reorder grips (see the property block above) ---
        Item {   // column grips: a thin strip across the table's top edge
            x: 20; y: 4; width: frameTable.width; height: 14
            Rectangle {   // grip pill over the hovered / dragged column
                visible: colGripMA.gcol >= 0
                x: Math.max(0, frameTable.columnLeftX(colGripMA.gcol) - frameTable.scrollX)
                width: Math.max(0, Math.min(frameTable.colW(colGripMA.gcol),
                                            parent.width - x))
                height: 8; y: 3; radius: 0
                color: tableFrame.colDragging ? Theme.colors.accentMuted : Theme.colors.surfaceHover
                border.width: 1; border.color: tableFrame.colDragging ? Theme.colors.accent : Theme.colors.border
            }
            MouseArea {
                id: colGripMA
                anchors.fill: parent
                hoverEnabled: true; preventStealing: true
                readonly property int gcol: tableFrame.colDragging ? tableFrame.dragFrom : tableFrame.gripC
                cursorShape: tableFrame.colDragging ? Qt.ClosedHandCursor
                           : tableFrame.gripC >= 0 ? Qt.OpenHandCursor : Qt.ArrowCursor
                onPositionChanged: (m) => {
                    if (tableFrame.colDragging) {
                        if (Math.abs(m.x - tableFrame.gripPressX) + Math.abs(m.y - tableFrame.gripPressY) > 4)
                            tableFrame.gripMoved = true
                        tableFrame.dropGap = tableFrame.colGapAt(m.x); return
                    }
                    tableFrame.gripC = tableFrame.colAt(m.x)
                }
                onPressed: (m) => {
                    var c = tableFrame.colAt(m.x)
                    if (c >= 0) { tableFrame.colDragging = true; tableFrame.dragFrom = c
                                  tableFrame.gripPressX = m.x; tableFrame.gripPressY = m.y
                                  tableFrame.gripMoved = false
                                  tableFrame.gripPressMods = m.modifiers
                                  tableFrame.dropGap = tableFrame.colGapAt(m.x) }
                }
                onReleased: tableFrame.commitGripDrag()
                onCanceled: tableFrame.cancelGripDrag()
                onExited: if (!tableFrame.colDragging) tableFrame.gripC = -1
            }
        }
        Item {   // row grips: a thin strip down the table's left edge (body rows)
            x: 4; y: 20; width: 14; height: frameTable.height
            Rectangle {   // grip pill beside the hovered / dragged row
                visible: rowGripMA.gr >= 0
                y: frameTable.rowTopY(rowGripMA.gr)
                height: Math.max(0, Math.min(frameTable.rowHeightAt(rowGripMA.gr),
                                             parent.height - y))
                width: 8; x: 3; radius: 0
                color: tableFrame.rowDragging ? Theme.colors.accentMuted : Theme.colors.surfaceHover
                border.width: 1; border.color: tableFrame.rowDragging ? Theme.colors.accent : Theme.colors.border
            }
            MouseArea {
                id: rowGripMA
                anchors.fill: parent
                hoverEnabled: true; preventStealing: true
                readonly property int gr: tableFrame.rowDragging ? tableFrame.dragFrom : tableFrame.gripR
                cursorShape: tableFrame.rowDragging ? Qt.ClosedHandCursor
                           : tableFrame.gripR >= 0 ? Qt.OpenHandCursor : Qt.ArrowCursor
                onPositionChanged: (m) => {
                    if (tableFrame.rowDragging) {
                        if (Math.abs(m.x - tableFrame.gripPressX) + Math.abs(m.y - tableFrame.gripPressY) > 4)
                            tableFrame.gripMoved = true
                        tableFrame.dropGap = tableFrame.rowGapAt(m.y); return
                    }
                    tableFrame.gripR = tableFrame.rowAt(m.y)
                }
                onPressed: (m) => {
                    var r = tableFrame.rowAt(m.y)
                    if (r >= 0) { tableFrame.rowDragging = true; tableFrame.dragFrom = r
                                  tableFrame.gripPressX = m.x; tableFrame.gripPressY = m.y
                                  tableFrame.gripMoved = false
                                  tableFrame.gripPressMods = m.modifiers
                                  tableFrame.dropGap = tableFrame.rowGapAt(m.y) }
                }
                onReleased: tableFrame.commitGripDrag()
                onCanceled: tableFrame.cancelGripDrag()
                onExited: if (!tableFrame.rowDragging) tableFrame.gripR = -1
            }
        }
        Rectangle {   // column drop line (insertion gap during a grip drag)
            visible: tableFrame.colDragging && tableFrame.dropGap >= 0
            x: 20 + Math.max(0, Math.min(frameTable.width,
                   frameTable.columnLeftX(tableFrame.dropGap) - frameTable.scrollX)) - 1
            y: 20; width: 3; height: frameTable.height
            radius: 0; color: Theme.colors.accent; z: 10
        }
        Rectangle {   // row drop line
            visible: tableFrame.rowDragging && tableFrame.dropGap >= 0
            x: 20; y: 20 + frameTable.rowTopY(tableFrame.dropGap) - 1
            width: frameTable.width; height: 3
            radius: 0; color: Theme.colors.accent; z: 10
        }
    }
    Rectangle {   // bottom clip cue: more table below the viewport — mirrors the
                  // table's right-edge cue; hidden once scrolled to the end.
        visible: tableFrame.visible && tableFrame.contentHeight > tableFrame.height
                 && tableFrame.contentY < tableFrame.contentHeight - tableFrame.height - 0.5
        x: 20; width: frameTable.width; height: 2
        anchors.bottom: tableFrame.bottom
        color: Theme.colors.border
        z: 15
    }

    // --- Full-frame kanban board (the active table tab in board mode). Scrolls
    // both ways; the board view owns all card interaction directly (a dedicated
    // mode like the table frame — no document mouse layer above it). ---
    Flickable {
        id: boardFrame
        visible: root.activeTableRow >= 0 && root.boardMode
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
            logicalRow: root.activeTableRow
            groupCol: root.boardCol
            onShowGrid: root.showGridView()      // grouping column vanished → grid
            onEditClosed: root.forceActiveFocus()
            onOpenCard: (r, c) => {              // double-click → grid, cell focused
                root.showGridView()
                tcur.place(r, c, 0)
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
            && blockModel.tableColumnKind(root.activeTableRow, root.boardCol) === 1
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
                          onActivated: blockModel.tableMoveOption(root.activeTableRow, root.boardCol,
                                                                  laneMenu.lane.key, laneMenu.li - 1) }
            LaneMenuRow { visible: laneMenu.optionLane && laneMenu.li < boardView.lanes.length - 2
                          text: "Move lane right"
                          onActivated: blockModel.tableMoveOption(root.activeTableRow, root.boardCol,
                                                                  laneMenu.lane.key, laneMenu.li + 1) }
            LaneMenuRow { visible: laneMenu.lane !== null && root.boardCol >= 0
                                   && blockModel.tableColumnKind(root.activeTableRow, root.boardCol) === 1
                          text: "Edit options…"
                          onActivated: root.openChoiceEditor(root.activeTableRow, root.boardCol) }
        }
    }
    Rectangle {   // table-tab toolbar: the family flat-button strip above the frame
        id: tableTabBar
        visible: root.activeTableRow >= 0
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
                onClicked: { root.showGridView(); root.forceActiveFocus() }
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
                        root.openBoard(root.activeTableRow,
                                       root.boardCol >= 0 ? root.boardCol : root.firstGroupCol)
                    root.forceActiveFocus()
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
        Repeater {
            model: root.poolSize
            delegate: Item {
                id: rnum
                required property int index
                readonly property int prow: (root.slotRev, viewSlots.rowForSlot(index))
                visible: prow >= 0 && prow < blockModel.count
                         && (blockModel.contentRevision, blockModel.laneForRow(prow)) < 0   // top entries only
                width: blockRuler.width
                height: Math.max(16, (blockModel.layoutRevision, blockModel.heightForRow(prow)))
                y: (blockModel.layoutRevision, blockModel.yForRow(prow)) - flick.contentY
                // Being dragged → the rail chip is the block's body; its slot dims.
                opacity: root.blockDragging && rnum.prow >= root.blockDragRow
                         && rnum.prow < root.blockDragRow + root.blockDragCount ? 0.3 : 1
                Text {
                    y: 2
                    width: parent.width - 8
                    horizontalAlignment: Text.AlignRight
                    text: rnum.prow + 1
                    color: rnum.prow === cursor.focusRow ? Theme.colors.textMuted
                                                         : Theme.colors.textSubtle
                    font.family: Theme.font.mono; font.pixelSize: 11
                }
                MouseArea {   // the number is the block's HANDLE — drag to
                              // reorder (the left gutter's twin; reuses the
                              // whole blockDrag lifecycle incl. auto-scroll)
                    anchors.fill: parent
                    hoverEnabled: true
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
                                root.blockDragRow = rnum.prow
                                // A split row's number carries the whole split row.
                                root.blockDragCount = blockModel.typeForRow(rnum.prow) === 10
                                    ? blockModel.splitRowLast(rnum.prow) - rnum.prow + 1 : 1
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
                text: root.blockDragCount > 1 ? root.blockDragCount + " blocks" : root.blockDragRow + 1
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
        x: root.leftEdge + root.dividerPreviewX - 1 - flick.contentX
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
            visible: root.activeTableRow < 0 && root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0
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
            visible: root.activeTableRow < 0 && root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0
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

    // Image resize affordances — root overlays (above the central mouse layer) at
    // the hovered/resizing image's corners: top-right fit-to-width, bottom-right
    // proportional drag. Images only (kind "image"); Document view only.
    Item {
        id: imgResize
        // Show handles while resizing, while hovering the image, OR while the image
        // is the selected block — so a click (which selects it) can't make them
        // vanish, and missing the small handle just selects + keeps them up.
        readonly property int row: root.imageResizing ? root.imageResizeRow
            : (root.imgHandleRow >= 0 ? root.imgHandleRow
               : (root._isResizableMediaRow(cursor.focusRow) ? cursor.focusRow : -1))
        // Also hidden in ink mode: a still-selected image's handles would sit
        // above the canvas and let the pen RESIZE the layout under the ink.
        visible: row >= 0 && !root.inkMode
                 && root.activeTableRow < 0 && root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0
        readonly property real imgX: (row >= 0 ? root.columnX(row) : root.leftEdge) - flick.contentX
        readonly property real imgTopV: row >= 0
            ? (blockModel.layoutRevision, blockModel.yForRow(row)) + 6 - flick.contentY : 0
        readonly property real imgW: row >= 0
            ? (blockModel.layoutRevision, blockModel.mediaDispWidth(row)) : 0
        readonly property real imgH: row >= 0
            ? (blockModel.layoutRevision, blockModel.mediaDisplayHeight(row)) : 0
        z: 57

        Rectangle {   // fit-to-width (top-right)
            id: fitBtn
            visible: !root.imageResizing
            width: 24; height: 24; radius: 0
            x: imgResize.imgX + imgResize.imgW - width - 6
            y: imgResize.imgTopV + 6
            color: fitMA.containsMouse ? Theme.colors.accent : Qt.rgba(0, 0, 0, 0.55)
            border.width: 1; border.color: Theme.colors.border
            Icon { anchors.centerIn: parent; name: "frame-corners"; size: 14; color: Theme.colors.textBright }
            MouseArea {
                id: fitMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: blockModel.setMediaWidth(imgResize.row, Math.round(root.measureForRow(imgResize.row)))   // fit the lane (the page at top level)
            }
        }

        Rectangle {   // proportional drag handle (bottom-right)
            id: dragHandle
            width: 22; height: 22; radius: 0
            x: imgResize.imgX + imgResize.imgW - width - 6
            y: imgResize.imgTopV + imgResize.imgH - height - 6
            color: (dragMA.containsMouse || root.imageResizing) ? Theme.colors.accent : Qt.rgba(0, 0, 0, 0.55)
            border.width: 1; border.color: Theme.colors.border
            Icon { anchors.centerIn: parent; name: "resize"; size: 14; color: Theme.colors.textBright }
            MouseArea {
                id: dragMA
                anchors.fill: parent; hoverEnabled: true; preventStealing: true
                cursorShape: Qt.SizeFDiagCursor
                onPressed: (m) => {
                    // Capture the target row + start geometry BEFORE flipping
                    // imageResizing — imgResize.row depends on it, so setting it
                    // first would re-evaluate row to the default (-1).
                    root.imageResizeRow = imgResize.row
                    root._imgResizePressX = m.x
                    root._imgResizeStartW = imgResize.imgW
                    root.imageResizeW = imgResize.imgW
                    root.imageResizeAspect = imgResize.imgW > 0 ? imgResize.imgH / imgResize.imgW : 1
                    root.imageResizing = true
                }
                onPositionChanged: (m) => {
                    if (!root.imageResizing) return
                    // No page cap (user ruling): the drag can take an image
                    // past the 760 measure — the reachable screen is the
                    // practical limit, and the page h-scroll holds the rest.
                    root.imageResizeW = Math.max(80,
                        root._imgResizeStartW + (m.x - root._imgResizePressX))
                }
                onReleased: {
                    if (root.imageResizing) {
                        blockModel.setMediaWidth(root.imageResizeRow, Math.round(root.imageResizeW))
                        root.imageResizing = false; root.imageResizeRow = -1
                    }
                }
                onCanceled: { root.imageResizing = false; root.imageResizeRow = -1 }
                onDoubleClicked: blockModel.setMediaWidth(imgResize.row, 0)   // reset to intrinsic
            }
        }
    }

    // Resize ghost: a target-size outline that follows the drag WITHOUT reflowing
    // the document (committed on release) — so indecisive dragging never stutters.
    Rectangle {
        visible: root.imageResizing
        z: 58
        x: (root.imageResizeRow >= 0 ? root.columnX(root.imageResizeRow) : root.leftEdge) - flick.contentX
        y: (blockModel.layoutRevision, root.imageResizeRow >= 0
            ? blockModel.yForRow(root.imageResizeRow) : 0) + 6 - flick.contentY
        width: root.imageResizeW
        height: root.imageResizeW * root.imageResizeAspect
        color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.08)
        border.width: 2; border.color: Theme.colors.accent
        radius: Theme.dim.radius
        Rectangle {
            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6
            width: dimLabel.width + 10; height: dimLabel.height + 6; radius: 0
            color: Qt.rgba(0, 0, 0, 0.7)
            Text {
                id: dimLabel; anchors.centerIn: parent
                text: Math.round(root.imageResizeW) + " × " + Math.round(root.imageResizeW * root.imageResizeAspect)
                color: Theme.colors.textBright; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
            }
        }
    }

    // Cell-image resize handles — root overlays over the focused table cell's image
    // (fit-to-column top-right, proportional drag bottom-right), the same model as
    // the document image but bounded by the column width.
    Item {
        id: cellImgResize
        visible: root.cellImgActive && root.cellImgRect.width > 0
        readonly property real rx: root.cellImgRect.x
        readonly property real ry: root.cellImgRect.y
        readonly property real rw: root.cellImgRect.width
        readonly property real rh: root.cellImgRect.height
        z: 57

        Rectangle {   // fit-to-column (top-right)
            visible: !root.cellImgResizing
            width: 20; height: 20; radius: 0
            x: cellImgResize.rx + cellImgResize.rw - width - 4
            y: cellImgResize.ry + 4
            color: cFitMA.containsMouse ? Theme.colors.accent : Qt.rgba(0, 0, 0, 0.55)
            border.width: 1; border.color: Theme.colors.border
            Icon { anchors.centerIn: parent; name: "frame-corners"; size: 12; color: Theme.colors.textBright }
            MouseArea {
                id: cFitMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: blockModel.tableSetCellImageWidth(tcur.row, tcur.cr, tcur.cc, Math.round(root.cellImgMaxW))
            }
        }

        Rectangle {   // proportional drag (bottom-right)
            width: 18; height: 18; radius: 0
            x: cellImgResize.rx + cellImgResize.rw - width - 4
            y: cellImgResize.ry + cellImgResize.rh - height - 4
            color: (cDragMA.containsMouse || root.cellImgResizing) ? Theme.colors.accent : Qt.rgba(0, 0, 0, 0.55)
            border.width: 1; border.color: Theme.colors.border
            Icon { anchors.centerIn: parent; name: "resize"; size: 12; color: Theme.colors.textBright }
            MouseArea {
                id: cDragMA
                anchors.fill: parent; hoverEnabled: true; preventStealing: true
                cursorShape: Qt.SizeFDiagCursor
                onPressed: (m) => {
                    root._cellResizePressX = m.x
                    root._cellResizeStartW = cellImgResize.rw
                    root.cellResizeW = cellImgResize.rw
                    root.cellResizeAspect = cellImgResize.rw > 0 ? cellImgResize.rh / cellImgResize.rw : 1
                    root.cellImgResizing = true
                }
                onPositionChanged: (m) => {
                    if (!root.cellImgResizing) return
                    root.cellResizeW = Math.max(40, Math.min(root.cellImgMaxW,
                        root._cellResizeStartW + (m.x - root._cellResizePressX)))
                }
                onReleased: {
                    if (root.cellImgResizing) {
                        blockModel.tableSetCellImageWidth(tcur.row, tcur.cr, tcur.cc, Math.round(root.cellResizeW))
                        root.cellImgResizing = false
                    }
                }
                onCanceled: root.cellImgResizing = false
                onDoubleClicked: blockModel.tableSetCellImageWidth(tcur.row, tcur.cr, tcur.cc, 0)   // reset
            }
        }
    }

    // Cell-image resize ghost — target-size outline during the drag (no reflow until
    // release), anchored at the cell image's top-left.
    Rectangle {
        visible: root.cellImgResizing
        z: 58
        x: root.cellImgRect.x
        y: root.cellImgRect.y
        width: root.cellResizeW
        height: root.cellResizeW * root.cellResizeAspect
        color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.08)
        border.width: 2; border.color: Theme.colors.accent
        Rectangle {
            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4
            width: cDimLabel.width + 10; height: cDimLabel.height + 6; radius: 0
            color: Qt.rgba(0, 0, 0, 0.7)
            Text {
                id: cDimLabel; anchors.centerIn: parent
                text: Math.round(root.cellResizeW) + " × " + Math.round(root.cellResizeW * root.cellResizeAspect)
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
        visible: root.menuHiScope === "block" && root.menuRow >= 0 && root.activeTableRow < 0 && root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0
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

    // +row / +column affordances for the focused table. Root overlays (above the
    // document mouse layer) — a clickable strip inside the table couldn't receive
    // events, since that mouse layer stacks over every delegate.
    Item {
        id: tableAdd
        readonly property Item dlg: (blockModel.layoutRevision, blockModel.contentRevision, flick.contentY,
            tcur.active ? root.cellForRow(cursor.focusRow) : null)
        readonly property Item tItem: dlg ? dlg.tableItem : null
        visible: tItem !== null && root.activeTableRow < 0 && root.activePdfRow < 0 && root.activeVideoRow < 0 && root.activeSketchRow < 0   // Document view only
        readonly property real topV: dlg ? dlg.y - flick.contentY + 32 : 0   // table content top (tableHost y:32)
        readonly property real cw: tItem ? tItem.width : 0
        readonly property real ch: tItem ? tItem.height : 0
        readonly property real tableX: root.leftEdge - flick.contentX   // shared edge, in VIEWPORT coords (page can h-scroll)
        readonly property bool overflow: tItem ? tItem.overflowing : false
        readonly property real sbH: Theme.dim.scrollBarWidth

        Rectangle {   // horizontal scrollbar — a root overlay (an inner ScrollBar
                      // would sit under the document mouse layer) that drives the
                      // table's scrollX; sits between the table and the +row button.
            visible: tableAdd.overflow
            x: tableAdd.tableX; y: tableAdd.topV + tableAdd.ch + 4
            width: tableAdd.cw; height: tableAdd.sbH; z: 41; color: "transparent"
            readonly property real contentTW: tableAdd.tItem ? tableAdd.tItem.contentW : 0
            readonly property real maxScroll: Math.max(0, contentTW - tableAdd.cw)
            readonly property real thumbW: Math.max(24, width * tableAdd.cw / Math.max(1, contentTW))
            readonly property real maxThumbX: width - thumbW
            Rectangle {
                id: hthumb
                height: parent.height; radius: 0; width: parent.thumbW
                x: parent.maxScroll > 0 && tableAdd.tItem ? (tableAdd.tItem.scrollX / parent.maxScroll) * parent.maxThumbX : 0
                color: Theme.colors.textSubtle
                opacity: hbarMA.pressed ? 0.85 : (hbarMA.containsMouse ? 0.65 : 0.45)
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
            MouseArea {
                id: hbarMA
                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                function setScroll(mx) {
                    var tx = Math.max(0, Math.min(parent.maxThumbX, mx - parent.thumbW / 2))
                    if (parent.maxThumbX > 0 && tableAdd.tItem)
                        tableAdd.tItem.scrollX = (tx / parent.maxThumbX) * parent.maxScroll
                }
                onPressed: (m) => setScroll(m.x)
                onPositionChanged: (m) => { if (pressed) setScroll(m.x) }
            }
        }

        Rectangle {   // + row, bottom edge (below the scrollbar when present)
            x: tableAdd.tableX; y: tableAdd.topV + tableAdd.ch + (tableAdd.overflow ? tableAdd.sbH + 8 : 4)
            width: tableAdd.cw; height: 14; radius: 0; z: 40
            color: addRowMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.surfaceHover
            border.width: 1; border.color: Theme.colors.border
            Text { anchors.centerIn: parent; text: "+"; color: Theme.colors.textMuted; font.pixelSize: Theme.font.sizeChrome }
            MouseArea {
                id: addRowMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: blockModel.tableInsertRow(cursor.focusRow, blockModel.tableRows(cursor.focusRow))
            }
        }
        Rectangle {   // + column, right edge
            x: tableAdd.tableX + tableAdd.cw + 4; y: tableAdd.topV
            width: 14; height: tableAdd.ch; radius: 0; z: 40
            color: addColMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.surfaceHover
            border.width: 1; border.color: Theme.colors.border
            Text { anchors.centerIn: parent; text: "+"; color: Theme.colors.textMuted; font.pixelSize: Theme.font.sizeChrome }
            MouseArea {
                id: addColMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: blockModel.tableInsertColumn(cursor.focusRow, blockModel.tableColumns(cursor.focusRow))
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

    // --- Choice-cell option picker (root overlay above the mouse layer) ---
    function openChoicePicker(trow, r, c, vx, vy) {
        choicePicker.gridHead = -1
        choicePicker.srow = -1; choicePicker.sstart = -1
        choicePicker.sr = -1; choicePicker.sc = -1
        choicePicker.row = trow; choicePicker.r = r; choicePicker.c = c
        root.choiceX = vx; root.choiceY = vy
        choicePicker.open()
    }
    // Inline chip variant (DT-2): span address instead of the cell triple.
    function openInlineChoicePicker(brow, s, vx, vy) {
        choicePicker.gridHead = -1
        choicePicker.row = -1; choicePicker.r = -1; choicePicker.c = -1
        choicePicker.sr = -1; choicePicker.sc = -1
        choicePicker.srow = brow; choicePicker.sstart = s
        root.choiceX = vx; root.choiceY = vy
        choicePicker.open()
    }
    // Cell-chip variant (2026-08-21): a chip span INSIDE a table text cell —
    // span address plus the cell coords.
    // A derived table's choice cell (SR-4 S6b): the picker under the cell, the add field
    // prefilled with `text` (the typed character that opened it, or "").
    function openGridChoicePicker(head, r, c, text) {
        choicePicker.row = -1; choicePicker.r = -1; choicePicker.c = -1
        choicePicker.srow = -1; choicePicker.sstart = -1; choicePicker.sr = -1; choicePicker.sc = -1
        choicePicker.gridHead = head; choicePicker.gridR = r; choicePicker.gridC = c
        const b = blockModel.gridCellAt(head, r, c)
        const cell = b >= 0 ? root.cellForRow(b) : null
        if (cell && cell.teItem) {
            const pt = cell.teItem.mapToItem(root, 0, cell.teItem.height + 4)
            root.choiceX = pt.x; root.choiceY = pt.y
        }
        choicePicker.prefill(text)
        choicePicker.open()
    }
    function openCellChoicePicker(trow, r, c, s, vx, vy) {
        choicePicker.gridHead = -1
        choicePicker.row = -1; choicePicker.r = -1; choicePicker.c = -1
        choicePicker.srow = trow; choicePicker.sstart = s
        choicePicker.sr = r; choicePicker.sc = c
        root.choiceX = vx; root.choiceY = vy
        choicePicker.open()
    }
    ChoicePicker {
        id: choicePicker
        z: 60
        x: Math.max(8, Math.min(root.choiceX, root.width - width - 8))
        y: Math.max(8, Math.min(root.choiceY, root.height - height - 8))
        onClosed: root.forceActiveFocus()
        onEditOptions: choicePicker.cellSpanMode
                           ? choiceEditor.open2CellSpan(choicePicker.srow, choicePicker.sr,
                                                        choicePicker.sc, choicePicker.sstart)
                       : choicePicker.spanMode
                           ? choiceEditor.open2Span(choicePicker.srow, choicePicker.sstart)
                           : root.openChoiceEditor(choicePicker.row, choicePicker.c)
    }
    // Modal editor for a choice column's option set (opened from the column menu or
    // the picker's "Edit options…"). Centres itself in the editor.
    function openChoiceEditor(trow, col) { choiceEditor.open2(trow, col) }
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
        readonly property bool isTable: !menuInSel && root.menuRow >= 0
            && (blockModel.contentRevision, blockModel.typeForRow(root.menuRow) === 7)
        readonly property bool isMedia: !menuInSel && root.menuRow >= 0
            && (blockModel.contentRevision, blockModel.typeForRow(root.menuRow) === 3)
        readonly property bool isPdf: isMedia
            && (blockModel.contentRevision, blockModel.mediaKind(root.menuRow)) === "pdf"
        readonly property bool isVideo: isMedia
            && (blockModel.contentRevision, blockModel.mediaKind(root.menuRow)) === "video"
        readonly property bool isSketch: isMedia
            && (blockModel.contentRevision, blockModel.mediaKind(root.menuRow)) === "sketch"
        // In a full-frame tab (table/PDF/video) the menu is a view INTO one block, so
        // document-structural block ops (add/duplicate/copy block) don't belong.
        readonly property bool inFrameTab: root.activeTableRow >= 0 || root.activePdfRow >= 0 || root.activeVideoRow >= 0 || root.activeSketchRow >= 0
        // The menu row lies inside a multi-block selection → the menu is a RUN
        // menu: block ops (add above/below, duplicate, copy, paste, move,
        // insert below, delete) act on the whole run and the target highlight
        // covers it; single-block rows (open, media, code, chip, link, table
        // columns) are withheld — the clicked row isn't apparent under a run
        // highlight (isTable/isMedia/isCode read false here).
        readonly property bool menuInSel: cursor.hasSel && cursor.loRow !== cursor.hiRow
                                          && root.menuRow >= cursor.loRow && root.menuRow <= cursor.hiRow
        readonly property int runLo: menuInSel ? cursor.loRow : root.menuRow
        readonly property int runHi: menuInSel ? cursor.hiRow : root.menuRow
        // Lanes (SR-3 S7b): the split row under the menu, and what it allows.
        readonly property int laneRecord: menuInSel || root.menuRow < 0 ? -1
            : (blockModel.contentRevision, blockModel.splitRowOf(root.menuRow))
        readonly property bool canSplit: root.menuRow >= 0 && (blockModel.contentRevision,
            blockModel.typeForRow(runLo) !== 7 && blockModel.typeForRow(runLo) !== 10
            && (!menuInSel || (blockModel.laneForRow(runLo) < 0 && blockModel.laneForRow(runHi) < 0)))
        readonly property bool canAlign: laneRecord >= 0 && (blockModel.contentRevision,
            blockModel.splitRowLast(laneRecord) - laneRecord > blockModel.laneCount(laneRecord))
        readonly property int mergeBelow: laneRecord < 0 ? -1
            : (blockModel.contentRevision, root.mergeTargetBelow(laneRecord))
        // Derived tables (SR-4 S7a): the table cell under the menu.
        readonly property int gridHead: menuInSel || root.menuRow < 0 ? -1
            : (blockModel.contentRevision, blockModel.tableHeadOf(root.menuRow))
        readonly property int gridC: gridHead >= 0 ? (blockModel.contentRevision, blockModel.gridColumnOf(root.menuRow)) : -1
        readonly property bool gridHeaderRow: gridHead >= 0 && (blockModel.contentRevision, blockModel.isHeaderRow(root.menuRow))
        readonly property int gridKind: gridC >= 0 && !gridHeaderRow ? (blockModel.contentRevision, blockModel.gridColumnKind(gridHead, gridC)) : 0
        readonly property int gridAlign: gridC >= 0 ? (blockModel.contentRevision, blockModel.gridColAlign(gridHead, gridC)) : 0
        readonly property int gridCols: gridHead >= 0 ? (blockModel.contentRevision, blockModel.tableColumnCount(gridHead)) : 0
        readonly property int gridHeaders: gridHead >= 0 ? (blockModel.contentRevision, blockModel.headerCount(gridHead)) : 0
        readonly property bool gridOn: !inFrameTab && gridHead >= 0
        // The right-clicked issue, re-read live: a background-pass issue has no
        // suggestions until the worker's follow-up lands (spell.revision bumps).
        readonly property var liveIssue: {
            var dep = spell.revision
            if (!root.menuIssue) return null
            var it = root.menuIssueInCell ? spell.cellIssueAt(root.menuRow, root.menuCellR, root.menuCellC, root.menuIssue.s)
                                          : spell.issueAt(root.menuRow, root.menuIssue.s)
            return it.ruleId !== undefined ? it : root.menuIssue
        }
        // Table facts the row visibilities share (revision-dep'd once here).
        readonly property int tHdr: isTable ? (blockModel.contentRevision, blockModel.tableHeaderRows(root.menuRow)) : 0
        readonly property int tRows: isTable ? (blockModel.contentRevision, blockModel.tableRows(root.menuRow)) : 0
        readonly property int tCols: isTable ? (blockModel.contentRevision, blockModel.tableColumns(root.menuRow)) : 0
        readonly property bool bodyRow: isTable && root.menuCellR >= tHdr
        readonly property bool sortable: isTable && tRows - tHdr > 1
        // Multi-select targeting (2026-08-21): the menu operates on the
        // SELECTION when the right-clicked cell sits inside it (right-click
        // preserves the selection by construction), else single-target.
        readonly property bool selRowsHit: isTable && (tcur.selRev, tcur.selRows.length > 1
            && tcur.selRows.indexOf(root.menuCellR) >= 0)
        readonly property bool selColsHit: isTable && (tcur.selRev, tcur.selCols.length > 1
            && tcur.selCols.indexOf(root.menuCellC) >= 0)
        readonly property int selRowCount: (tcur.selRev, tcur.selRows.length)
        readonly property int selColCount: (tcur.selRev, tcur.selCols.length)
        // A live multi-cell rect containing the clicked cell (rect corners are
        // plain int props — they notify on their own, no selRev needed).
        readonly property bool selRectHit: isTable && tcur.rangeR0 >= 0
            && !(tcur.rangeR0 === tcur.rangeR1 && tcur.rangeC0 === tcur.rangeC1)
            && root.menuCellR >= Math.min(tcur.rangeR0, tcur.rangeR1)
            && root.menuCellR <= Math.max(tcur.rangeR0, tcur.rangeR1)
            && root.menuCellC >= Math.min(tcur.rangeC0, tcur.rangeC1)
            && root.menuCellC <= Math.max(tcur.rangeC0, tcur.rangeC1)
        // Multi-selection → ONE compact menu column (user ruling 2026-08-21:
        // the full three-column menu is noise when the target is the
        // selection); the three regular columns hide entirely.
        readonly property bool bulkMode: selRowsHit || selColsHit || selRectHit
        readonly property int rectRows: selRectHit
            ? Math.abs(tcur.rangeR1 - tcur.rangeR0) + 1 : 0
        readonly property int rectCols: selRectHit
            ? Math.abs(tcur.rangeC1 - tcur.rangeC0) + 1 : 0
        // Tallest of the visible columns — the inter-column dividers stretch to it.
        readonly property real bodyH: bulkMode ? bulkColMenu.implicitHeight
            : isTable
            ? Math.max(blockColMenu.implicitHeight, colColMenu.implicitHeight, rowColMenu.implicitHeight)
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
                MenuHeader { visible: blockMenu.isTable; text: "Block" }
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
                        var list = root.menuIssueInCell ? spell.issuesForCell(root.menuRow, root.menuCellR, root.menuCellC)
                                                        : spell.issuesForRow(root.menuRow)
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
                                  text: (root.menuIssueInCell ? "Fix all in cell (" : "Fix all in block (") + spellWell.fixable + ")"
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
                MenuRow { visible: blockMenu.isTable && root.activeTableRow < 0; text: "Open in tab"; onActivated: root.setActiveTab(blockModel.idForRow(root.menuRow)) }
                MenuRow { visible: blockMenu.isPdf && root.activePdfRow < 0; text: "Open in tab"; onActivated: root.setActiveTab(blockModel.idForRow(root.menuRow)) }
                MenuRow { visible: blockMenu.isVideo && root.activeVideoRow < 0; text: "Open in studio"; onActivated: root.setActiveTab(blockModel.idForRow(root.menuRow)) }
                MenuRow { visible: blockMenu.isSketch && root.activeSketchRow < 0; text: "Open in tab"; onActivated: root.setActiveTab(blockModel.idForRow(root.menuRow)) }
                MenuRow { visible: !blockMenu.inFrameTab && !blockMenu.isMedia && !blockMenu.isTable && !blockMenu.menuInSel
                          text: "Insert choice chip"
                          onActivated: { cursor.setCaret(root.menuRow, cursor.focusRow === root.menuRow ? cursor.focusCol : blockModel.contentForRow(root.menuRow).length); root.insertChoiceChip() } }
                MenuRow { visible: !blockMenu.inFrameTab; text: "Add block above"; onActivated: root.addBlockAbove(blockMenu.runLo) }
                MenuRow { visible: !blockMenu.inFrameTab; text: "Add block below"; onActivated: root.addBlockBelow(blockMenu.runHi) }
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.canSplit && blockMenu.gridHead < 0; text: "Split into columns"
                          onActivated: root.splitMenu(blockMenu.runLo, blockMenu.runHi) }
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.canAlign && blockMenu.gridHead < 0; text: "Align lanes"
                          onActivated: blockModel.alignLanes(blockMenu.laneRecord) }
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.mergeBelow >= 0 && blockMenu.gridHead < 0; text: "Merge with the row below"
                          onActivated: blockModel.mergeRowsIntoLanes(blockMenu.laneRecord, blockMenu.mergeBelow) }
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.laneRecord >= 0 && blockMenu.gridHead < 0; text: "Delete lane"; danger: true
                          onActivated: root.deleteLane(root.menuRow) }
                // Derived tables (SR-4 S7a). A split row takes the header role; a table cell gets the table's ops.
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.laneRecord >= 0 && (blockMenu.gridHead < 0 || !blockMenu.gridHeaderRow)
                          text: "Assign as header"
                          onActivated: blockModel.setHeaderRole(blockMenu.laneRecord, 1) }
                MenuHeader { visible: blockMenu.gridOn; text: "Table" }
                MenuRow { visible: blockMenu.gridOn; text: "Insert row above"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridInsertRow(h, r); return [r, c] }) }
                MenuRow { visible: blockMenu.gridOn; text: "Insert row below"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridInsertRow(h, r + 1); return [r + 1, c] }) }
                MenuRow { visible: blockMenu.gridOn; text: "Insert column left"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridInsertColumn(h, c); return [r, c] }) }
                MenuRow { visible: blockMenu.gridOn; text: "Insert column right"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridInsertColumn(h, c + 1); return [r, c + 1] }) }
                MenuRow { visible: blockMenu.gridOn; text: "Duplicate row"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridDuplicateRow(h, r); return [r + 1, c] }) }
                MenuRow { visible: blockMenu.gridOn; text: "Duplicate column"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridDuplicateColumn(h, c); return [r, c + 1] }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridC > 0; text: "Move column left"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridMoveColumn(h, c, c - 1); return [r, c - 1] }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridC < blockMenu.gridCols - 1; text: "Move column right"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridMoveColumn(h, c, c + 1); return [r, c + 1] }) }
                MenuRow { visible: blockMenu.gridOn; text: "Sort ascending"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridSortByColumn(h, c, true); return null }) }
                MenuRow { visible: blockMenu.gridOn; text: "Sort descending"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridSortByColumn(h, c, false); return null }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridKind !== 1; text: "Make choice column"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridSetColumnKind(h, c, 1); return [r, c] }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridKind !== 2; text: "Make checkmark column"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridSetColumnKind(h, c, 2); return [r, c] }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridKind !== 0; text: "Make text column"; danger: true
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridSetColumnKind(h, c, 0); return [r, c] }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridAlign !== 0; text: "Align column left"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridSetColAlign(h, c, 0); return null }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridAlign !== 1; text: "Align column center"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridSetColAlign(h, c, 1); return null }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridAlign !== 2; text: "Align column right"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridSetColAlign(h, c, 2); return null }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridHeaders < (blockModel.contentRevision, blockModel.gridRowCount(blockMenu.gridHead)) - 1
                          text: "Add a header row"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.setHeaderRole(h, blockModel.headerCount(h) + 1); return null }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridHeaders > 1; text: "Remove a header row"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.setHeaderRole(h, blockModel.headerCount(h) - 1); return null }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridHeaderRow; text: "Unassign header"
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.setHeaderRole(h, 0); return null }) }
                MenuRow { visible: blockMenu.gridOn; text: blockMenu.gridHeaderRow ? "Delete table" : "Delete row"; danger: true
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridDeleteRow(h, r); return [r, c] }) }
                MenuRow { visible: blockMenu.gridOn && blockMenu.gridCols > 1; text: "Delete column"; danger: true
                          onActivated: root.gridMenuOp(function(h, r, c) { blockModel.gridDeleteColumn(h, c); return [r, Math.max(0, c - 1)] }) }
                MenuRow { visible: !blockMenu.inFrameTab; text: blockMenu.menuInSel ? "Duplicate blocks" : "Duplicate block"
                          onActivated: root.duplicateRun(blockMenu.runLo, blockMenu.runHi) }
                MenuRow { visible: !blockMenu.inFrameTab && blockMenu.runLo > 0
                          text: blockMenu.menuInSel ? "Move blocks up" : "Move up"; onActivated: root.moveMenuRow(-1) }
                MenuRow { visible: !blockMenu.inFrameTab && (blockModel.contentRevision, blockMenu.runHi < blockModel.count - 1)
                          text: blockMenu.menuInSel ? "Move blocks down" : "Move down"; onActivated: root.moveMenuRow(1) }
                MenuRow { visible: !blockMenu.inFrameTab
                          text: blockMenu.menuInSel ? "Copy blocks" : blockMenu.isTable ? "Copy table" : "Copy"
                          onActivated: { if (blockMenu.menuInSel) { var ce = cursor.effectiveRange(); root.copyRange(ce.lR, ce.lC, ce.hR, ce.hC) }
                                         else root.copyBlock(root.menuRow) } }
                MenuRow { visible: !blockMenu.inFrameTab; text: "Paste"
                          onActivated: root.pasteAtBlock(root.menuRow, blockMenu.isTable ? root.menuCellR : -1, root.menuCellC) }
                MenuRow { visible: blockMenu.isMedia
                                   && (blockModel.contentRevision, blockModel.mediaKind(root.menuRow)) === "image"
                          text: "Copy image"
                          onActivated: { clipboard.writeImageFromFile(blockModel.mediaUrl(root.menuRow))
                                         Toasts.show(qsTr("Image copied")) } }
                MenuRow { visible: blockMenu.isMedia && !blockMenu.isSketch   // sketch has no backing file
                          text: Qt.platform.os === "windows" ? "Show in Explorer" : "Reveal in Finder"
                          onActivated: blockModel.revealMedia(root.menuRow) }
                MenuRow { visible: blockMenu.isMedia && !blockMenu.isSketch; text: "Open in ufb"
                          onActivated: blockModel.openMediaInUfb(root.menuRow) }
                MenuRow { visible: !blockMenu.isTable; text: "Insert table below"; onActivated: root.insertTableAt(blockMenu.runHi) }
                MenuRow { visible: !blockMenu.inFrameTab; text: "Insert sketch below"; onActivated: root.insertSketchAt(blockMenu.runHi) }
                MenuRow { visible: blockMenu.isTable; text: blockMenu.tHdr > 0 ? "Remove header row" : "Add header row"; onActivated: root.tblToggleHeader() }
                // comment on the current (single-row) text selection
                MenuRow {
                    visible: !blockMenu.inFrameTab && cursor.hasSel && cursor.loRow === cursor.hiRow
                    text: "Add comment"
                    onActivated: root.addCommentOnSelection()
                }
                // text-only transform (withheld for a run — the clicked row isn't apparent)
                Rectangle { visible: !blockMenu.isTable && !blockMenu.isMedia && !blockMenu.menuInSel; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow {
                    visible: !blockMenu.isTable && !blockMenu.isMedia && !blockMenu.menuInSel
                    text: blockMenu.isCode ? "Change language…" : "Make code block"
                    onActivated: blockMenu.isCode ? root.openLangPopupForRow(root.menuRow)
                                                  : root.makeCodeAt(root.menuRow)
                }
                // cell image (right-clicked table cell)
                Rectangle { visible: root.menuCellHasImage; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: root.menuCellHasImage; text: "Copy image"
                          onActivated: { clipboard.writeImageFromFile(blockModel.tableCellMediaUrl(root.menuRow, root.menuCellR, root.menuCellC))
                                         Toasts.show(qsTr("Image copied")) } }
                MenuRow { visible: root.menuCellHasImage; text: "Remove image"; danger: true; onActivated: root.tblRemoveImage() }
                Rectangle { visible: !blockMenu.inFrameTab; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: !blockMenu.inFrameTab; text: blockMenu.menuInSel ? "Delete blocks" : "Delete block"; danger: true
                          onActivated: root.deleteRun(blockMenu.runLo, blockMenu.runHi) }
            }

            Rectangle { visible: blockMenu.isTable && !blockMenu.bulkMode; width: 1; height: blockMenu.bodyH; color: Theme.colors.divider }

            // --- Column column (the right-clicked table column) ---
            Column {
                id: colColMenu
                visible: blockMenu.isTable && !blockMenu.bulkMode
                spacing: 1
                MenuHeader { text: "Column" }
                MenuRow { scope: "column"; text: "Select column"; onActivated: root.selectTableColumn(root.menuRow, root.menuCellC) }
                MenuRow { scope: "column"; text: "Insert column left"; onActivated: root.tblInsColLeft() }
                MenuRow { scope: "column"; text: "Insert column right"; onActivated: root.tblInsColRight() }
                MenuRow { visible: root.menuCellC > 0; scope: "column"; text: "Move column left"; onActivated: root.tblMoveCol(-1) }
                MenuRow { visible: root.menuCellC < blockMenu.tCols - 1; scope: "column"; text: "Move column right"; onActivated: root.tblMoveCol(1) }
                MenuRow { scope: "column"; text: "Duplicate column"; onActivated: root.tblDupCol() }
                Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
                MenuSegRow {
                    label: "Align"
                    readonly property int _a: blockMenu.isTable ? (blockModel.contentRevision, blockModel.tableColAlign(root.menuRow, root.menuCellC)) : 0
                    MenuIconBtn { icon: "text-align-left";   on: parent._a === 0; onActivated: root.tblAlign(0) }
                    MenuIconBtn { icon: "text-align-center"; on: parent._a === 1; onActivated: root.tblAlign(1) }
                    MenuIconBtn { icon: "text-align-right";  on: parent._a === 2; onActivated: root.tblAlign(2) }
                }
                // One-shot sort of the body rows by this column (header stays pinned).
                Rectangle { visible: blockMenu.sortable; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuSegRow {
                    visible: blockMenu.sortable
                    label: "Sort"
                    MenuIconBtn { icon: "sort-ascending";  onActivated: root.tblSort(true) }
                    MenuIconBtn { icon: "sort-descending"; onActivated: root.tblSort(false) }
                }
                // Column type
                Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: root.tblColKind() !== 1; scope: "column"; text: "Make choice column"; onActivated: root.tblMakeChoiceCol() }
                MenuRow { visible: root.tblColKind() !== 2; scope: "column"; text: "Make checkmark column"; onActivated: root.tblMakeCheckCol() }
                MenuRow { visible: root.tblColKind() === 1; scope: "column"; text: "Edit options…"; onActivated: root.openChoiceEditor(root.menuRow, root.menuCellC) }
                MenuRow { visible: root.tblColKind() !== 0 && !root.boardMode; scope: "column"; text: "View as board"; onActivated: root.openBoard(root.menuRow, root.menuCellC) }
                MenuRow { visible: root.tblColKind() !== 0; scope: "column"; text: "Make text column"; danger: true; onActivated: root.tblMakeTextCol() }
                Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { scope: "column"; text: "Delete column"; danger: true; onActivated: root.tblDelCol() }
            }

            Rectangle { visible: blockMenu.isTable && !blockMenu.bulkMode; width: 1; height: blockMenu.bodyH; color: Theme.colors.divider }

            // --- Row column (the right-clicked table row) ---
            Column {
                id: rowColMenu
                visible: blockMenu.isTable && !blockMenu.bulkMode
                spacing: 1
                MenuHeader { text: "Row" }
                MenuRow { scope: "row"; text: "Select row"; onActivated: root.selectTableRow(root.menuRow, root.menuCellR) }
                MenuRow { scope: "row"; text: "Insert row above"; onActivated: root.tblInsRowAbove() }
                MenuRow { scope: "row"; text: "Insert row below"; onActivated: root.tblInsRowBelow() }
                // Reorder/duplicate: body rows only (a header row's place is structural).
                MenuRow { visible: root.menuCellR > blockMenu.tHdr; scope: "row"; text: "Move row up"; onActivated: root.tblMoveRow(-1) }
                MenuRow { visible: blockMenu.bodyRow && root.menuCellR < blockMenu.tRows - 1; scope: "row"; text: "Move row down"; onActivated: root.tblMoveRow(1) }
                MenuRow { visible: blockMenu.bodyRow; scope: "row"; text: "Duplicate row"; onActivated: root.tblDupRow() }
                Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { scope: "row"; text: "Delete row"; danger: true; onActivated: root.tblDelRow() }
            }

            // --- Bulk column: the ONE compact menu when the right-click
            // targets a multi-selection (rows set / columns set / cell rect).
            Column {
                id: bulkColMenu
                visible: blockMenu.bulkMode
                spacing: 1
                MenuHeader {
                    text: blockMenu.selRowsHit ? blockMenu.selRowCount + " rows"
                        : blockMenu.selColsHit ? blockMenu.selColCount + " columns"
                        : blockMenu.rectRows + "×" + blockMenu.rectCols + " cells"
                }
                // Rows set
                MenuRow { visible: blockMenu.selRowsHit; scope: "row"; text: "Copy as table"; onActivated: root.tblCopyRows() }
                MenuRow { visible: blockMenu.selRowsHit; scope: "row"; text: "Clear contents"; onActivated: root.tblClearRows() }
                // Columns set
                MenuRow { visible: blockMenu.selColsHit; scope: "column"; text: "Copy as table"; onActivated: root.tblCopyCols() }
                MenuRow { visible: blockMenu.selColsHit; scope: "column"; text: "Clear contents"; onActivated: root.tblClearCols() }
                MenuSegRow {
                    visible: blockMenu.selColsHit
                    label: "Align"
                    MenuIconBtn { icon: "text-align-left";   onActivated: root.tblAlign(0) }
                    MenuIconBtn { icon: "text-align-center"; onActivated: root.tblAlign(1) }
                    MenuIconBtn { icon: "text-align-right";  onActivated: root.tblAlign(2) }
                }
                MenuRow { visible: blockMenu.selColsHit; scope: "column"; text: "Make choice columns"; onActivated: root.tblMakeChoiceCol() }
                MenuRow { visible: blockMenu.selColsHit; scope: "column"; text: "Make checkmark columns"; onActivated: root.tblMakeCheckCol() }
                MenuRow { visible: blockMenu.selColsHit; scope: "column"; text: "Make text columns"; onActivated: root.tblMakeTextCol() }
                // Cell rect: promote to whole rows/columns, or act on the cells.
                MenuRow { visible: blockMenu.selRectHit
                          text: "Select " + blockMenu.rectRows + (blockMenu.rectRows === 1 ? " row" : " rows")
                          onActivated: root.tblSelectRectRows() }
                MenuRow { visible: blockMenu.selRectHit
                          text: "Select " + blockMenu.rectCols + (blockMenu.rectCols === 1 ? " column" : " columns")
                          onActivated: root.tblSelectRectCols() }
                MenuRow { visible: blockMenu.selRectHit; text: "Copy cells"; onActivated: root.tblCopyRect() }
                MenuRow { visible: blockMenu.selRectHit; text: "Clear cells"; onActivated: root.tblClearRect() }
                // Destructive tail (sets only — the rect clears, never deletes)
                Rectangle { visible: !blockMenu.selRectHit; width: parent.width; height: 1; color: Theme.colors.divider }
                MenuRow { visible: blockMenu.selRowsHit; scope: "row"
                          text: "Delete " + blockMenu.selRowCount + " rows"
                          danger: true; onActivated: root.tblDelRow() }
                MenuRow { visible: blockMenu.selColsHit; scope: "column"
                          text: "Delete " + blockMenu.selColCount + " columns"
                          danger: true; onActivated: root.tblDelCol() }
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
