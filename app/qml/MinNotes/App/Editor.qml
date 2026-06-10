import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Pdf

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
    Component.onCompleted: { forceActiveFocus(); _recomputeVideoRows(); _recomputePdfRows(); blockModel.setContentWidth(pageWidth) }
    // Single 760 reading measure shared by ALL blocks (tables included for now),
    // left-aligned at a common edge (the column is centred in the window). Tables
    // scroll horizontally inside their delegate when content exceeds it.
    property real pageWidth: Math.min(width - 40, Theme.dim.columnWidth)
    // Media is known-geometry: tell the model the width it derives media heights
    // from, and re-tell it on resize so reserved height stays exact (no jump).
    onPageWidthChanged: blockModel.setContentWidth(pageWidth)
    readonly property real leftEdge: (flick.width - pageWidth) / 2
    function measureForType(t) { return pageWidth }
    function measureForRow(row) { return pageWidth }

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
    onActivePdfRowChanged: if (activePdfId !== "" && activePdfRow < 0) activePdfId = ""
    // The block id shown full-frame ("" = Document view) — drives the tab strip's
    // active state across both table and PDF tabs.
    readonly property string activeFrameId: activeTableId !== "" ? activeTableId : activePdfId
    function setActiveTab(id) {
        boardMode = false; boardCol = -1     // a tab switch always lands on the grid
        if (id === "") { activeTableId = ""; activePdfId = ""; return }
        var r = blockModel.rowForId(id)
        if (blockModel.typeForRow(r) === 7) { activePdfId = ""; activeTableId = id }
        else { activeTableId = ""; activePdfId = id }
    }
    // Kanban board: the active table tab rendered as a board grouped by a
    // choice/check column. View state only (not persisted, not undoable).
    property bool boardMode: false
    property int  boardCol: -1
    function openBoard(row, c) {
        setActiveTab(blockModel.idForRow(row))
        boardCol = c
        boardMode = true
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
    property string blockDragText: ""    // first line, shown in the ghost
    property real blockDragViewY: 0       // viewport y of the cursor
    property int  dropGap: -1            // insertion gap 0..count (line at its top)
    property int  hoverRow: -1           // row whose grip is lit
    readonly property real gutterX: leftEdge   // grip gutter sits just left of the common left edge

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
    // Cell-image resize (the same affordance scoped to a focused table cell that
    // holds an image; Document view only — the full-frame tab keeps just the tint).
    property bool cellImgResizing: false
    property real cellResizeW: 0
    property real cellResizeAspect: 1
    property real _cellResizePressX: 0
    property real _cellResizeStartW: 0
    readonly property var _cellImgBt: (tcur.active && cellForRow(tcur.row)) ? cellForRow(tcur.row).tableItem : null
    readonly property bool cellImgActive: tcur.active && activeTableRow < 0 && activePdfRow < 0
        && _cellImgBt && (blockModel.contentRevision,
                          blockModel.tableCellMedia(tcur.row, tcur.cr, tcur.cc) !== "")
    // The focused cell image's rect in viewport coords (re-evaluated on scroll /
    // table h-scroll / layout / cell change, then mapped from the BlockTable).
    readonly property rect cellImgRect: {
        var dep = flick.contentY + blockModel.layoutRevision + blockModel.contentRevision
                + (_cellImgBt ? _cellImgBt.scrollX : 0) + tcur.cr + tcur.cc + tcur.pos
        if (!cellImgActive || !_cellImgBt) return Qt.rect(0, 0, 0, 0)
        var r = _cellImgBt.cellImageRect(tcur.cr, tcur.cc)
        var tl = _cellImgBt.mapToItem(root, r.x, r.y)
        return Qt.rect(tl.x, tl.y, r.width, r.height)
    }
    // Column inner width for the focused cell (the resize upper bound).
    readonly property real cellImgMaxW: (cellImgActive && _cellImgBt) ? _cellImgBt.colW(tcur.cc) - 16 : 800

    // Is a content-x in the left grip gutter (just left of the text column)?
    function inGutter(mx) { return mx < gutterX - 4 && mx > gutterX - 40 }
    // Insertion gap (0..count) for a content-y: before/after the row by its midpoint.
    function gapForY(cy) {
        var n = blockModel.count
        if (cy <= 0) return 0
        var row = blockModel.rowForY(cy)
        var mid = blockModel.yForRow(row) + blockModel.heightForRow(row) / 2
        return (cy < mid) ? row : row + 1
    }
    // Content-y of a gap's drop line (top of that block, or doc end).
    function gapY(gap) {
        var n = blockModel.count
        return (gap >= n) ? blockModel.yForRow(n - 1) + blockModel.heightForRow(n - 1)
                          : blockModel.yForRow(gap)
    }
    function commitBlockDrag() {
        if (blockDragRow >= 0 && dropGap >= 0) {
            var to = (dropGap > blockDragRow) ? dropGap - 1 : dropGap
            blockModel.moveBlock(blockDragRow, to)
        }
        blockDragging = false; blockDragRow = -1; dropGap = -1
    }

    Rectangle { anchors.fill: parent; color: Theme.colors.surface }
    MouseArea { anchors.fill: parent; onClicked: root.forceActiveFocus() }  // reclaim focus on bg click

    // --- Inline video player. ONE decoder, root-owned (a pooled MediaBlock
    // can't host a live decoder — it recycles mid-scroll). The surface + a
    // dedicated transport toolbar BELOW it are overlaid on the playing block,
    // which reserves videoTransportH extra height while it's the active player
    // (see the overlay below). A single shared decoder gives "one video at a
    // time" for free; scrolling the playing block out of view tears it down.
    // Transport logic is ported from ufb's VideoPreview. ---
    property int  videoPlayingRow: -1
    property bool videoLoop: false
    // False from activation until the active video paints its first frame, so the
    // single shared surface never flashes the PREVIOUS video's stale frame — the
    // (correct) poster stays up until the new frame is ready.
    property bool _videoSurfaceReady: false
    readonly property real videoTransportH: 40
    readonly property real pdfNavH: 40          // reserved under an inline PDF page for the nav strip (matches kPdfNav)
    readonly property bool videoVisible: videoPlayingRow >= 0
        && activeTableRow < 0 && activePdfRow < 0   // not in a full-frame tab
        && videoPlayingRow >= firstVisible && videoPlayingRow <= lastVisible
    onVideoVisibleChanged: if (!videoVisible && videoPlayingRow >= 0) stopVideo()

    // Per-video playhead memory (keyed by file path) so a torn-down video shows
    // its last frame as the poster and resumes there. videoPlayheadRev makes the
    // poster bindings re-evaluate when a playhead is recorded.
    property var videoPlayheads: ({})
    property int videoPlayheadRev: 0
    function videoPlayheadFor(row) {
        var key = blockModel.mediaLocalPath(row)
        return (key !== "" && videoPlayheads[key] !== undefined) ? videoPlayheads[key] : 0
    }
    function _rememberVideoPlayhead() {   // bank the last-accessed frame
        if (videoPlayingRow < 0) return
        var key = blockModel.mediaLocalPath(videoPlayingRow)
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
        var key = blockModel.mediaLocalPath(row)
        return (key !== "" && pdfPages[key] !== undefined) ? pdfPages[key] : 0
    }
    function setPdfPage(row, page) {
        var n = blockModel.mediaPdfPages(row)
        var p = Math.max(0, Math.min(page, n - 1))
        var key = blockModel.mediaLocalPath(row)
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
        if (videoDec.fps > 0) videoAudio.seek(f / videoDec.fps)
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
        videoDec.close(); videoAudio.close()
        _vidScrubTarget = -1
        var p = blockModel.mediaLocalPath(row)
        if (p === "" || !videoDec.open(p)) { videoPlayingRow = -1; return false }
        videoPlayingRow = row
        videoAudio.initialize()   // idempotent; open() no-ops without it
        videoAudio.open(p)
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
        videoDec.close(); videoAudio.close()
        videoPlayingRow = -1; _videoSurfaceReady = false
        _vidScrubTarget = -1; _vidFastSeekDir = 0; videoFastSeekTimer.stop()
    }
    // Frame-accurate step — implies review, so pause first.
    function stepVideoFrames(n) { videoDec.pause(); videoAudio.pause(); _vidScrubTo(_vidIntendedFrame() + n) }
    function seekVideoStart() { videoDec.pause(); videoAudio.pause(); _vidScrubTo(0) }
    function seekVideoEnd()   { videoDec.pause(); videoAudio.pause(); _vidScrubTo(videoDec.frameCount - 1) }
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
        _vidFastSeekDir = dir; _vidFastSeekSpeed = 2.0; _vidFastSeekElapsed = 0
        _vidFastSeekPos = (videoDec.fps > 0) ? _vidIntendedFrame() / videoDec.fps : 0
        videoFastSeekTimer.start()
    }
    function stopVideoFastSeek() { _vidFastSeekDir = 0; videoFastSeekTimer.stop() }

    VideoDecoder { id: videoDec }
    AudioPlayer  { id: videoAudio }
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
    // Drop-onto-a-cell target (an image dragged over a table cell); −1 = none. When
    // set, the block-insertion gap line is suppressed and the cell is highlighted.
    property int dropTableRow: -1
    property int dropCellR: -1
    property int dropCellC: -1
    function clearDropState() { imageDropGap = -1; dropTableRow = -1; dropCellR = -1; dropCellC = -1 }
    // Aim a drag at a content point: a table cell wins (→ image into cell), else a
    // block-insertion gap. Mutually exclusive, so the affordances don't both show.
    function aimDrop(cx, cy) {
        var th = root.tableHitAt(cx, cy)
        if (th) { dropTableRow = th.row; dropCellR = th.r; dropCellC = th.c; imageDropGap = -1 }
        else    { dropTableRow = -1; dropCellR = -1; dropCellC = -1; imageDropGap = root.gapForY(cy) }
    }
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onEntered: (drag) => { root.imageDropActive = true; root.aimDrop(drag.x, drag.y + flick.contentY) }
        onPositionChanged: (drag) => { root.aimDrop(drag.x, drag.y + flick.contentY) }
        onExited: { root.imageDropActive = false; root.clearDropState() }
        onDropped: (drop) => {
            root.imageDropActive = false
            if (!drop.hasUrls) { root.clearDropState(); return }
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
            var any = false
            for (var i = 0; i < drop.urls.length; ++i)
                if (blockModel.insertMediaFromUrl(afterRow, drop.urls[i].toString())) { afterRow++; any = true }
            root.clearDropState()
            if (any) { cursor.setCaret(Math.max(0, afterRow), 0); root.ensureVisible(afterRow) }
            drop.accept()
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
        function toggleMark(kind) {
            var bit = kind === "bold" ? 1 : kind === "italic" ? 2 : kind === "code" ? 4
                    : kind === "strike" ? 8 : kind === "underline" ? 16 : 0
            if (bit) activeMarks ^= bit
        }
        function clearMarks() { activeMarks = 0 }

        // The caret stays hidden until the user first interacts (click / key /
        // type), so the app opens with nothing active. sync() is the chokepoint
        // for every caret change, so flag it active there.
        property bool active: false

        // Mirror the caret into the model so undo transactions can snapshot it
        // (and stamp a just-pushed entry's caret-after). Called after any change.
        function sync() { active = true; blockModel.noteCaret(focusRow, focusCol, anchorRow, anchorCol) }

        function setCaret(r, col) { anchorRow = r; anchorCol = col; focusRow = r; focusCol = col; goalX = -1; sync() }
        function move(r, col, extend) {
            // Leaving a block with a collapsed caret consumes its markdown → spans.
            if (!extend && r !== focusRow && !root.dragging) blockModel.commitMarkdown(focusRow)
            focusRow = r; focusCol = col
            if (!extend) { anchorRow = r; anchorCol = col }
            root.ensureVisible(r)
            sync()
        }
        function deleteSelection() {
            var lR = loRow, lC = loCol, hR = hiRow, hC = hiCol
            // Keep a table's JSON (and other opaque content) out of a text merge:
            // a table at the HI end is included whole (deleteRange removes it); one
            // at the LO end (or a lone opaque block) bails rather than spill.
            if (lR !== hR) {
                if (opaque(lR)) { setCaret(loRow, loCol); return }
                if (opaque(hR)) hC = blockModel.contentForRow(hR).length
            } else if (opaque(lR)) { setCaret(loRow, loCol); return }
            blockModel.deleteRange(lR, lC, hR, hC)
            anchorRow = lR; anchorCol = lC; focusRow = lR; focusCol = lC
            goalX = -1
            root.ensureVisible(lR)
            sync()
        }
        // Opaque blocks (table/media/divider) hold non-prose content (a table's is
        // JSON) — a cross-block text merge would spill it. Never merge across one.
        function opaque(r) { var t = blockModel.typeForRow(r); return t === 7 || t === 3 || t === 6 }
        // Is the CARET on a media/divider? (Tables route to tcur, never reach these
        // text ops.) These blocks have no text caret, so text ops must not edit them.
        function opaqueHere() { var t = blockModel.typeForRow(focusRow); return t === 3 || t === 6 }
        function backspace() {
            if (hasSel) { deleteSelection(); return }
            if (opaqueHere()) { root.deleteBlock(focusRow); return }   // caret on media/divider → delete it
            if (focusCol > 0) {
                blockModel.deleteRange(focusRow, focusCol - 1, focusRow, focusCol)
                setCaret(focusRow, focusCol - 1)
            } else if (focusRow > 0) {
                var pt = blockModel.typeForRow(focusRow - 1)
                if (pt === 7) {                           // table before: step into its last cell
                    if (blockModel.contentForRow(focusRow).length === 0 && blockModel.count > 1)
                        blockModel.removeBlock(focusRow)  // drop the empty trailing block
                    root.enterTable(focusRow - 1, false); root.ensureVisible(focusRow - 1); return
                }
                if (pt === 3 || pt === 6) {               // media/divider before: backspace deletes it
                    blockModel.removeBlock(focusRow - 1)
                    setCaret(focusRow - 1, 0); root.ensureVisible(focusRow - 1); return
                }
                var pl = blockModel.contentForRow(focusRow - 1).length
                blockModel.deleteRange(focusRow - 1, pl, focusRow, 0)
                setCaret(focusRow - 1, pl)
                root.ensureVisible(focusRow - 1)
            }
        }
        function forwardDelete() {
            if (hasSel) { deleteSelection(); return }
            if (opaqueHere()) { root.deleteBlock(focusRow); return }   // caret on media/divider → delete it
            var len = blockModel.contentForRow(focusRow).length
            if (focusCol < len) {
                blockModel.deleteRange(focusRow, focusCol, focusRow, focusCol + 1)
            } else if (focusRow < blockModel.count - 1) {
                var nt = blockModel.typeForRow(focusRow + 1)
                if (nt === 7) { root.enterTable(focusRow + 1, true); return }   // step into the table
                if (nt === 3 || nt === 6) { blockModel.removeBlock(focusRow + 1); setCaret(focusRow, focusCol); return }
                // At a block's end: pull the next block up onto this one (caret stays).
                blockModel.deleteRange(focusRow, len, focusRow + 1, 0)
            }
            setCaret(focusRow, focusCol)
        }
        function insertChar(ch) {
            if (opaqueHere()) {                           // typing next to a media/divider → fresh paragraph after it
                blockModel.insertBlock(focusRow + 1); setCaret(focusRow + 1, 0)
            }
            if (hasSel) deleteSelection()
            blockModel.insertText(focusRow, focusCol, ch, activeMarks)   // armed attrs → span the run
            setCaret(focusRow, focusCol + ch.length)
            // Markdown autoformat fires on the space that completes a prefix
            // (e.g. "## "): the prefix is consumed, so pull the caret back.
            if (ch === " ") {
                var strip = blockModel.applyMarkdownTrigger(focusRow)
                if (strip > 0) setCaret(focusRow, Math.max(0, focusCol - strip))
            }
        }
        function splitLine() {
            if (hasSel) deleteSelection()
            if (opaqueHere()) {   // Enter on a media/divider → a fresh paragraph below it
                blockModel.insertBlock(focusRow + 1); setCaret(focusRow + 1, 0); root.ensureVisible(focusRow + 1); return
            }
            // "```" / "```lang" + Enter → an (empty) code block; caret stays inside.
            if (blockModel.makeCodeBlockIfFence(focusRow)) { setCaret(focusRow, 0); return }
            // Inside a code block, Enter adds a newline; pressing it on an empty
            // trailing line exits to a fresh paragraph below.
            if (blockModel.typeForRow(focusRow) === 2) {
                var c = blockModel.contentForRow(focusRow)
                var atEnd = focusCol >= c.length
                if (atEnd && (c.length === 0 || c.charAt(c.length - 1) === "\n")) {
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
            blockModel.splitBlock(focusRow, focusCol)
            setCaret(focusRow + 1, 0)
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
        readonly property int row: cursor.focusRow
        readonly property bool active: (blockModel.layoutRevision, blockModel.contentRevision,
                                        blockModel.typeForRow(cursor.focusRow) === 7)

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
            clearRange()
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
            blockModel.tableCellInsert(row, cr, cc, pos, ch)
            pos += ch.length; anchorPos = pos; cursor.sync()
        }
        function backspace() {
            if (pos !== anchorPos) { delSel(); cursor.sync(); return }
            if (pos > 0) { blockModel.tableCellDelete(row, cr, cc, pos - 1, pos); pos--; anchorPos = pos }
            // At the cell start, Backspace removes the cell's image (it sits above
            // the text), mirroring backspace-at-block-start.
            else if (blockModel.tableCellMedia(row, cr, cc) !== "") blockModel.tableClearCellMedia(row, cr, cc)
            cursor.sync()
        }
        function forwardDelete() {
            if (pos !== anchorPos) { delSel(); cursor.sync(); return }
            if (pos < text().length) blockModel.tableCellDelete(row, cr, cc, pos, pos + 1)
            // Forward-delete in an empty cell also clears its image.
            else if (text().length === 0 && blockModel.tableCellMedia(row, cr, cc) !== "")
                blockModel.tableClearCellMedia(row, cr, cc)
            cursor.sync()
        }
        function left(shift) {
            if (pos > 0) pos--
            else if (cc > 0) { cc--; pos = text().length }
            else if (cr > 0) { cr--; cc = cols() - 1; pos = text().length }
            if (!shift) anchorPos = pos
            cursor.sync()
        }
        function right(shift) {
            if (pos < text().length) pos++
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
            root.ensureVisible(r)
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
    // clean path to cross-block selection: rowForY() finds the block, then that
    // block's own TextEdit maps pixels → column via positionAt().
    function cellForRow(r) {
        for (var i = 0; i < pool.count; ++i) {
            var c = pool.itemAt(i)
            if (c && c.active && c.logicalRow === r) return c
        }
        return null
    }
    // (cx, cy) in CONTENT coordinates → {row, col}.
    function hitTest(cx, cy) {
        var row = blockModel.rowForY(Math.max(0, cy))
        var cell = cellForRow(row)
        if (!cell || cell.isMedia) return { row: row, col: 0 }
        var te = cell.teItem
        var col = te.positionAt(cx - te.x, cy - cell.y - te.y)
        return { row: row, col: col }
    }
    // (cx, cy) in CONTENT coordinates → the row index if the click is over a task
    // item's checkbox glyph (the left decoration column, first line), else -1. Its
    // delegate can't own a MouseArea (the document mouse layer sits above it), so the
    // central handler hit-tests the glyph zone here.
    function taskCheckboxAt(cx, cy) {
        var row = blockModel.rowForY(Math.max(0, cy))
        if (blockModel.typeForRow(row) !== 8) return -1
        var cell = cellForRow(row)
        if (!cell) return -1
        var te = cell.teItem
        var lx = cx - cell.colLeft           // x within the cell's content column
        var ly = cy - cell.y - te.y          // y relative to the text top
        if (lx >= 0 && lx <= 20 && ly >= -2 && ly <= te.lineH) return row
        return -1
    }
    // (cx, cy) in CONTENT coordinates → {row, r, c, pos} if over a table, else
    // null. Delegates to the table's own BlockTable.cellAtPoint (its delegate
    // can't own a MouseArea — the document mouse layer sits above it).
    function tableHitAt(cx, cy) {
        var row = blockModel.rowForY(Math.max(0, cy))
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
    function beginTableInteraction(bt, row, lx, ly) {
        var bc = bt.columnBorderAt(lx)
        if (bc >= 0) {                                       // near a border → resize
            root.tableResizing = true; root.resizeRow = row; root.resizeColIdx = bc
            root.resizeW = bt.widthForDrag(bc, lx)
            return
        }
        var hit = bt.cellAtPoint(lx, ly)
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
                tcur.clearRange(); tcur.cr = hit.r; tcur.cc = hit.c
                tcur.anchorPos = root.tableAnchorPos; tcur.pos = hit.pos
            } else tcur.setRange(root.tableAnchorR, root.tableAnchorC, hit.r, hit.c)
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
    function navRight(shift) {
        cursor.resetGoalX(); cursor.clearMarks()
        var fb = root.focusBlockItem, n = blockModel.count
        if (fb && cursor.focusCol < fb.length) cursor.move(cursor.focusRow, cursor.focusCol + 1, shift)
        else if (cursor.focusRow < n - 1) {
            if (blockModel.typeForRow(cursor.focusRow + 1) === 7) root.enterTable(cursor.focusRow + 1, true)
            else cursor.move(cursor.focusRow + 1, 0, shift)
        }
    }
    function navLeft(shift) {
        cursor.resetGoalX(); cursor.clearMarks()
        if (cursor.focusCol > 0) cursor.move(cursor.focusRow, cursor.focusCol - 1, shift)
        else if (cursor.focusRow > 0) {
            if (blockModel.typeForRow(cursor.focusRow - 1) === 7) root.enterTable(cursor.focusRow - 1, false)
            else cursor.move(cursor.focusRow - 1, blockModel.contentForRow(cursor.focusRow - 1).length, shift)
        }
    }
    // Map the sticky goal-x onto a visual line of `row`'s block (te-local y),
    // returning the column there. Falls back to col 0 if that block has no live
    // delegate (off-screen) or is media. Used when up/down crosses a boundary.
    function colAtGoalX(row, yLocal) {
        var cell = cellForRow(row)
        if (!cell || cell.isMedia) return 0
        return cell.teItem.positionAt(cursor.goalX, yLocal)
    }
    function navDown(shift) {
        cursor.clearMarks()
        var fb = root.focusBlockItem, n = blockModel.count
        if (!fb) return
        var r = fb.positionToRectangle(Math.min(cursor.focusCol, fb.length))
        var lh = r.height > 0 ? r.height : 18
        if (cursor.goalX < 0) cursor.goalX = r.x          // capture at the start of a vertical run
        if (r.y < fb.contentHeight - lh * 1.5)            // another visual line below in this block
            cursor.move(cursor.focusRow, fb.positionAt(cursor.goalX, r.y + lh * 1.5), shift)
        else if (cursor.focusRow < n - 1) {               // cross into the next block at goal-x
            if (blockModel.typeForRow(cursor.focusRow + 1) === 7) root.enterTable(cursor.focusRow + 1, true)
            else cursor.move(cursor.focusRow + 1, colAtGoalX(cursor.focusRow + 1, 2), shift)
        }
    }
    function navUp(shift) {
        cursor.clearMarks()
        var fb = root.focusBlockItem
        if (!fb) return
        var r = fb.positionToRectangle(Math.min(cursor.focusCol, fb.length))
        var lh = r.height > 0 ? r.height : 18
        if (cursor.goalX < 0) cursor.goalX = r.x
        if (r.y > lh * 0.5)                               // another visual line above in this block
            cursor.move(cursor.focusRow, fb.positionAt(cursor.goalX, r.y - lh * 0.5), shift)
        else if (cursor.focusRow > 0) {                   // cross into the previous block's last line
            if (blockModel.typeForRow(cursor.focusRow - 1) === 7) { root.enterTable(cursor.focusRow - 1, false); return }
            var prev = cellForRow(cursor.focusRow - 1)
            var yLast = (prev && !prev.isMedia) ? prev.teItem.contentHeight - 2 : 0
            cursor.move(cursor.focusRow - 1, colAtGoalX(cursor.focusRow - 1, yLast), shift)
        }
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
        var lo = Math.min(tcur.pos, tcur.anchorPos), hi = Math.max(tcur.pos, tcur.anchorPos)
        if (lo === hi) { hi = blockModel.tableCell(tcur.row, tcur.cr, tcur.cc).length; lo = 0 }
        blockModel.tableClearCellFormat(tcur.row, tcur.cr, tcur.cc, lo, hi)
        cursor.sync()
    }

    // Right-rail colour tools: recolour / highlight the current selection (no-op
    // without one for now). One grouped undo step.
    function applyTextColor(color) {
        var hex = "" + color
        if (tcur.active) { applyTableColor(true, hex); return }   // table: text colour
        if (!cursor.hasSel) return
        blockModel.beginGroup(cursor.loRow, cursor.hiRow)
        for (var r = cursor.loRow; r <= cursor.hiRow; ++r)
            blockModel.setTextColor(r, rowSelStart(r), rowSelEnd(r), hex)
        blockModel.endGroup()
        cursor.sync()
    }
    function applyHighlight(color) {
        var hex = "" + color
        if (tcur.active) { applyTableColor(false, hex); return }  // table: cell background
        if (!cursor.hasSel) return
        blockModel.beginGroup(cursor.loRow, cursor.hiRow)
        for (var r = cursor.loRow; r <= cursor.hiRow; ++r)
            blockModel.setHighlight(r, rowSelStart(r), rowSelEnd(r), hex)
        blockModel.endGroup()
        cursor.sync()
    }
    // Right-rail colour applied to a table: fg = text colour, else cell background.
    // A cell range is coloured if one is selected, otherwise just the focused cell.
    function applyTableColor(isFg, hex) {
        var fr = cursor.focusRow
        if (tcur.rangeR0 >= 0)
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
    function addBlockAbove(row) { blockModel.insertBlock(row);     cursor.setCaret(row, 0);     cursor.sync() }
    function addBlockBelow(row) { blockModel.insertBlock(row + 1); cursor.setCaret(row + 1, 0); cursor.sync() }
    function duplicateBlock(row) { blockModel.duplicateBlock(row); cursor.setCaret(row + 1, 0); cursor.sync() }
    function makeCodeAt(row)    { blockModel.makeCodeBlock(row, ""); cursor.setCaret(row, 0); cursor.sync() }
    function insertTableAt(row) { blockModel.insertTable(row, 3, 3); cursor.setCaret(row + 1, 0); tcur.place(0, 0, 0); root.ensureVisible(row + 1) }
    function insertTableAtCaret() { insertTableAt(cursor.focusRow) }
    // Table context-menu ops — act on the right-clicked block (menuRow) + cell.
    function tblInsRowAbove() { blockModel.tableInsertRow(menuRow, menuCellR) }
    function tblInsRowBelow() { blockModel.tableInsertRow(menuRow, menuCellR + 1) }
    function tblInsColLeft()  { blockModel.tableInsertColumn(menuRow, menuCellC) }
    function tblInsColRight() { blockModel.tableInsertColumn(menuRow, menuCellC + 1) }
    function tblDelRow()      { blockModel.tableDeleteRow(menuRow, menuCellR) }
    function tblDelCol()      { blockModel.tableDeleteColumn(menuRow, menuCellC) }
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
    function tblAlign(a)      { blockModel.tableSetColAlign(menuRow, menuCellC, a) }
    function tblColKind()      { return (blockModel.contentRevision, blockModel.tableColumnKind(menuRow, menuCellC)) }
    function tblMakeChoiceCol(){ blockModel.tableSetColumnKind(menuRow, menuCellC, 1)
                                 Qt.callLater(root.openChoiceEditor, menuRow, menuCellC) }   // set options right away
    function tblMakeCheckCol() { blockModel.tableSetColumnKind(menuRow, menuCellC, 2) }
    function tblMakeTextCol()  { blockModel.tableSetColumnKind(menuRow, menuCellC, 0) }
    function tblRemoveImage() { blockModel.tableClearCellMedia(menuRow, menuCellR, menuCellC) }
    readonly property bool menuCellHasImage: blockMenu.isTable
        && blockModel.tableCellMedia(menuRow, menuCellR, menuCellC) !== ""

    // --- Clipboard (copy / cut / paste), table- and text-aware ---
    function selectedText() {
        if (!cursor.hasSel) return ""
        if (cursor.loRow === cursor.hiRow) return blockModel.contentForRow(cursor.loRow).slice(cursor.loCol, cursor.hiCol)
        var parts = [blockModel.contentForRow(cursor.loRow).slice(cursor.loCol)]
        for (var r = cursor.loRow + 1; r < cursor.hiRow; ++r) parts.push(blockModel.contentForRow(r))
        parts.push(blockModel.contentForRow(cursor.hiRow).slice(0, cursor.hiCol))
        return parts.join("\n")
    }
    function doCopy() {
        if (tcur.active) {
            var fr = cursor.focusRow
            if (tcur.rangeR0 >= 0) {
                clipboard.writeTable(blockModel.tableRangeTSV(fr, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1),
                                     blockModel.tableRangeHtml(fr, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1))
            } else {
                var t = blockModel.tableCell(fr, tcur.cr, tcur.cc)
                clipboard.writeText(tcur.pos !== tcur.anchorPos
                    ? t.slice(Math.min(tcur.pos, tcur.anchorPos), Math.max(tcur.pos, tcur.anchorPos)) : t)
            }
            return
        }
        clipboard.writeText(cursor.hasSel ? selectedText() : blockModel.contentForRow(cursor.focusRow))
    }
    function doPaste() {
        // --- Into a table cell: an image (copied file or raster) drops into the
        // focused cell; TSV → cells; else plain text (rich paste lands in the
        // document, not inside a cell). ---
        if (tcur.active) {
            var cu = clipboard.readUrls()              // copied image file (Finder/Preview)
            if (cu.length > 0 && blockModel.tableSetCellImageFromUrl(cursor.focusRow, tcur.cr, tcur.cc, cu[0])) {
                cursor.sync(); return
            }
            if (clipboard.hasImage() &&                // raster image (screenshot / Copy Image)
                blockModel.tableSetCellImageFromClipboard(cursor.focusRow, tcur.cr, tcur.cc)) {
                cursor.sync(); return
            }
            var ct = clipboard.readText()
            if (ct.length === 0) return
            if (ct.indexOf("\t") >= 0 || ct.indexOf("\n") >= 0)
                blockModel.tablePasteTSV(cursor.focusRow, tcur.cr, tcur.cc, ct)
            else tcur.type(ct)
            return
        }
        // --- Rich HTML (Word / Google Docs / Excel / web) → structured blocks:
        // headings/lists/paragraphs + bold/italic/underline/strike/links, tables
        // → Table blocks. Falls through if the HTML yields nothing usable (e.g. a
        // bare image wrapper → handled as media below). ---
        if (clipboard.hasHtml()) {
            var html = clipboard.readHtml()
            if (html && html.length > 0) {
                if (cursor.hasSel) cursor.deleteSelection()
                var hc = blockModel.pasteHtml(cursor.focusRow, cursor.focusCol, html)
                if (hc && hc.length === 2) { cursor.setCaret(hc[0], hc[1]); root.ensureVisible(hc[0]); return }
            }
        }
        // --- Copied file(s) (Finder / Preview "Copy") → import as media, exactly
        // like a drag-drop: image/video/pdf render, anything else → a file chip.
        // (Preview copies an image as a file URL, not raster bytes — without this
        // it would fall through to pasting the path as text.) ---
        var urls = clipboard.readUrls()
        if (urls.length > 0) {
            var afterRow = cursor.focusRow, anyU = false
            for (var i = 0; i < urls.length; ++i)
                if (blockModel.insertMediaFromUrl(afterRow, urls[i])) { afterRow++; anyU = true }
            if (anyU) { cursor.setCaret(afterRow, 0); root.ensureVisible(afterRow); return }
        }
        // --- Raster image on the clipboard (screenshot, "Copy Image") → media block. ---
        if (clipboard.hasImage()) {
            if (blockModel.insertImageFromClipboard(cursor.focusRow)) {
                cursor.setCaret(cursor.focusRow + 1, 0); root.ensureVisible(cursor.focusRow)
                return
            }
        }
        // --- Plain text. ---
        var txt = clipboard.readText()
        if (txt.length === 0) return
        if (cursor.hasSel) cursor.deleteSelection()
        if (root.looksTabular(txt)) {                       // rectangular TSV → table block
            var tr = blockModel.insertTableFromTSV(cursor.focusRow, txt)
            if (tr >= 0) { cursor.setCaret(tr, 0); root.ensureVisible(tr); return }
        }
        // Smart paste: blocks (blank lines separate) + per-line markdown prefixes +
        // inline **bold**/*italic*/`code`/~~strike~~/[links], all as one undo step.
        var caret = blockModel.pasteText(cursor.focusRow, cursor.focusCol, txt)
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
            if (tcur.rangeR0 >= 0) { blockModel.tableClearRange(cursor.focusRow, tcur.rangeR0, tcur.rangeC0, tcur.rangeR1, tcur.rangeC1); tcur.clearRange() }
            else if (tcur.pos !== tcur.anchorPos) tcur.delSel()
            else blockModel.tableSetCell(cursor.focusRow, tcur.cr, tcur.cc, "")
            cursor.sync()
        } else if (cursor.hasSel) cursor.deleteSelection()
    }
    function copyBlock(row) {
        if (blockModel.typeForRow(row) === 7)
            clipboard.writeTable(blockModel.tableRangeTSV(row, 0, 0, blockModel.tableRows(row) - 1, blockModel.tableColumns(row) - 1),
                                 blockModel.tableRangeHtml(row, 0, 0, blockModel.tableRows(row) - 1, blockModel.tableColumns(row) - 1))
        else clipboard.writeText(blockModel.contentForRow(row))
    }
    function deleteBlock(row) {
        if (blockModel.count > 1) {
            blockModel.removeBlock(row)
            cursor.setCaret(Math.max(0, row - (row >= blockModel.count ? 1 : 0)), 0)
        } else {
            blockModel.setContent(row, "")        // last block: clear rather than leave an empty doc
            cursor.setCaret(row, 0)
        }
        cursor.sync()
    }

    // Open the block context menu at viewport (vx,vy) for `row`. (vx,vy) is also
    // reused to anchor the language picker if "Change language…" is chosen.
    function openBlockMenu(vx, vy, row) {
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
    function openLangPopupForRow(row) {
        langPopup.targetRow = row
        langField.text = blockModel.languageForRow(row)
        langPopup.open()    // x/y are reactive bindings (root.menuX/menuY → clamped)
        langField.selectAll(); langField.forceActiveFocus()
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

    Keys.onPressed: (event) => {
        var shift = (event.modifiers & Qt.ShiftModifier) !== 0
        var cmd = (event.modifiers & Qt.ControlModifier) !== 0   // Cmd on macOS (Qt maps it)
        var k = event.key
        var inTable = tcur.active
        if (k === Qt.Key_Escape) {
            // Cancel the current op: block-drag (revert, no move) → text-drag →
            // collapse selection → disarm format toggle. In a table: collapse the
            // cell selection, else step the caret out below the table.
            if (root.blockDragging) { root.blockDragging = false; root.blockDragRow = -1; root.dropGap = -1 }
            else if (root.dragging) { root.dragging = false }
            else if (root.boardMode && root.activeTableRow >= 0) { root.boardMode = false }   // board → grid
            else if (inTable) { if (tcur.pos !== tcur.anchorPos) { tcur.anchorPos = tcur.pos; cursor.sync() } else root.exitTable(1) }
            else if (cursor.hasSel) { cursor.setCaret(cursor.focusRow, cursor.focusCol) }
            else if (cursor.activeMarks !== 0) { cursor.clearMarks() }
            event.accepted = true
        }
        else if (cmd && k === Qt.Key_Z && shift) { blockModel.redo(); event.accepted = true }
        else if (cmd && k === Qt.Key_Z) { blockModel.undo(); event.accepted = true }
        else if (cmd && k === Qt.Key_Y) { blockModel.redo(); event.accepted = true }
        else if (cmd && k === Qt.Key_C) { root.doCopy(); event.accepted = true }
        else if (cmd && k === Qt.Key_V) { root.doPaste(); event.accepted = true }
        else if (cmd && !shift && k === Qt.Key_X) { root.doCut(); event.accepted = true }
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
        else if (inTable) {
            if (cmd) { event.accepted = true; return }   // swallow other Cmd-combos (don't type the letter)
            if (tcur.rangeR0 >= 0) tcur.clearRange()   // any key collapses a cell-range selection
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
        else if (cmd && k === Qt.Key_B) { applyFormat("bold"); event.accepted = true }
        else if (cmd && k === Qt.Key_I) { applyFormat("italic"); event.accepted = true }
        else if (cmd && k === Qt.Key_U) { applyFormat("underline"); event.accepted = true }
        else if (cmd && shift && k === Qt.Key_X) { applyFormat("strike"); event.accepted = true }
        else if (cmd && k === Qt.Key_Backslash) { clearFormatting(); event.accepted = true }
        else if (cmd && k === Qt.Key_K) { applyLink(); event.accepted = true }
        else if (k === Qt.Key_Right) { navRight(shift); event.accepted = true }
        else if (k === Qt.Key_Left) { navLeft(shift); event.accepted = true }
        else if (k === Qt.Key_Down) { navDown(shift); event.accepted = true }
        else if (k === Qt.Key_Up) { navUp(shift); event.accepted = true }
        else if (k === Qt.Key_Backspace) { cursor.backspace(); event.accepted = true }
        else if (k === Qt.Key_Delete) { cursor.forwardDelete(); event.accepted = true }
        else if (k === Qt.Key_Return || k === Qt.Key_Enter) { cursor.splitLine(); event.accepted = true }
        else if (event.text.length === 1 && event.text >= " ") { cursor.insertChar(event.text); event.accepted = true }
    }

    Timer { interval: 530; running: true; repeat: true; onTriggered: root.caretOn = !root.caretOn }

    // --- HUD telemetry (same surface as the other arms) ---
    readonly property int firstVisible: blockModel.rowForY(flick.contentY)
    readonly property int lastVisible: Math.min(blockModel.count - 1,
                                       blockModel.rowForY(flick.contentY + flick.height - 1))
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
        }
    }

    readonly property int firstRow: Math.max(0, firstVisible - overscan)
    readonly property int poolSize: Math.min(blockModel.count,
                                    Math.ceil(root.height / 38) + 2 * overscan + 4)
    readonly property int delegateCount: poolSize
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
        visible: root.activeTableRow < 0 && root.activePdfRow < 0   // hidden in a table/PDF tab
        anchors.fill: parent
        contentWidth: width
        contentHeight: blockModel.totalHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Connections {
            target: blockModel
            function onHeightSettled(row, delta) { if (row < root.firstVisible) flick.contentY += delta }
        }

        Repeater {
            id: pool
            model: root.poolSize
            delegate: Item {
                id: cell
                required property int index
                readonly property int logicalRow: root.firstRow
                    + (((index - root.firstRow) % root.poolSize) + root.poolSize) % root.poolSize
                readonly property bool active: logicalRow >= 0 && logicalRow < blockModel.count
                // contentRevision covers row-shifts (insert/remove/move all bump it,
                // reactivity rule 2) so a recycled delegate can't mis-render. NOT
                // layoutRevision: isMedia drives this delegate's height, whose settle
                // bumps layoutRevision — depending on it here forms a binding loop
                // (same trap the `measure` binding below documents).
                readonly property bool isMedia: active
                    && (blockModel.contentRevision, blockModel.typeForRow(logicalRow)) === 3
                readonly property bool isVideoMedia: isMedia
                    && (blockModel.contentRevision, blockModel.mediaKind(logicalRow)) === "video"
                readonly property bool isPdfMedia: isMedia
                    && (blockModel.contentRevision, blockModel.mediaKind(logicalRow)) === "pdf"
                readonly property bool isFocus: active && logicalRow === cursor.focusRow
                readonly property bool inSel: active && logicalRow >= cursor.loRow && logicalRow <= cursor.hiRow
                readonly property Item teItem: te    // layout oracle, for hit-testing
                readonly property Item tableItem: tableHost   // BlockTable, for table hit-testing
                // Horizontal measure for this block by type (page vs text bound).
                // Keyed off te.btype (already reactive) — NOT the layout revision,
                // which the measured height bumps and would form a binding loop.
                readonly property real measure: root.measureForType(te.btype)

                width: flick.width
                visible: active
                y: (blockModel.layoutRevision, active ? blockModel.yForRow(logicalRow) : 0)
                // Code blocks get double vertical padding (24 vs 12) so the
                // syntax-themed background has breathing room above/below.
                // Every video block reserves room for its always-on transport
                // toolbar below the frame. The media frame height is the MODEL's
                // authoritative value (mediaDisplayHeight — dims + content width),
                // so the delegate exactly matches the Fenwick reservation: no
                // transient, no scroll-in jump. layoutRevision dep picks up resize.
                height: isMedia      ? 12 + (blockModel.layoutRevision, blockModel.contentRevision,
                                             blockModel.mediaDisplayHeight(logicalRow))
                                       + (isVideoMedia ? root.videoTransportH
                                                       : isPdfMedia ? root.pdfNavH : 0)
                      : te.btype === 6 ? 12 + 18                       // divider
                      : te.btype === 7 ? 26 + tableHost.implicitHeight // table: 6 top + 20 bottom (clears the +row button, which sits 2px below the table and is 14px tall)
                      : (te.btype === 2 ? 24 : 12) + te.height   // te.height = lineCount*lineH (even)

                // Media is known-geometry: the MODEL derives its height from the
                // probed dims + the content width (setContentWidth). The delegate
                // renders to that authoritative value and never measures back —
                // measuring the child's transient (a 0.5-ratio fallback while
                // implicitHeight lags logicalRow on recycle) would push a wrong
                // height to the Fenwick → contentY/firstVisible churn and a
                // scroll-in jump. Text/code/table still measure (reflow is unknown).
                function reportHeight() {
                    if (!active || isMedia) return
                    // Tables: measure ONCE, then reuse the model's cache. A table
                    // can't be estimated from data (cell wrapping), but its height
                    // is stable once known — and on recycle the delegate briefly
                    // reports a near-empty implicitHeight before its rows populate.
                    // Measuring that (or re-measuring a settled table) collapses then
                    // restores the Fenwick height, jumping the view (esp. scrolling
                    // UP into the table). So skip the empty transient, and once the
                    // row is cached don't re-measure it.
                    if (te.btype === 7 && (tableHost.implicitHeight < 40 || blockModel.rowMeasured(logicalRow)))
                        return
                    blockModel.setMeasuredHeight(logicalRow, height)
                }
                // Measure-back is DEFERRED (coalesced) via a 0-interval timer: on
                // recycle, te's font/btype/text settle across several onHeightChanged
                // ticks within the frame (e.g. a heading briefly measures at the body
                // font → ~28px, then settles → ~49px). Reporting each tick pushes the
                // transient to the Fenwick → height jitter (inconsistent heights, and
                // a moved block's preserved height gets clobbered by the transient).
                // restart() collapses the ticks into ONE report of the SETTLED height.
                // Timer is a delegate child, so (unlike Qt.callLater) it can't fire
                // after the delegate is torn down. Tables/media still handled in
                // reportHeight (cache / skip).
                // Tables settle their auto-column-widths + text wrapping a frame or
                // two AFTER the first layout (and again when the custom fonts finish
                // loading), so a 0-interval coalesce fires on a TALL transient and the
                // measure-once cache locks it (→ a too-tall table that snaps shorter on
                // first edit). Debounce tables: restart()-on-each-tick waits for the
                // layout to go quiet, then caches the SETTLED height. Other blocks
                // reflow synchronously, so they keep the instant 0-interval.
                Timer { id: measureTimer; interval: te.btype === 7 ? 150 : 0; repeat: false; onTriggered: cell.reportHeight() }
                onHeightChanged: if (active && !isMedia) measureTimer.restart()
                // Re-measure on RECYCLE too, not just on height change: blocks of a
                // type now render at an identical height (the line-height fix), so a
                // delegate recycling between two same-height blocks fires no
                // onHeightChanged — the new row would never get measured and would
                // keep its (taller) estimate in the Fenwick → a gap below it. Keying
                // off logicalRow ensures every block the delegate shows is measured.
                onLogicalRowChanged: if (active && !isMedia) measureTimer.restart()
                onIsFocusChanged: if (isFocus) root.focusBlockItem = te
                Component.onCompleted: {
                    if (active && !isMedia) measureTimer.restart()
                    if (isFocus) root.focusBlockItem = te
                }

                // inline-code chips — overlay rects (one per visual line of each
                // code range), drawn BELOW the selection so selecting code shows
                // the highlight, and below the glyphs. NOT a char-format
                // background (that paints inside the TextEdit, above selection).
                property var codeRects: {
                    var dep = blockModel.contentRevision + blockModel.layoutRevision
                    if (!cell.active || cell.isMedia) return []
                    var ranges = blockModel.codeRangesForRow(cell.logicalRow)
                    var out = []
                    for (var i = 0; i < ranges.length; ++i) {
                        var rs = root.selectionRects(te, ranges[i].s, ranges[i].e)
                        for (var j = 0; j < rs.length; ++j) out.push(rs[j])
                    }
                    return out
                }
                Repeater {
                    model: cell.codeRects
                    delegate: Rectangle {
                        required property int index
                        readonly property rect rr: cell.codeRects[index]
                        color: Theme.colors.inlineCodeBg
                        radius: 3
                        z: 0
                        x: te.x + rr.x - 2
                        y: te.y + rr.y
                        width: rr.width + 4
                        height: rr.height
                    }
                }

                // selection highlight (behind text), one rect per visual line.
                property var selRects: {
                    var dep = blockModel.contentRevision + blockModel.layoutRevision   // re-eval triggers
                    if (!cell.inSel || cell.isMedia) return []
                    var sp = (cell.logicalRow === cursor.loRow) ? Math.min(cursor.loCol, te.length) : 0
                    var ep = (cell.logicalRow === cursor.hiRow) ? Math.min(cursor.hiCol, te.length) : te.length
                    return root.selectionRects(te, sp, ep)
                }
                Repeater {
                    model: cell.selRects
                    delegate: Rectangle {
                        required property int index
                        readonly property rect rr: cell.selRects[index]
                        color: Theme.colors.selectionBg
                        z: 0
                        x: te.x + rr.x
                        y: te.y + rr.y
                        width: Math.max(2, rr.width)
                        height: rr.height
                    }
                }

                MediaBlock {  // image + video (poster = decoded frame thumbnail)
                    id: mediaHost
                    visible: cell.active && cell.isMedia
                    active: cell.active && cell.isMedia
                    logicalRow: cell.logicalRow
                    x: cell.colLeft; y: 6
                    // pageWidth directly (NOT cell.measure → te.btype → layoutRevision):
                    // the cell height reads mediaHost.implicitHeight, and height bumps
                    // layoutRevision, so a te.btype dependency here is a latent loop the
                    // async poster decode wakes up. BlockTable sidesteps it the same way.
                    maxWidth: root.pageWidth
                    width: implicitWidth
                    // Frame height = the model's authoritative value (same as the
                    // cell reservation), so the rendered media never disagrees with
                    // the reserved space.
                    height: (blockModel.layoutRevision, blockModel.contentRevision,
                             blockModel.mediaDisplayHeight(cell.logicalRow))
                    // Poster frame: the remembered playhead (0 until first play).
                    posterFrame: cell.isVideoMedia
                        ? (root.videoPlayheadRev, root.videoPlayheadFor(cell.logicalRow)) : 0
                    pdfPage: cell.isPdfMedia
                        ? (root.pdfPageRev, root.pdfPageFor(cell.logicalRow)) : 0
                    // Stay the poster (correct frame) until the live surface is
                    // ready — avoids a flash of the previous video's stale frame.
                    isActivePlayer: cell.logicalRow === root.videoPlayingRow && root._videoSurfaceReady
                }

                // Media is opaque (no caret, no text selection rects), so its
                // selected state needs its own affordance: a translucent accent
                // tint over the frame when it's the focus (clicked) or inside a
                // multi-block range. (During playback the live surface covers it.)
                Rectangle {
                    visible: cell.active && cell.isMedia && (cell.isFocus || cell.inSel)
                    x: mediaHost.x; y: mediaHost.y
                    width: mediaHost.width; height: mediaHost.height
                    radius: Theme.dim.radius
                    z: 2
                    color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g,
                                   Theme.colors.accent.b, 0.22)
                }

                BlockTable {  // table block — passive grid (interaction lands in later phases)
                    id: tableHost
                    visible: cell.active && te.btype === 7
                    logicalRow: cell.logicalRow
                    active: cell.active && te.btype === 7
                    // Left-aligned at the shared left edge, up to the page measure;
                    // wider content scrolls inside.
                    maxWidth: root.pageWidth
                    width: implicitWidth
                    x: cell.colLeft; y: 6
                    height: implicitHeight   // a bare Item won't adopt implicitHeight itself
                    // Focus / in-cell caret + selection (driven by the table sub-cursor).
                    focused: cell.isFocus && te.btype === 7
                    caretOn: root.caretOn
                    focusR: tcur.cr; focusC: tcur.cc
                    caretPos: tcur.pos
                    selFrom: Math.min(tcur.pos, tcur.anchorPos)
                    selTo: Math.max(tcur.pos, tcur.anchorPos)
                    rangeR0: focused ? tcur.rangeR0 : -1
                    rangeC0: tcur.rangeC0; rangeR1: tcur.rangeR1; rangeC1: tcur.rangeC1
                    // live column-resize preview for this table
                    resizeCol: (root.tableResizing && root.resizeRow === cell.logicalRow) ? root.resizeColIdx : -1
                    resizeW: root.resizeW
                    // Drag-drop target cell (image dragged over this table).
                    dropR: root.dropTableRow === cell.logicalRow ? root.dropCellR : -1
                    dropC: root.dropTableRow === cell.logicalRow ? root.dropCellC : -1
                    // Context-menu column/row target highlight (this is the menu's table).
                    hiScope: (cell.logicalRow === root.menuRow
                              && (root.menuHiScope === "column" || root.menuHiScope === "row")) ? root.menuHiScope : ""
                    hiIndex: root.menuHiScope === "column" ? root.menuCellC : root.menuCellR
                    hiDanger: root.menuHiDanger
                }

                Rectangle {  // code background — matches the syntax theme's fill
                    visible: cell.active && !cell.isMedia && te.btype === 2
                    anchors.fill: te; anchors.margins: -8
                    z: -1   // behind the selection highlight (else it hides the selection)
                    color: codeHl.backgroundColor.a > 0 ? codeHl.backgroundColor : Theme.colors.codeBg
                    radius: Theme.dim.radius
                    border.width: 1; border.color: Theme.colors.border
                }

                readonly property real colLeft: root.leftEdge   // shared left edge for all blocks

                TextEdit {
                    id: te
                    visible: !cell.isMedia && btype !== 6 && btype !== 7   // hidden for divider/table
                    readOnly: true
                    activeFocusOnPress: false
                    selectByMouse: false
                    // quote/list get a left indent; the decoration sits in it.
                    readonly property real deco: (btype === 4 || btype === 5 || btype === 8) ? 22 : 0
                    // task items (type 8): tri-state status 0 todo / 1 doing / 2 done
                    readonly property int taskState: (blockModel.contentRevision,
                                                      cell.active && btype === 8 ? blockModel.taskStateForRow(cell.logicalRow) : 0)
                    x: cell.colLeft + deco
                    width: cell.measure - deco
                    y: btype === 2 ? 12 : 6   // code: centered in the taller (doubled-margin) cell
                    // Quotes are upright Merriweather (serif + bar + muted colour
                    // mark them); italic/bold come from spans so all four faces
                    // are reachable, rather than forcing the whole block italic.
                    text: (blockModel.contentRevision, cell.active ? blockModel.contentForRow(cell.logicalRow) : "")
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.PlainText
                    // Revision dep: re-evaluate type when autoformat changes a block
                    // in place or a row-shift remaps this delegate — both bump
                    // contentRevision (rule 2). NOT layoutRevision: btype feeds
                    // font.pixelSize -> te.implicitHeight -> cell height, and height
                    // bumps layoutRevision, so depending on it is a latent loop (dormant
                    // until the async poster decode forces a relayout). Same trap the
                    // isMedia/maxWidth bindings document.
                    readonly property int btype: (blockModel.contentRevision,
                                                  cell.active ? blockModel.typeForRow(cell.logicalRow) : 0)
                    readonly property var headingSizes: [26, 30, 26, 22, 19, 17, 16]   // index by level (1–6)
                    color: btype === 1 ? Theme.colors.textBright
                         : btype === 2 ? Theme.colors.codeText
                         : btype === 4 ? Theme.colors.textMuted   // quote
                         : (btype === 8 && taskState === 2) ? Theme.colors.textMuted   // done task
                         : Theme.colors.text
                    font.family: btype === 2 ? Theme.font.mono
                               : btype === 4 ? Theme.font.serif   // quote → Merriweather
                               : Theme.font.family
                    font.pixelSize: {
                        var _ = blockModel.layoutRevision + blockModel.contentRevision   // deps
                        if (btype === 2) return Theme.font.sizeMono
                        if (btype !== 1 || !cell.active) return Theme.font.sizeBody
                        return headingSizes[Math.max(1, Math.min(6, blockModel.levelForRow(cell.logicalRow)))]
                    }
                    font.bold: btype === 1
                    font.strikeout: btype === 8 && taskState === 2   // done task
                    // Deterministic line height: TextEdit's natural single-line
                    // implicitHeight rounds to 19 OR 20px for the same body text (a Qt
                    // text-layout quirk), so same-type blocks came out 1px uneven.
                    // TextEdit has no lineHeight property, so pin the item height to
                    // lineCount * a normalized per-font line height (>= natural, so no
                    // clipping); the cell reserves from this, making every block of a
                    // given type identical. (selection/caret still use the real text
                    // layout, so they track the glyphs.)
                    readonly property int lineH: Math.round(font.pixelSize * 1.35)
                    height: Math.max(1, lineCount) * lineH
                }

                // Inline markdown styling: applies bold/italic/mono char formats
                // to te's PlainText document and dims the markers in place. No
                // HTML, identity caret positions. Off for code (markdown is
                // literal inside a fence) and non-text blocks.
                InlineMarkdownHighlighter {
                    // Attach ONLY for text blocks; a document can have one
                    // highlighter, so code blocks detach this and use codeHl.
                    document: (te.btype === 0 || te.btype === 1 || te.btype === 4 || te.btype === 5 || te.btype === 8) ? te.textDocument : null
                    enabled: cell.active && (te.btype === 0 || te.btype === 1 || te.btype === 4 || te.btype === 5 || te.btype === 8)
                    markerColor: Theme.colors.accent
                    selectedMarkerColor: Theme.colors.textBright
                    codeColor: Theme.colors.inlineCodeText
                    linkColor: Theme.colors.accent
                    codeFontFamily: Theme.font.mono
                    // NOTE: selection does NOT drive marker recolouring here.
                    // Binding selStart/selEnd to the selection re-highlights the
                    // block on every selection change, which re-lays-out it mid-
                    // frame and corrupts positionToRectangle → the selection rect
                    // renders only part of the word. Markers stay accent-blue when
                    // selected (markers only appear while typing, rarely selected).
                    // semantic format spans (clean bold/italic/mono, no markers)
                    spans: (blockModel.contentRevision, blockModel.spansForRow(cell.logicalRow))
                }

                // Code-block syntax colouring (KSyntaxHighlighting). Attaches only
                // for code blocks; `backgroundColor` is the theme's editor fill so
                // the block background matches the token colours.
                CodeHighlighter {
                    id: codeHl
                    document: te.btype === 2 ? te.textDocument : null
                    language: te.btype === 2 ? (blockModel.contentRevision, blockModel.languageForRow(cell.logicalRow)) : ""
                }

                Rectangle {  // caret
                    visible: cursor.active && cell.isFocus && root.caretOn && !cursor.hasSel && !cell.isMedia && te.btype !== 6 && te.btype !== 7
                    color: Theme.colors.accent
                    width: 2
                    property rect cr: te.positionToRectangle(Math.min(cursor.focusCol, te.length))
                    x: te.x + cr.x
                    y: te.y + cr.y
                    height: cr.height > 0 ? cr.height : 18
                    z: 2
                }

                Rectangle {  // quote: left bar
                    visible: cell.active && te.btype === 4
                    x: cell.colLeft + 4; y: te.y
                    width: 3; height: te.implicitHeight
                    radius: 1; color: Theme.colors.quoteBar
                }
                Text {  // list: bullet
                    visible: cell.active && te.btype === 5
                    x: cell.colLeft + 6; y: te.y
                    text: "•"; color: Theme.colors.textMuted; font.pixelSize: Theme.font.sizeBody
                }
                Item {  // task: tri-state checkbox (0 todo / 1 doing / 2 done)
                    visible: cell.active && te.btype === 8
                    x: cell.colLeft + 2; y: te.y + Math.round((te.lineH - 14) / 2)
                    width: 14; height: 14
                    Rectangle {
                        anchors.fill: parent
                        radius: 3
                        color: te.taskState === 2 ? Theme.colors.accent : "transparent"
                        border.width: te.taskState === 2 ? 0 : 1.5
                        border.color: te.taskState === 1 ? Theme.colors.accent : Theme.colors.textMuted
                    }
                    Rectangle {  // in-progress: centred dash
                        visible: te.taskState === 1
                        anchors.centerIn: parent
                        width: 7; height: 2; radius: 1
                        color: Theme.colors.accent
                    }
                    Text {  // done: check mark
                        visible: te.taskState === 2
                        anchors.centerIn: parent
                        text: "✓"
                        color: Theme.colors.textBright
                        font.pixelSize: 11; font.bold: true
                    }
                }
                Rectangle {  // divider: horizontal rule
                    visible: cell.active && te.btype === 6
                    x: cell.colLeft; y: cell.height / 2 - 1
                    width: cell.measure; height: 1
                    color: Theme.colors.divider
                }

                // drag-reorder grip (left gutter). Lit on row hover or while this
                // block is the one being dragged. Press starts the drag (handled
                // by the persistent `mouse` area via the gutter zone).
                Icon {
                    visible: cell.active && !cell.isMedia
                    name: "dots-six-vertical"
                    size: Theme.icon.sizeToolbar
                    x: cell.colLeft - 26; y: te.y
                    color: Theme.colors.textMuted
                    opacity: (root.blockDragRow === cell.logicalRow) ? 1.0
                           : (root.hoverRow === cell.logicalRow ? 0.7 : 0.0)
                    Behavior on opacity { NumberAnimation { duration: 90 } }
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
            property bool overGrip: false
            property bool overClickable: false   // over a task checkbox / table check or choice cell
            cursorShape: root.blockDragging ? Qt.ClosedHandCursor
                       : (root.tableResizing || root.tableOverBorder) ? Qt.SplitHCursor
                       : overClickable ? Qt.PointingHandCursor
                       : (overGrip ? Qt.OpenHandCursor : Qt.IBeamCursor)

            onPressed: (m) => {
                root.forceActiveFocus()
                // Right-click anywhere on a block → its context menu (capturing the
                // cell when over a table, for the row/column ops).
                if (m.button === Qt.RightButton) {
                    var trow = blockModel.rowForY(m.y)
                    root.menuLinkUrl = ""
                    if (blockModel.typeForRow(trow) === 7) {
                        var th = root.tableHitAt(m.x, m.y)
                        root.menuCellR = th ? th.r : 0; root.menuCellC = th ? th.c : 0
                    } else {
                        var rh = root.hitTest(m.x, m.y)              // link under the click?
                        root.menuLinkUrl = blockModel.linkAt(rh.row, rh.col)
                    }
                    root.openBlockMenu(m.x, m.y - flick.contentY, trow)
                    return
                }
                // Press in the grip gutter → start a block drag-reorder.
                if (root.inGutter(m.x) && !(m.modifiers & Qt.ShiftModifier)) {
                    root.blockDragRow = blockModel.rowForY(m.y)
                    root.blockDragText = blockModel.contentForRow(root.blockDragRow).split("\n")[0]
                    root.blockDragViewY = m.y - flick.contentY
                    root.dropGap = root.gapForY(m.y)
                    root.blockDragging = true
                    return
                }
                cursor.resetGoalX(); cursor.clearMarks()
                // Click into a table cell → place the table caret; arm drag for
                // in-cell text selection / cross-cell range.
                var th = root.tableHitAt(m.x, m.y)
                if (th) {
                    // Typed body cells: choice → option picker; check → cycle state.
                    var tk = blockModel.tableColumnKind(th.row, th.c)
                    var tbody = th.r >= blockModel.tableHeaderRows(th.row)
                    if (tk === 1 && tbody) { root.openChoicePicker(th.row, th.r, th.c, m.x, m.y - flick.contentY); return }
                    if (tk === 2 && tbody) { blockModel.tableCycleCellCheck(th.row, th.r, th.c); return }
                    var dcell = root.cellForRow(th.row), bt = dcell ? dcell.tableItem : null
                    if (bt) { var lp = bt.mapFromItem(mouse, m.x, m.y); root.beginTableInteraction(bt, th.row, lp.x, lp.y) }
                    return
                }
                // Click a task-item checkbox → cycle its status (todo→doing→done).
                var tcb = root.taskCheckboxAt(m.x, m.y)
                if (tcb >= 0) { blockModel.toggleTask(tcb); return }
                var h = root.hitTest(m.x, m.y)
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
                if (root.blockDragging) {
                    root.blockDragViewY = m.y - flick.contentY
                    root.dropGap = root.gapForY(m.y)
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
                // hover (not pressed): light the row's grip; gutter → grab cursor;
                // near a table column border → resize cursor.
                root.hoverRow = blockModel.rowForY(m.y)
                mouse.overGrip = root.inGutter(m.x)
                var overBorder = false
                if (blockModel.typeForRow(root.hoverRow) === 7) {
                    var hd = root.cellForRow(root.hoverRow), hbt = hd ? hd.tableItem : null
                    if (hbt) { var hlp = hbt.mapFromItem(mouse, m.x, m.y); overBorder = hbt.columnBorderAt(hlp.x) >= 0 }
                }
                root.tableOverBorder = overBorder
                // Over an interactive widget (block task checkbox, or a table check /
                // choice body cell) → a pointing-hand cursor instead of the I-beam.
                var clk = root.taskCheckboxAt(m.x, m.y) >= 0
                if (!clk && !overBorder && blockModel.typeForRow(root.hoverRow) === 7) {
                    var ch = root.tableHitAt(m.x, m.y)
                    if (ch && ch.r >= blockModel.tableHeaderRows(ch.row)) {
                        var ck = blockModel.tableColumnKind(ch.row, ch.c)
                        clk = (ck === 1 || ck === 2)
                    }
                }
                mouse.overClickable = clk
                // Hovering an image row → show its resize handles. Don't clear on a
                // non-image *handle* hover (the central layer onExits then); only a
                // different block hides them.
                if (root._isImageRow(root.hoverRow)) root.imgHandleRow = root.hoverRow
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
                        root.hoverLinkX = m.x; root.hoverLinkViewY = m.y - flick.contentY
                    }
                    linkTipHide.stop()
                } else if (root.hoverLinkUrl.length > 0) {
                    linkTipHide.restart()
                }
            }
            onExited: { root.hoverRow = -1; mouse.overGrip = false; root.tableOverBorder = false
                        if (root.hoverLinkUrl.length > 0) linkTipHide.restart() }
            onReleased: {
                if (root.blockDragging) root.commitBlockDrag()
                else if (root.tableResizing || root.tableDragging) root.endTableInteraction()
                else root.dragging = false
            }
            onCanceled: {
                if (root.blockDragging) { root.blockDragging = false; root.blockDragRow = -1; root.dropGap = -1 }
                else { root.dragging = false; root.tableDragging = false; root.tableResizing = false }
            }
            onDoubleClicked: (m) => {
                // End the press-drag the 2nd press armed, so a tiny mouse jitter
                // before release can't re-extend the selection back to the click
                // point (which collapsed the word to word-start→cursor).
                root.dragging = false; root.tableResizing = false
                // Double-click a file-attachment chip → reveal it in Finder/Explorer.
                var mrow = blockModel.rowForY(m.y)
                if (blockModel.typeForRow(mrow) === 3 && blockModel.mediaKind(mrow) === "file") {
                    blockModel.revealMedia(mrow); return
                }
                // Double-click a table column border → reset that column to auto.
                var drow = blockModel.rowForY(m.y)
                if (blockModel.typeForRow(drow) === 7) {
                    var dd = root.cellForRow(drow), dbt = dd ? dd.tableItem : null
                    if (dbt) {
                        var dlp = dbt.mapFromItem(mouse, m.x, m.y)
                        var dbc = dbt.columnBorderAt(dlp.x)
                        if (dbc >= 0) { blockModel.tableSetColWidth(drow, dbc, 0); return }
                    }
                    return   // don't word-select inside a table
                }
                // select the word under the cursor
                var h = root.hitTest(m.x, m.y)
                var t = blockModel.contentForRow(h.row)
                var s = h.col, e = h.col
                while (s > 0 && /\w/.test(t.charAt(s - 1))) s--
                while (e < t.length && /\w/.test(t.charAt(e))) e++
                cursor.setCaret(h.row, s); cursor.move(h.row, e, true)
            }
        }

        // Edge auto-scroll while drag-selecting OR drag-reordering near the
        // top/bottom (the persistent `mouse` area keeps its grab through scroll).
        Timer {
            interval: 16; repeat: true; running: root.dragging || root.blockDragging
            onTriggered: {
                var margin = 44, sp = 0
                var viewY = root.blockDragging ? root.blockDragViewY : root.dragViewY
                if (viewY < margin) sp = -Math.max(6, margin - viewY)
                else if (viewY > flick.height - margin) sp = Math.max(6, viewY - (flick.height - margin))
                if (sp === 0) return
                flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + sp))
                var cy = viewY + flick.contentY               // content point under the held cursor
                if (root.blockDragging) { root.dropGap = root.gapForY(cy); return }
                var h = root.hitTest(root.dragX, cy)
                cursor.move(h.row, h.col, true)
            }
        }

        ScrollBar.vertical: ScrollBar {
            id: vbar
            policy: ScrollBar.AsNeeded
            width: Theme.dim.scrollBarWidth
            contentItem: Rectangle {
                radius: width / 2
                color: Theme.colors.textSubtle
                opacity: vbar.pressed ? 0.85 : (vbar.hovered ? 0.65 : 0.40)
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
        }
    }

    // --- Full-frame table view (the active table tab). Fills the editor; vertical
    // scroll for tall tables, the BlockTable handles horizontal internally. Reuses
    // the same tcur edit model (the cursor is pinned to this table row). ---
    Flickable {
        id: tableFrame
        visible: root.activeTableRow >= 0 && !root.boardMode
        anchors.fill: parent
        anchors.topMargin: Theme.dim.toolStripHeight   // room for the tab toolbar
        contentWidth: width
        contentHeight: frameTable.implicitHeight + 70   // room for the scrollbar + row button
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
            if (dropGap >= 0 && dragFrom >= 0) {
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
            dragFrom = -1; dropGap = -1; gripC = -1; gripR = -1
        }
        ScrollBar.vertical: ScrollBar {
            id: fvbar
            policy: ScrollBar.AsNeeded; width: Theme.dim.scrollBarWidth
            contentItem: Rectangle {
                radius: width / 2; color: Theme.colors.textSubtle
                opacity: fvbar.pressed ? 0.85 : (fvbar.hovered ? 0.65 : 0.40)
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
        }
        BlockTable {
            id: frameTable
            active: root.activeTableRow >= 0
            logicalRow: root.activeTableRow
            x: 20; y: 20
            maxWidth: tableFrame.width - 40        // full editor width
            width: implicitWidth
            height: implicitHeight
            focused: root.activeTableRow >= 0
            caretOn: root.caretOn
            focusR: tcur.cr; focusC: tcur.cc
            caretPos: tcur.pos
            selFrom: Math.min(tcur.pos, tcur.anchorPos)
            selTo: Math.max(tcur.pos, tcur.anchorPos)
            rangeR0: tcur.rangeR0; rangeC0: tcur.rangeC0; rangeR1: tcur.rangeR1; rangeC1: tcur.rangeC1
            resizeCol: (root.tableResizing && root.resizeRow === root.activeTableRow) ? root.resizeColIdx : -1
            resizeW: root.resizeW
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
                    var p = frameMA.mapToItem(root, m.x, m.y)
                    root.openBlockMenu(p.x, p.y, root.activeTableRow)
                    return
                }
                // Typed body cells behave the same as the inline view: choice → the
                // option picker; check → cycle the tri-state. Both go through the same
                // mutateTable-backed invokables, so undo is identical here.
                var fhit = frameTable.cellAtPoint(m.x, m.y)
                var ftk = blockModel.tableColumnKind(root.activeTableRow, fhit.c)
                var fbody = fhit.r >= blockModel.tableHeaderRows(root.activeTableRow)
                if (ftk === 1 && fbody) {
                    var fp = frameMA.mapToItem(root, m.x, m.y)
                    root.openChoicePicker(root.activeTableRow, fhit.r, fhit.c, fp.x, fp.y)
                    return
                }
                if (ftk === 2 && fbody) { blockModel.tableCycleCellCheck(root.activeTableRow, fhit.r, fhit.c); return }
                root.beginTableInteraction(frameTable, root.activeTableRow, m.x, m.y)
            }
            onPositionChanged: (m) => {
                if (root.tableResizing || root.tableDragging) { root.updateTableInteraction(frameTable, m.x, m.y); return }
                var overBorder = frameTable.columnBorderAt(m.x) >= 0
                root.tableOverBorder = overBorder
                // Pointer cursor over a check / choice body cell.
                var clk = false
                if (!overBorder) {
                    var fh = frameTable.cellAtPoint(m.x, m.y)
                    if (fh && fh.r >= blockModel.tableHeaderRows(root.activeTableRow)) {
                        var fk = blockModel.tableColumnKind(root.activeTableRow, fh.c)
                        clk = (fk === 1 || fk === 2)
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

        // Full-frame affordances: +column (right), horizontal scrollbar + +row
        // (bottom). Direct children — no mouse-layer conflict in this view.
        Rectangle {   // + column
            x: 20 + frameTable.width + 2; y: 20
            width: 14; height: frameTable.height; radius: 3
            color: fAddColMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.surfaceHover
            border.width: 1; border.color: Theme.colors.border
            Text { anchors.centerIn: parent; text: "+"; color: Theme.colors.textMuted; font.pixelSize: 13 }
            MouseArea { id: fAddColMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: blockModel.tableInsertColumn(root.activeTableRow, blockModel.tableColumns(root.activeTableRow)) }
        }
        Rectangle {   // horizontal scrollbar (when overflowing)
            visible: frameTable.overflowing
            x: 20; y: 20 + frameTable.height + 2
            width: frameTable.width; height: Theme.dim.scrollBarWidth; color: "transparent"
            readonly property real maxScroll: Math.max(0, frameTable.contentW - frameTable.width)
            readonly property real thumbW: Math.max(24, width * frameTable.width / Math.max(1, frameTable.contentW))
            readonly property real maxThumbX: width - thumbW
            Rectangle {
                height: parent.height; radius: height / 2; width: parent.thumbW
                x: parent.maxScroll > 0 ? (frameTable.scrollX / parent.maxScroll) * parent.maxThumbX : 0
                color: Theme.colors.textSubtle
                opacity: fHbarMA.pressed ? 0.85 : (fHbarMA.containsMouse ? 0.65 : 0.45)
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
            MouseArea {
                id: fHbarMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                function setScroll(mx) {
                    var tx = Math.max(0, Math.min(parent.maxThumbX, mx - parent.thumbW / 2))
                    if (parent.maxThumbX > 0) frameTable.scrollX = (tx / parent.maxThumbX) * parent.maxScroll
                }
                onPressed: (m) => setScroll(m.x)
                onPositionChanged: (m) => { if (pressed) setScroll(m.x) }
            }
        }
        Rectangle {   // + row (below the scrollbar when present)
            x: 20; y: 20 + frameTable.height + (frameTable.overflowing ? Theme.dim.scrollBarWidth + 6 : 2)
            width: frameTable.width; height: 14; radius: 3
            color: fAddRowMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.surfaceHover
            border.width: 1; border.color: Theme.colors.border
            Text { anchors.centerIn: parent; text: "+"; color: Theme.colors.textMuted; font.pixelSize: 13 }
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
                height: 8; y: 3; radius: 4
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
                    if (tableFrame.colDragging) { tableFrame.dropGap = tableFrame.colGapAt(m.x); return }
                    tableFrame.gripC = tableFrame.colAt(m.x)
                }
                onPressed: (m) => {
                    var c = tableFrame.colAt(m.x)
                    if (c >= 0) { tableFrame.colDragging = true; tableFrame.dragFrom = c
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
                width: 8; x: 3; radius: 4
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
                    if (tableFrame.rowDragging) { tableFrame.dropGap = tableFrame.rowGapAt(m.y); return }
                    tableFrame.gripR = tableFrame.rowAt(m.y)
                }
                onPressed: (m) => {
                    var r = tableFrame.rowAt(m.y)
                    if (r >= 0) { tableFrame.rowDragging = true; tableFrame.dragFrom = r
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
            radius: 1; color: Theme.colors.accent; z: 10
        }
        Rectangle {   // row drop line
            visible: tableFrame.rowDragging && tableFrame.dropGap >= 0
            x: 20; y: 20 + frameTable.rowTopY(tableFrame.dropGap) - 1
            width: frameTable.width; height: 3
            radius: 1; color: Theme.colors.accent; z: 10
        }
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
        BlockKanban {
            id: boardView
            x: 20; y: 20
            width: implicitWidth; height: implicitHeight
            active: boardFrame.visible
            logicalRow: root.activeTableRow
            groupCol: root.boardCol
            onShowGrid: root.boardMode = false   // grouping column vanished → grid
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
        Row {   // Table / Board — a two-segment view toggle
            anchors.left: parent.left; anchors.leftMargin: 8
            height: parent.height - 1
            FlatButton {
                iconName: "table"; text: "Table"
                height: parent.height
                checked: !root.boardMode
                onClicked: { root.boardMode = false; root.forceActiveFocus() }
            }
            FlatButton {
                iconName: "kanban"; text: "Board"
                height: parent.height
                checked: root.boardMode
                enabled_: root.boardCol >= 0 || root.firstGroupCol >= 0
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
        color: Theme.colors.bg
        // A continuous-scroll page list (NOT PdfMultiPageView, whose internal
        // TableView always overflows horizontally by the vertical-scrollbar width
        // → a stray horizontal bar over the page). One PdfPageImage per page,
        // fit-width with a scrollbar gutter; vertical scroll only.
        PdfDocument {
            id: pdfFrameDoc
            source: root.activePdfRow >= 0 ? blockModel.mediaUrl(root.activePdfRow) : ""
        }
        ListView {
            id: pdfList
            anchors.fill: parent
            anchors.margins: 10
            clip: true
            model: pdfFrameDoc.pageCount
            spacing: 10
            cacheBuffer: Math.round(height * 1.5)
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Item {
                required property int index
                readonly property size pts: pdfFrameDoc.status === PdfDocument.Ready
                    ? pdfFrameDoc.pagePointSize(index) : Qt.size(8.5, 11)
                readonly property real pageW: pdfList.width - Theme.dim.scrollBarWidth - 8   // gutter
                width: pdfList.width
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
                }
            }
        }
    }

    // --- Block-drag overlays (viewport-fixed, on top of the document) ---
    // Drop-indicator line at the insertion gap.
    Rectangle {
        visible: root.blockDragging && root.dropGap >= 0
        x: root.gutterX; width: root.measureForRow(root.blockDragRow); height: 2; radius: 1
        y: (blockModel.layoutRevision, root.gapY(root.dropGap)) - flick.contentY - 1
        color: Theme.colors.accent
        z: 50
    }
    // Floating ghost of the dragged block — an accent outline with a barely-there
    // translucent fill (the document shows through), following the cursor.
    Rectangle {
        visible: root.blockDragging
        x: root.gutterX; width: root.measureForRow(root.blockDragRow); height: 30
        y: root.blockDragViewY - height / 2
        readonly property color _a: Theme.colors.accent
        color: Qt.rgba(_a.r, _a.g, _a.b, 0.06)
        border.width: 1; border.color: Qt.rgba(_a.r, _a.g, _a.b, 0.55)
        radius: Theme.dim.radius; z: 51
        Row {
            anchors { left: parent.left; leftMargin: 8; right: parent.right; rightMargin: 8
                      verticalCenter: parent.verticalCenter }
            spacing: 6
            Icon { name: "dots-six-vertical"; size: Theme.icon.sizeToolbar
                   color: Theme.colors.textMuted; anchors.verticalCenter: parent.verticalCenter }
            Text {
                width: parent.width - 30
                text: root.blockDragText; color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                elide: Text.ElideRight; anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    // Image-drop insertion indicator — a pulsing accent line at the snapped gap,
    // with an expanding ring on a solid dot, so it's obvious where a dragged image
    // will land. Follows the cursor between blocks as you drag.
    Item {
        visible: root.imageDropActive && root.imageDropGap >= 0
        z: 55
        readonly property real lineY: (blockModel.layoutRevision, root.gapY(root.imageDropGap)) - flick.contentY

        Rectangle {   // insertion line
            x: root.leftEdge; width: root.textWidth; height: 3; radius: 2
            y: parent.lineY - 1.5
            Behavior on y { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
            color: Theme.colors.accent
            SequentialAnimation on opacity {
                running: root.imageDropActive; loops: Animation.Infinite
                NumberAnimation { from: 1.0; to: 0.45; duration: 550; easing.type: Easing.InOutQuad }
                NumberAnimation { from: 0.45; to: 1.0; duration: 550; easing.type: Easing.InOutQuad }
            }
        }
        Rectangle {   // solid dot at the left end
            x: root.leftEdge - 4; y: parent.lineY - 4; width: 8; height: 8; radius: 4
            color: Theme.colors.accent
            Behavior on y { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
        }
        Rectangle {   // expanding ring emanating from the dot
            id: dropRing
            x: root.leftEdge - 4; y: parent.lineY - 4; width: 8; height: 8; radius: 4
            Behavior on y { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
            color: "transparent"; border.width: 2; border.color: Theme.colors.accent
            transformOrigin: Item.Center
            SequentialAnimation on scale {
                running: root.imageDropActive; loops: Animation.Infinite
                NumberAnimation { from: 0.7; to: 2.6; duration: 900; easing.type: Easing.OutQuad }
            }
            SequentialAnimation on opacity {
                running: root.imageDropActive; loops: Animation.Infinite
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
        x: root.leftEdge
        y: (blockModel.layoutRevision, r >= 0 ? blockModel.yForRow(r) : 0) - flick.contentY + 6
        width: dispW; height: dispH

        VideoSurfaceItem {
            id: videoSurface
            anchors.fill: parent
            videoDecoder: videoDec
            fillColor: Qt.rgba(Theme.colors.surface.r, Theme.colors.surface.g,
                               Theme.colors.surface.b, 1.0)
        }
        // No click-to-play on the frame — the toolbar is the sole transport.
    }

    // Persistent transport toolbar for EVERY video (all built on load — see
    // allVideoRows). `live` = this row is the active player (controls bound to
    // the decoder); otherwise the bar shows the static state and any control
    // activates the video first. Only viewport-near toolbars are shown; the rest
    // stay instantiated (no scroll churn) but hidden.
    Repeater {
        model: root.allVideoRows
        delegate: Rectangle {
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
            opacity: (blockSelected && !live) ? 0.35 : 1.0
            Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutQuad } }
            visible: root.activeTableRow < 0 && root.activePdfRow < 0
                     && row >= root.firstVisible - 2 && row <= root.lastVisible + 2
            readonly property real measure: root.measureForRow(row)
            readonly property int vw: (blockModel.contentRevision, blockModel.mediaW(row))
            readonly property int vh: blockModel.mediaH(row)
            readonly property real dispW: vw > 0 ? Math.min(measure, vw) : measure
            readonly property real dispH: (vw > 0 && vh > 0) ? Math.round(dispW * vh / vw)
                                                             : Math.round(dispW * 0.5)
            readonly property int totalFrames: live ? videoDec.frameCount
                                                    : (blockModel.contentRevision, blockModel.mediaFrames(row))

            z: 56
            x: root.leftEdge
            y: (blockModel.layoutRevision, blockModel.yForRow(row)) + 6 + dispH - flick.contentY
            width: dispW
            height: root.videoTransportH
            color: "#212121"      // a hair lighter than the page (#1b1b1b)
            Rectangle { width: parent.width; height: 1; color: Theme.colors.border }   // top hairline

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6; anchors.rightMargin: 8
                spacing: 0

                FlatButton { iconName: "skip-back"; tooltip: qsTr("Jump to start"); tooltipSide: "top"
                    onClicked: { root.ensureVideoActive(vbar.row); root.seekVideoStart() } }
                FlatButton { iconName: "rewind"; tooltip: qsTr("Rewind — hold"); tooltipSide: "top"
                    onPressed: { root.ensureVideoActive(vbar.row); root.startVideoFastSeek(-1) }
                    onReleased: root.stopVideoFastSeek() }
                FlatButton { iconName: "caret-left"; tooltip: qsTr("Step back"); tooltipSide: "top"
                    onClicked: { root.ensureVideoActive(vbar.row); root.stepVideoFrames(-1) } }
                FlatButton { iconName: (vbar.live && videoDec.isPlaying) ? "pause" : "play"
                    tooltip: qsTr("Play / Pause"); tooltipSide: "top"; onClicked: root.playVideo(vbar.row) }
                FlatButton { iconName: "caret-right"; tooltip: qsTr("Step forward"); tooltipSide: "top"
                    onClicked: { root.ensureVideoActive(vbar.row); root.stepVideoFrames(1) } }
                FlatButton { iconName: "fast-forward"; tooltip: qsTr("Fast-forward — hold"); tooltipSide: "top"
                    onPressed: { root.ensureVideoActive(vbar.row); root.startVideoFastSeek(1) }
                    onReleased: root.stopVideoFastSeek() }
                FlatButton { iconName: "skip-forward"; tooltip: qsTr("Jump to end"); tooltipSide: "top"
                    onClicked: { root.ensureVideoActive(vbar.row); root.seekVideoEnd() } }

                FlatSlider {
                    id: vscrub
                    Layout.fillWidth: true
                    Layout.leftMargin: 8; Layout.rightMargin: 8
                    Layout.alignment: Qt.AlignVCenter
                    from: 0; to: Math.max(1, vbar.totalFrames - 1)
                    fillColor: Theme.colors.textBright   // white progress fill
                    property bool wasPlaying: false
                    // Pause while scrubbing so each seek lands; resume repositions
                    // the streaming decoder to the scrubbed frame first.
                    onPressedChanged: {
                        if (pressed) {
                            root.ensureVideoActive(vbar.row)
                            wasPlaying = videoDec.isPlaying
                            videoDec.pause(); videoAudio.pause()
                        } else if (wasPlaying) {
                            root._vidSyncForResume(); videoDec.play()
                            if (videoAudio.hasAudio) videoAudio.play()
                        }
                    }
                    onMoved: { root.ensureVideoActive(vbar.row); root._vidScrubTo(Math.round(value)) }
                    Connections {
                        target: videoDec
                        function onCurrentFrameChanged() {
                            if (vbar.live && !vscrub.pressed) vscrub.value = videoDec.currentFrame
                        }
                    }
                    // Not the live player → park the scrubber at the banked playhead
                    // (not 0), so a torn-down video shows where it left off.
                    Binding {
                        target: vscrub; property: "value"
                        value: (root.videoPlayheadRev, root.videoPlayheadFor(vbar.row))
                        when: !vbar.live && !vscrub.pressed
                    }
                }

                Text {   // frame counter (mirrors ufb)
                    text: (vbar.live ? videoDec.currentFrame
                                     : (root.videoPlayheadRev, root.videoPlayheadFor(vbar.row)))
                          + " / " + Math.max(0, vbar.totalFrames - 1)
                    color: Theme.colors.textMuted
                    font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
                    Layout.rightMargin: 4
                }

                FlatButton { iconName: "repeat"; tooltip: qsTr("Loop"); tooltipSide: "top"
                    checked: root.videoLoop; onClicked: root.toggleVideoLoop() }
                FlatButton {
                    visible: vbar.live && videoAudio.hasAudio
                    iconName: (videoAudio.muted || videoAudio.volume <= 0) ? "speaker-x" : "speaker-high"
                    tooltip: qsTr("Mute"); tooltipSide: "top"; onClicked: root.toggleVideoMute()
                }
                FlatSlider {
                    visible: vbar.live && videoAudio.hasAudio
                    Layout.preferredWidth: 64; Layout.leftMargin: 2
                    Layout.alignment: Qt.AlignVCenter
                    from: 0; to: 1
                    value: videoAudio.muted ? 0 : videoAudio.volume
                    fillColor: Theme.colors.textMuted
                    onMoved: { videoAudio.setVolume(value); if (value > 0) videoAudio.setMuted(false) }
                }
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
            opacity: blockSelected ? 0.35 : 1.0
            Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutQuad } }
            visible: root.activeTableRow < 0 && root.activePdfRow < 0
                     && row >= root.firstVisible - 2 && row <= root.lastVisible + 2
            readonly property real measure: root.measureForRow(row)
            readonly property int vw: (blockModel.contentRevision, blockModel.mediaW(row))
            readonly property int vh: blockModel.mediaH(row)
            readonly property real dispH: (vw > 0 && vh > 0) ? Math.round(measure * vh / vw)
                                                             : Math.round(measure * 1.3)
            readonly property int pages: (blockModel.contentRevision, blockModel.mediaPdfPages(row))
            readonly property int page: (root.pdfPageRev, root.pdfPageFor(row))

            z: 56
            x: root.leftEdge
            y: (blockModel.layoutRevision, blockModel.yForRow(row)) + 6 + dispH - flick.contentY
            width: measure
            height: root.pdfNavH
            color: "#212121"      // a hair lighter than the page
            Rectangle { width: parent.width; height: 1; color: Theme.colors.border }   // top hairline

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
               : (root._isImageRow(cursor.focusRow) ? cursor.focusRow : -1))
        visible: row >= 0 && root.activeTableRow < 0 && root.activePdfRow < 0
        readonly property real imgX: root.leftEdge
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
            width: 24; height: 24; radius: 4
            x: imgResize.imgX + imgResize.imgW - width - 6
            y: imgResize.imgTopV + 6
            color: fitMA.containsMouse ? Theme.colors.accent : Qt.rgba(0, 0, 0, 0.55)
            border.width: 1; border.color: Theme.colors.border
            Icon { anchors.centerIn: parent; name: "frame-corners"; size: 14; color: "#f2f2f2" }
            MouseArea {
                id: fitMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: blockModel.setMediaWidth(imgResize.row, Math.round(root.pageWidth))
            }
        }

        Rectangle {   // proportional drag handle (bottom-right)
            id: dragHandle
            width: 22; height: 22; radius: 4
            x: imgResize.imgX + imgResize.imgW - width - 6
            y: imgResize.imgTopV + imgResize.imgH - height - 6
            color: (dragMA.containsMouse || root.imageResizing) ? Theme.colors.accent : Qt.rgba(0, 0, 0, 0.55)
            border.width: 1; border.color: Theme.colors.border
            Icon { anchors.centerIn: parent; name: "resize"; size: 14; color: "#f2f2f2" }
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
                    root.imageResizeW = Math.max(80, Math.min(root.pageWidth,
                        root._imgResizeStartW + (m.x - root._imgResizePressX)))
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
        x: root.leftEdge
        y: (blockModel.layoutRevision, root.imageResizeRow >= 0
            ? blockModel.yForRow(root.imageResizeRow) : 0) + 6 - flick.contentY
        width: root.imageResizeW
        height: root.imageResizeW * root.imageResizeAspect
        color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.08)
        border.width: 2; border.color: Theme.colors.accent
        radius: Theme.dim.radius
        Rectangle {
            anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6
            width: dimLabel.width + 10; height: dimLabel.height + 6; radius: 3
            color: Qt.rgba(0, 0, 0, 0.7)
            Text {
                id: dimLabel; anchors.centerIn: parent
                text: Math.round(root.imageResizeW) + " × " + Math.round(root.imageResizeW * root.imageResizeAspect)
                color: "#f2f2f2"; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
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
            width: 20; height: 20; radius: 4
            x: cellImgResize.rx + cellImgResize.rw - width - 4
            y: cellImgResize.ry + 4
            color: cFitMA.containsMouse ? Theme.colors.accent : Qt.rgba(0, 0, 0, 0.55)
            border.width: 1; border.color: Theme.colors.border
            Icon { anchors.centerIn: parent; name: "frame-corners"; size: 12; color: "#f2f2f2" }
            MouseArea {
                id: cFitMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: blockModel.tableSetCellImageWidth(tcur.row, tcur.cr, tcur.cc, Math.round(root.cellImgMaxW))
            }
        }

        Rectangle {   // proportional drag (bottom-right)
            width: 18; height: 18; radius: 4
            x: cellImgResize.rx + cellImgResize.rw - width - 4
            y: cellImgResize.ry + cellImgResize.rh - height - 4
            color: (cDragMA.containsMouse || root.cellImgResizing) ? Theme.colors.accent : Qt.rgba(0, 0, 0, 0.55)
            border.width: 1; border.color: Theme.colors.border
            Icon { anchors.centerIn: parent; name: "resize"; size: 12; color: "#f2f2f2" }
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
            width: cDimLabel.width + 10; height: cDimLabel.height + 6; radius: 3
            color: Qt.rgba(0, 0, 0, 0.7)
            Text {
                id: cDimLabel; anchors.centerIn: parent
                text: Math.round(root.cellResizeW) + " × " + Math.round(root.cellResizeW * root.cellResizeAspect)
                color: "#f2f2f2"; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
            }
        }
    }

    // Context-menu target highlight (whole block) — tints the block the hovered
    // menu item will act on (red for destructive). Column/row scopes are drawn
    // inside the table itself. Document view only (the menu opens there).
    Rectangle {
        visible: root.menuHiScope === "block" && root.menuRow >= 0 && root.activeTableRow < 0 && root.activePdfRow < 0
        x: root.leftEdge
        y: (blockModel.layoutRevision, blockModel.yForRow(root.menuRow)) - flick.contentY
        width: root.measureForRow(root.menuRow)
        height: (blockModel.layoutRevision, blockModel.heightForRow(root.menuRow))
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
        visible: tItem !== null && root.activeTableRow < 0 && root.activePdfRow < 0   // Document view only
        readonly property real topV: dlg ? dlg.y - flick.contentY + 6 : 0   // table content top (tableHost y:6)
        readonly property real cw: tItem ? tItem.width : 0
        readonly property real ch: tItem ? tItem.height : 0
        readonly property real tableX: root.leftEdge   // table is left-aligned at the shared edge
        readonly property bool overflow: tItem ? tItem.overflowing : false
        readonly property real sbH: Theme.dim.scrollBarWidth

        Rectangle {   // horizontal scrollbar — a root overlay (an inner ScrollBar
                      // would sit under the document mouse layer) that drives the
                      // table's scrollX; sits between the table and the +row button.
            visible: tableAdd.overflow
            x: tableAdd.tableX; y: tableAdd.topV + tableAdd.ch + 2
            width: tableAdd.cw; height: tableAdd.sbH; z: 41; color: "transparent"
            readonly property real contentTW: tableAdd.tItem ? tableAdd.tItem.contentW : 0
            readonly property real maxScroll: Math.max(0, contentTW - tableAdd.cw)
            readonly property real thumbW: Math.max(24, width * tableAdd.cw / Math.max(1, contentTW))
            readonly property real maxThumbX: width - thumbW
            Rectangle {
                id: hthumb
                height: parent.height; radius: height / 2; width: parent.thumbW
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
            x: tableAdd.tableX; y: tableAdd.topV + tableAdd.ch + (tableAdd.overflow ? tableAdd.sbH + 6 : 2)
            width: tableAdd.cw; height: 14; radius: 3; z: 40
            color: addRowMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.surfaceHover
            border.width: 1; border.color: Theme.colors.border
            Text { anchors.centerIn: parent; text: "+"; color: Theme.colors.textMuted; font.pixelSize: 13 }
            MouseArea {
                id: addRowMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: blockModel.tableInsertRow(cursor.focusRow, blockModel.tableRows(cursor.focusRow))
            }
        }
        Rectangle {   // + column, right edge
            x: tableAdd.tableX + tableAdd.cw + 2; y: tableAdd.topV
            width: 14; height: tableAdd.ch; radius: 3; z: 40
            color: addColMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.surfaceHover
            border.width: 1; border.color: Theme.colors.border
            Text { anchors.centerIn: parent; text: "+"; color: Theme.colors.textMuted; font.pixelSize: 13 }
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
    component MenuRow: Rectangle {
        property alias text: menuRowLabel.text
        property bool danger: false
        property string scope: "block"   // what this item targets: block | column | row
        signal activated()
        width: 184; height: 28; radius: 4
        color: menuRowMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
        Text {
            id: menuRowLabel
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: 10
            anchors.right: parent.right; anchors.rightMargin: 10
            elide: Text.ElideRight                 // keep long labels (a URL) inside the menu
            color: parent.danger ? Theme.colors.error : Theme.colors.text
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
        }
        MouseArea {
            id: menuRowMA
            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
            // Hovering an item highlights its target scope on the document/table.
            onContainsMouseChanged: if (containsMouse) { root.menuHiScope = parent.scope; root.menuHiDanger = parent.danger }
            onClicked: { parent.activated(); blockMenu.close() }
        }
    }

    // --- Choice-cell option picker (root overlay above the mouse layer) ---
    function openChoicePicker(trow, r, c, vx, vy) {
        choicePicker.row = trow; choicePicker.r = r; choicePicker.c = c
        root.choiceX = vx; root.choiceY = vy
        choicePicker.open()
    }
    ChoicePicker {
        id: choicePicker
        z: 60
        x: Math.max(8, Math.min(root.choiceX, root.width - width - 8))
        y: Math.max(8, Math.min(root.choiceY, root.height - height - 8))
        onClosed: root.forceActiveFocus()
        onEditOptions: root.openChoiceEditor(choicePicker.row, choicePicker.c)
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
        readonly property bool isCode: root.menuRow >= 0
            && (blockModel.contentRevision, blockModel.typeForRow(root.menuRow) === 2)
        readonly property bool isTable: root.menuRow >= 0
            && (blockModel.contentRevision, blockModel.typeForRow(root.menuRow) === 7)
        readonly property bool isMedia: root.menuRow >= 0
            && (blockModel.contentRevision, blockModel.typeForRow(root.menuRow) === 3)
        readonly property bool isPdf: isMedia
            && (blockModel.contentRevision, blockModel.mediaKind(root.menuRow)) === "pdf"
        // In a full-frame tab (table/PDF) the menu is a view INTO one block, so
        // document-structural block ops (add/duplicate/copy block) don't belong.
        readonly property bool inFrameTab: root.activeTableRow >= 0 || root.activePdfRow >= 0
        padding: 4; z: 60
        // Reactive on-screen clamp: re-evaluates as the menu's height settles after
        // open (so a long menu is positioned right on the FIRST trigger, not the 2nd).
        x: Math.max(8, Math.min(root.menuX, root.width - width - 8))
        y: Math.max(8, Math.min(root.menuY, root.height - height - 8))
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside | Popup.CloseOnReleaseOutside
        onClosed: { root.menuHiScope = ""; root.forceActiveFocus() }
        background: Rectangle { color: Theme.colors.surface; radius: 6
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 1
            MenuRow { visible: root.menuLinkUrl.length > 0
                      text: "Open " + root.truncUrl(root.menuLinkUrl)
                      onActivated: Qt.openUrlExternally(root.menuLinkUrl) }
            Rectangle { visible: root.menuLinkUrl.length > 0; width: parent.width; height: 1; color: Theme.colors.divider }
            MenuRow { visible: !blockMenu.inFrameTab; text: "Add block above"; onActivated: root.addBlockAbove(root.menuRow) }
            MenuRow { visible: !blockMenu.inFrameTab; text: "Add block below"; onActivated: root.addBlockBelow(root.menuRow) }
            MenuRow { visible: !blockMenu.inFrameTab; text: "Duplicate block"; onActivated: root.duplicateBlock(root.menuRow) }
            MenuRow { visible: !blockMenu.inFrameTab; text: blockMenu.isTable ? "Copy table" : "Copy"; onActivated: root.copyBlock(root.menuRow) }
            MenuRow { visible: blockMenu.isMedia
                      text: Qt.platform.os === "windows" ? "Show in Explorer" : "Reveal in Finder"
                      onActivated: blockModel.revealMedia(root.menuRow) }
            MenuRow { visible: blockMenu.isMedia; text: "Open in ufb"
                      onActivated: blockModel.openMediaInUfb(root.menuRow) }
            MenuRow { visible: !blockMenu.isTable; text: "Insert table below"; onActivated: root.insertTableAt(root.menuRow) }
            // --- table cell/row/column ops (table blocks only) ---
            Rectangle { visible: blockMenu.isTable && !blockMenu.inFrameTab; width: parent.width; height: 1; color: Theme.colors.divider }
            MenuRow { visible: blockMenu.isTable && root.activeTableRow < 0; text: "Open in tab"; onActivated: root.setActiveTab(blockModel.idForRow(root.menuRow)) }
            MenuRow { visible: blockMenu.isPdf && root.activePdfRow < 0; text: "Open in tab"; onActivated: root.setActiveTab(blockModel.idForRow(root.menuRow)) }
            MenuRow { visible: blockMenu.isTable; scope: "row";    text: "Insert row above";  onActivated: root.tblInsRowAbove() }
            MenuRow { visible: blockMenu.isTable; scope: "row";    text: "Insert row below";  onActivated: root.tblInsRowBelow() }
            MenuRow { visible: blockMenu.isTable; scope: "column"; text: "Insert column left";  onActivated: root.tblInsColLeft() }
            MenuRow { visible: blockMenu.isTable; scope: "column"; text: "Insert column right"; onActivated: root.tblInsColRight() }
            // Reorder (body rows only — a header row's place is structural).
            MenuRow { visible: blockMenu.isTable && root.menuCellR > blockModel.tableHeaderRows(root.menuRow)
                      scope: "row"; text: "Move row up"; onActivated: root.tblMoveRow(-1) }
            MenuRow { visible: blockMenu.isTable && root.menuCellR >= blockModel.tableHeaderRows(root.menuRow)
                               && root.menuCellR < blockModel.tableRows(root.menuRow) - 1
                      scope: "row"; text: "Move row down"; onActivated: root.tblMoveRow(1) }
            MenuRow { visible: blockMenu.isTable && root.menuCellC > 0
                      scope: "column"; text: "Move column left"; onActivated: root.tblMoveCol(-1) }
            MenuRow { visible: blockMenu.isTable && root.menuCellC < blockModel.tableColumns(root.menuRow) - 1
                      scope: "column"; text: "Move column right"; onActivated: root.tblMoveCol(1) }
            MenuRow { visible: blockMenu.isTable && root.menuCellR >= blockModel.tableHeaderRows(root.menuRow)
                      scope: "row"; text: "Duplicate row"; onActivated: root.tblDupRow() }
            MenuRow { visible: blockMenu.isTable; scope: "column"; text: "Duplicate column"; onActivated: root.tblDupCol() }
            // One-shot sort of the body rows by this column (header stays pinned).
            MenuRow { visible: blockMenu.isTable
                               && blockModel.tableRows(root.menuRow) - blockModel.tableHeaderRows(root.menuRow) > 1
                      scope: "column"; text: "Sort ascending"; onActivated: root.tblSort(true) }
            MenuRow { visible: blockMenu.isTable
                               && blockModel.tableRows(root.menuRow) - blockModel.tableHeaderRows(root.menuRow) > 1
                      scope: "column"; text: "Sort descending"; onActivated: root.tblSort(false) }
            Rectangle { visible: blockMenu.isTable; width: parent.width; height: 1; color: Theme.colors.divider }
            MenuRow { visible: blockMenu.isTable; scope: "column"; text: "Align left";   onActivated: root.tblAlign(0) }
            MenuRow { visible: blockMenu.isTable; scope: "column"; text: "Align center"; onActivated: root.tblAlign(1) }
            MenuRow { visible: blockMenu.isTable; scope: "column"; text: "Align right";  onActivated: root.tblAlign(2) }
            MenuRow { visible: blockMenu.isTable && root.tblColKind() !== 1; scope: "column"; text: "Make choice column"; onActivated: root.tblMakeChoiceCol() }
            MenuRow { visible: blockMenu.isTable && root.tblColKind() !== 2; scope: "column"; text: "Make checkmark column"; onActivated: root.tblMakeCheckCol() }
            MenuRow { visible: blockMenu.isTable && root.tblColKind() === 1; scope: "column"; text: "Edit options…"; onActivated: root.openChoiceEditor(root.menuRow, root.menuCellC) }
            MenuRow { visible: blockMenu.isTable && root.tblColKind() !== 0 && !root.boardMode
                      scope: "column"; text: "View as board"; onActivated: root.openBoard(root.menuRow, root.menuCellC) }
            MenuRow { visible: blockMenu.isTable && root.tblColKind() !== 0; scope: "column"; text: "Make text column"; danger: true; onActivated: root.tblMakeTextCol() }
            MenuRow { visible: blockMenu.isTable; text: blockModel.tableHeaderRows(root.menuRow) > 0 ? "Remove header row" : "Add header row"; onActivated: root.tblToggleHeader() }
            Rectangle { visible: blockMenu.isTable; width: parent.width; height: 1; color: Theme.colors.divider }
            MenuRow { visible: root.menuCellHasImage; text: "Remove image"; danger: true; onActivated: root.tblRemoveImage() }
            MenuRow { visible: blockMenu.isTable; scope: "row";    text: "Delete row";    danger: true; onActivated: root.tblDelRow() }
            MenuRow { visible: blockMenu.isTable; scope: "column"; text: "Delete column"; danger: true; onActivated: root.tblDelCol() }
            // --- code (non-table) ---
            Rectangle { visible: !blockMenu.isTable && !blockMenu.isMedia; width: parent.width; height: 1; color: Theme.colors.divider }
            MenuRow {
                visible: !blockMenu.isTable && !blockMenu.isMedia   // text-only op
                text: blockMenu.isCode ? "Change language…" : "Make code block"
                onActivated: blockMenu.isCode ? root.openLangPopupForRow(root.menuRow)
                                              : root.makeCodeAt(root.menuRow)
            }
            Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
            MenuRow { text: "Delete block"; danger: true; onActivated: root.deleteBlock(root.menuRow) }
        }
    }

    // --- Code-block language picker (shared; positioned where the menu opened) ---
    // Type any language (lenient: js, bash, python…; blank = plain) or pick a
    // common one. Applies to langPopup.targetRow via blockModel.setCodeLanguage.
    Popup {
        id: langPopup
        property int targetRow: -1
        readonly property var quick: ["javascript", "typescript", "python", "bash", "json",
                                      "html", "css", "cpp", "c", "go", "rust", "sql", "yaml", "markdown"]
        width: 250; padding: 8; focus: true; z: 60
        x: Math.max(8, Math.min(root.menuX, root.width - width - 8))
        y: Math.max(8, Math.min(root.menuY, root.height - height - 8))
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: { targetRow = -1; root.forceActiveFocus() }
        background: Rectangle { color: Theme.colors.surface; radius: 6
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
                width: parent.width; height: 30; radius: 4
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
                        text: "language — e.g. js, bash (blank = plain)"
                        color: Theme.colors.textSubtle; font: langField.font
                        elide: Text.ElideRight
                    }
                }
            }
            Flow {
                width: parent.width; spacing: 4
                Repeater {
                    model: langPopup.quick
                    delegate: Rectangle {
                        required property string modelData
                        height: 20; width: chipText.implicitWidth + 14; radius: 3
                        color: chipMA.containsMouse ? Theme.colors.accentMuted : Theme.colors.codeBg
                        border.width: 1; border.color: Theme.colors.border
                        Text { id: chipText; anchors.centerIn: parent; text: modelData
                               color: Theme.colors.textMuted
                               font.family: Theme.font.family; font.pixelSize: 11 }
                        MouseArea { id: chipMA; anchors.fill: parent; hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: langPopup.apply(modelData) }
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
        background: Rectangle { color: Theme.colors.surface; radius: 6
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
                width: parent.width; height: 30; radius: 4
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
