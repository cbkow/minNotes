import QtQuick

// One block's renderer — the editor's delegate pool instantiates one per slot
// and points it at a row (logicalRow). Extracted from Editor.qml for SR-2 so
// split-row lanes (SR-3) can place blocks by x and width, not only by row.
// Passive like every block: the editor's central handlers own input.
Item {
    id: cell
    // The editor this block renders in, and the controllers it reads. Ids
    // don't cross file boundaries, so the editor hands them over (Editor.qml).
    required property var editor
    readonly property var cursor: editor.cursorObj
    readonly property var flick: editor.flickItem
    property int logicalRow: -1   // the row this slot renders — set by the pool (Editor.qml)
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
    readonly property Item langChip: codeLangChip // code language chip, for hit-testing
    // Lane geometry (SR-3): {x, w} page-relative — the page for a top-level block, its
    // lane otherwise. contentRevision covers structure changes, pageWidth the measure.
    // NOT the layout revision: the measure drives the height, whose settle bumps it.
    readonly property var lane: (blockModel.contentRevision, editor.pageWidth,
                                 active ? editor.laneOf(logicalRow) : ({ x: 0, w: editor.pageWidth }))
    // Horizontal measure: the lane's width (the page's for a top-level block).
    readonly property real measure: lane.w - 2 * cellInset
    readonly property bool isRecord: active && te.btype === 10   // a split row's record: no text of its own
    readonly property bool inLane: active && (blockModel.contentRevision, blockModel.laneForRow(logicalRow)) >= 0
    // Derived tables (SR-4 S5): the table this record or cell block belongs to (-1 = none), its
    // row in the table, and a cell block's column. contentRevision only (no layout deps: these
    // feed the text's font and so the height).
    readonly property int tableHead: active ? (blockModel.contentRevision, blockModel.tableHeadOf(logicalRow)) : -1
    readonly property bool inTable: tableHead >= 0
    readonly property bool isTableRecord: isRecord && inTable
    readonly property int gridRow: inTable ? (blockModel.contentRevision, blockModel.gridRowOf(logicalRow)) : -1
    readonly property int gridCol: inTable && !isRecord ? (blockModel.contentRevision, blockModel.gridColumnOf(logicalRow)) : -1
    readonly property bool headerCell: inTable && (blockModel.contentRevision, blockModel.isHeaderRow(logicalRow))
    readonly property int colKind: gridCol >= 0 && !headerCell
        ? (blockModel.contentRevision, blockModel.gridColumnKind(tableHead, gridCol)) : 0
    readonly property int colAlign: gridCol >= 0 ? (blockModel.contentRevision, blockModel.gridColAlign(tableHead, gridCol)) : 0
    readonly property string cellFg: gridCol >= 0
        ? (blockModel.contentRevision, blockModel.gridCellFg(tableHead, gridRow, gridCol)) : ""
    readonly property int checkState: colKind === 2
        ? (blockModel.contentRevision, blockModel.gridCellCheck(tableHead, gridRow, gridCol)) : 0
    readonly property real cellInset: gridCol >= 0 ? 8 : 0   // a table cell's text sits inside its column

    // A record draws the dividers between its lanes: a hairline centred in each gap,
    // the full height of the row (S7 makes them draggable).
    Repeater {
        model: cell.isRecord && !cell.inTable ? Math.max(0, (blockModel.contentRevision, blockModel.laneCount(cell.logicalRow)) - 1) : 0
        delegate: Rectangle {
            required property int index
            readonly property var span: editor.laneSpan(cell.logicalRow, index)
            // Accent on hover only (2026-09-14 walk) — a drag shows its own preview line.
            readonly property bool hot: !editor.dividerDragging && !editor.pulling
                && editor.dividerHoverRecord === cell.logicalRow && editor.dividerHoverIndex === index
            x: editor.leftEdge + span.x + span.w + blockModel.laneGap / 2 - (hot ? 1 : 0.5)
            y: 0
            width: hot ? 2 : 1
            height: cell.height
            color: hot ? Theme.colors.accent : Theme.colors.border
        }
    }
    // Hover cues (2026-09-14 walk): a table column border under the pointer (this table's records),
    // and the pull band on a block's edge. Hover only — never while a drag is in flight.
    readonly property int hotColumn: !isTableRecord || editor.dividerDragging || editor.pulling
        || editor.dividerHoverRecord < 0 || blockModel.tableHeadOf(editor.dividerHoverRecord) !== tableHead
        ? -1 : editor.dividerHoverIndex
    Rectangle {
        visible: cell.active && !cell.isRecord && !editor.pulling && !editor.dividerDragging
                 && editor.pullHoverRow === cell.logicalRow
        x: editor.leftEdge + cell.lane.x + (editor.pullHoverSide === 1 ? 0 : cell.lane.w - 3)
        y: 0
        width: 3
        height: cell.height
        color: Theme.colors.accent
        opacity: 0.55
        z: 2
    }

    // A table row's record paints the row under its cells (SR-4 S5): each column's cell
    // background (cell > row > column colour; header rows tinted) and the grid lines. It
    // stacks below the cell blocks' delegates.
    // The row's cells sit between its pockets (S5b: the first row's top, the last row's bottom).
    readonly property real padTop: isTableRecord ? (blockModel.layoutRevision, blockModel.tablePadTop(logicalRow)) : 0
    readonly property real padBottom: isTableRecord ? (blockModel.layoutRevision, blockModel.tablePadBottom(logicalRow)) : 0
    Repeater {
        model: cell.isTableRecord ? (blockModel.contentRevision, blockModel.tableColumnCount(cell.tableHead)) : 0
        delegate: Rectangle {
            required property int index
            readonly property string bg: (blockModel.contentRevision, blockModel.gridCellBg(cell.tableHead, cell.gridRow, index))
            x: editor.leftEdge + (blockModel.layoutRevision, blockModel.tableColumnLeft(cell.tableHead, index))
            y: cell.padTop
            width: (blockModel.layoutRevision, blockModel.tableColumnWidth(cell.tableHead, index))
            height: Math.max(0, cell.height - cell.padTop - cell.padBottom)
            color: bg !== "" ? bg : cell.headerCell ? Theme.colors.surfaceHover : "transparent"
            // ONE wash per cell (2026-09-14: four stacked rectangles cost a big table's scroll), by
            // priority: a file drop / the column a grip drag moves (accent) > the hovered block-menu
            // item's target (accent, error when destructive) > the cell rectangle / a grip-picked set
            // (selection). 0 = none.
            readonly property bool inSet: {
                const s = editor.gridSet
                return s !== null && s.head === cell.tableHead && s.rev === blockModel.contentRevision
                    && (s.kind === "row" ? s.items.indexOf(cell.gridRow) >= 0 : s.items.indexOf(index) >= 0)
            }
            readonly property int wash: {
                if (editor.dropGridHead === cell.tableHead && editor.dropGridR === cell.gridRow && editor.dropGridC === index) return 4
                if (editor.gridColDragging && editor.gridGripPressHead === cell.tableHead && editor.gridGripPressIndex === index) return 3
                const sc = editor.menuHiScope, t = editor.menuGrid
                if (t !== null && t.head === cell.tableHead
                    && (sc === "table" || (sc === "row" && cell.gridRow === t.r) || (sc === "column" && index === t.c)
                        || (sc === "set" && inSet))) return 2
                const rect = editor.cellRect
                if (rect !== null && rect.head === cell.tableHead && cell.gridRow >= rect.r0 && cell.gridRow <= rect.r1
                    && index >= rect.c0 && index <= rect.c1) return 1
                return inSet ? 1 : 0
            }
            Rectangle {
                visible: parent.wash > 0
                anchors.fill: parent
                readonly property color tone: parent.wash === 2 && editor.menuHiDanger ? Theme.colors.error : Theme.colors.accent
                color: parent.wash === 1 ? Theme.colors.selectionBg
                     : Qt.rgba(tone.r, tone.g, tone.b, parent.wash === 4 ? 0.16 : 0.14)
                border.width: parent.wash === 4 ? 2 : 0
                border.color: Theme.colors.accent
            }
            Rectangle {   // the column's right border — accent while hovered
                readonly property bool hot: cell.hotColumn === index
                anchors.right: parent.right; width: hot ? 2 : 1; height: parent.height
                color: hot ? Theme.colors.accent : Theme.colors.border
            }
            Rectangle { anchors.bottom: parent.bottom; height: 1; width: parent.width; color: Theme.colors.border }
        }
    }
    Rectangle {   // the table's left edge
        visible: cell.isTableRecord
        x: editor.leftEdge; y: cell.padTop; width: 1; height: Math.max(0, cell.height - cell.padTop - cell.padBottom)
        color: Theme.colors.border
    }
    Rectangle {   // the top edge, on the table's first row
        visible: cell.isTableRecord && cell.gridRow === 0
        x: editor.leftEdge; y: cell.padTop; height: 1
        width: (blockModel.layoutRevision, blockModel.contentRevision, cell.isTableRecord ? blockModel.tableWidth(cell.tableHead) : 0)
        color: Theme.colors.border
    }
    // Grips (SR-4 S7b): the hovered row's pill in the left margin, the hovered column's in the pocket
    // above the first row; accent while pressed or dragged.
    Rectangle {
        readonly property bool live: editor.gridGripPressed && editor.gridGripPressKind === "row"
            && editor.gridGripPressHead === cell.tableHead && editor.gridGripPressIndex === cell.gridRow
        visible: cell.isTableRecord && (live || (editor.gridGripKind === "row" && editor.gridGripHead === cell.tableHead
                                                 && editor.gridGripIndex === cell.gridRow))
        x: editor.leftEdge - 13; y: cell.padTop; width: 8
        height: Math.max(0, cell.height - cell.padTop - cell.padBottom)
        color: live ? Theme.colors.accentMuted : Theme.colors.surfaceHover
        border.width: 1; border.color: live ? Theme.colors.accent : Theme.colors.border
    }
    Rectangle {
        readonly property bool live: (editor.gridColDragging || (editor.gridGripPressed && editor.gridGripPressKind === "col"))
            && editor.gridGripPressHead === cell.tableHead
        readonly property int col: !cell.isTableRecord || cell.gridRow !== 0 ? -1
            : live ? editor.gridGripPressIndex
            : editor.gridGripKind === "col" && editor.gridGripHead === cell.tableHead ? editor.gridGripIndex : -1
        visible: col >= 0
        x: editor.leftEdge + (blockModel.layoutRevision, col >= 0 ? blockModel.tableColumnLeft(cell.tableHead, col) : 0)
        width: (blockModel.layoutRevision, col >= 0 ? blockModel.tableColumnWidth(cell.tableHead, col) : 0)
        y: cell.padTop - 13; height: 8
        color: live ? Theme.colors.accentMuted : Theme.colors.surfaceHover
        border.width: 1; border.color: live ? Theme.colors.accent : Theme.colors.border
    }

    // The delegate spans the whole field (row fill, washes); content sits at colLeft.
    z: isTableRecord ? -1 : 0
    x: 0
    width: flick.contentWidth   // == flick.width outside ink mode
    visible: active && (editor.frameLo < 0 || (logicalRow >= editor.frameLo && logicalRow <= editor.frameHi))   // the grid frame shows one table
             && !(blockModel.layoutRevision, blockModel.rowHidden(logicalRow))                                  // T4: a filtered-out row
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
                           + (isVideoMedia ? editor.videoTransportH
                                           : isPdfMedia ? editor.pdfNavH : 0)
          : te.btype === 10 ? (blockModel.layoutRevision, blockModel.heightForRow(logicalRow))   // record: its tallest lane
          : te.btype === 6 ? 12 + 18                       // divider
          : (te.btype === 2 ? 24 : 12) + te.height   // te.height = lineCount*lineH (even)

    // Media is known-geometry: the MODEL derives its height from the
    // probed dims + the content width (setContentWidth). The delegate
    // renders to that authoritative value and never measures back —
    // measuring the child's transient (a 0.5-ratio fallback while
    // implicitHeight lags logicalRow on recycle) would push a wrong
    // height to the Fenwick → contentY/firstVisible churn and a
    // scroll-in jump. Text/code/table still measure (reflow is unknown).
    function reportHeight() {
        if (!active || isMedia || te.btype === 10) return   // media and records never measure back
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
    Timer { id: measureTimer; interval: 0; repeat: false; onTriggered: cell.reportHeight() }
    onHeightChanged: if (active && !isMedia) measureTimer.restart()
    // Re-measure on RECYCLE too, not just on height change: blocks of a
    // type now render at an identical height (the line-height fix), so a
    // delegate recycling between two same-height blocks fires no
    // onHeightChanged — the new row would never get measured and would
    // keep its (taller) estimate in the Fenwick → a gap below it. Keying
    // off logicalRow ensures every block the delegate shows is measured.
    onLogicalRowChanged: if (active && !isMedia) measureTimer.restart()
    onIsFocusChanged: if (isFocus) editor.focusBlockItem = te
    Component.onCompleted: {
        if (active && !isMedia) measureTimer.restart()
        if (isFocus) editor.focusBlockItem = te
    }

    // highlight spans — overlay rects below the selection (same trick
    // as the code chips: a char-format background would paint above
    // the selection, so selecting highlighted text showed nothing).
    // The overlay rect arrays below are fresh JS arrays on every re-evaluation, and a Repeater
    // regenerates on any new model object — so each Repeater takes the constant 0 while its array
    // is empty (the common case: only a real array resets it), and records compute nothing.
    property var hlRects: {
        var dep = blockModel.contentRevision + blockModel.layoutRevision
        if (!cell.active || cell.isMedia || cell.isRecord) return []
        var ranges = blockModel.highlightRangesForRow(cell.logicalRow)
        var out = []
        for (var i = 0; i < ranges.length; ++i) {
            var rs = editor.selectionRects(te, ranges[i].s, ranges[i].e)
            for (var j = 0; j < rs.length; ++j) out.push({rr: rs[j], col: ranges[i].color})
        }
        return out
    }
    Repeater {
        model: cell.hlRects.length ? cell.hlRects : 0
        delegate: Rectangle {
            required property int index
            readonly property var h: cell.hlRects[index]
            color: h.col
            z: 0
            x: te.x + h.rr.x
            y: te.y + h.rr.y
            width: h.rr.width
            height: h.rr.height
        }
    }

    // Comment-anchor tint: a low-alpha accent wash over commented
    // ranges (the margin pin is the interactive affordance; plain
    // clicks here keep editing text). One rect per visual line.
    property var commentRects: {
        var dep = blockModel.contentRevision + blockModel.layoutRevision
                + blockModel.commentsRevision
        if (!cell.active || cell.isMedia || cell.isRecord) return []
        var ranges = blockModel.commentRangesForRow(cell.logicalRow)
        var out = []
        for (var i = 0; i < ranges.length; ++i) {
            var rs = editor.selectionRects(te, ranges[i].s, ranges[i].e)
            for (var j = 0; j < rs.length; ++j)
                out.push({ r: rs[j], res: ranges[i].resolved === true })
        }
        return out
    }
    Repeater {
        model: cell.commentRects.length ? cell.commentRects : 0
        delegate: Rectangle {
            required property int index
            readonly property var cr2: cell.commentRects[index]
            // Resolved conversations go quiet: a faint grey wash
            // instead of the semantic accent (2026-08-21).
            color: cr2.res ? Qt.rgba(0.55, 0.55, 0.55, 0.10)
                           : Qt.rgba(0.004, 0.537, 0.945, 0.14)
            z: 0
            x: te.x + cr2.r.x
            y: te.y + cr2.r.y
            width: cr2.r.width
            height: cr2.r.height
        }
    }

    // Spell / grammar underlines (0.5.0): dotted tiles under the
    // baseline of every flagged range, the overlay pattern (one
    // QSyntaxHighlighter per document is already taken). The issue
    // under a collapsed caret is withheld while the word is typed.
    property var spellRects: {
        var dep = blockModel.contentRevision + blockModel.layoutRevision + spell.revision
        if (!cell.active || cell.isMedia || cell.isRecord || te.btype === 2 || te.btype === 6) return []
        if (!spell.checkSpelling && !spell.checkGrammar) return []
        var caret = (cell.isFocus && cursor.active && !cursor.hasSel) ? cursor.focusCol : -1
        var issues = spell.issuesForRow(cell.logicalRow, caret)
        var out = []
        for (var i = 0; i < issues.length; ++i) {
            var it = issues[i]
            var rs = editor.selectionRects(te, it.s, Math.min(it.e, te.length))
            for (var j = 0; j < rs.length; ++j) out.push({ r: rs[j], k: it.kind })
        }
        return out
    }
    Repeater {
        model: cell.spellRects.length ? cell.spellRects : 0
        delegate: Image {
            required property int index
            readonly property var sr: cell.spellRects[index]
            source: sr.k === 0 ? Theme.colors.squiggleSpelling : Theme.colors.squiggleGrammar
            fillMode: Image.Tile; smooth: false; z: 0
            x: te.x + sr.r.x; y: te.y + sr.r.y + sr.r.height - 3
            width: Math.max(4, sr.r.width); height: 2
        }
    }

    // inline-code chips — overlay rects (one per visual line of each
    // code range), drawn BELOW the selection so selecting code shows
    // the highlight, and below the glyphs. NOT a char-format
    // background (that paints inside the TextEdit, above selection).
    property var codeRects: {
        var dep = blockModel.contentRevision + blockModel.layoutRevision
        if (!cell.active || cell.isMedia || cell.isRecord) return []
        var ranges = blockModel.codeRangesForRow(cell.logicalRow)
        var out = []
        for (var i = 0; i < ranges.length; ++i) {
            var rs = editor.selectionRects(te, ranges[i].s, ranges[i].e)
            for (var j = 0; j < rs.length; ++j) out.push(rs[j])
        }
        return out
    }
    Repeater {
        model: cell.codeRects.length ? cell.codeRects : 0
        delegate: Rectangle {
            required property int index
            readonly property rect rr: cell.codeRects[index]
            color: Theme.colors.inlineCodeBg
            radius: 0
            z: 0
            x: te.x + rr.x - 2
            y: te.y + rr.y
            width: rr.width + 4
            height: rr.height
        }
    }

    // Inline choice chips (DT-2, 2026-08-20): the table chip's
    // language — option colour at 0.28 fill, 0.55 border — drawn
    // as overlay rects (overlays can't reserve layout width).
    // The label is the block's own text; this is only the pill
    // behind it.
    property var choiceRects: {
        var dep = blockModel.contentRevision + blockModel.layoutRevision
        if (!cell.active || cell.isMedia || cell.isRecord) return []
        if (cell.colKind === 2) return []   // a check cell shows its checkbox, not the chip
        var ranges = blockModel.choiceRangesForRow(cell.logicalRow)
        var out = []
        for (var i = 0; i < ranges.length; ++i) {
            var rs = editor.selectionRects(te, ranges[i].s, ranges[i].e)
            for (var j = 0; j < rs.length; ++j)
                out.push({ r: rs[j], color: ranges[i].color })
        }
        return out
    }
    Repeater {
        model: cell.choiceRects.length ? cell.choiceRects : 0
        delegate: Rectangle {
            required property int index
            readonly property var cr: cell.choiceRects[index]
            readonly property color oc: cr.color !== ""
                ? cr.color : Theme.colors.textMuted
            color: Qt.rgba(oc.r, oc.g, oc.b, 0.28)
            border.width: 1
            border.color: Qt.rgba(oc.r, oc.g, oc.b, 0.55)
            radius: 0
            z: 0
            // 4px pad each side (2026-08-21): the pill can't reserve
            // layout width, so this rides under any directly-adjacent
            // glyph — acceptable for the breathing room.
            x: te.x + cr.r.x - 4
            y: te.y + cr.r.y
            width: cr.r.width + 8
            height: cr.r.height
        }
    }

    // selection highlight (behind text), one rect per visual line.
    property var selRects: {
        var dep = blockModel.contentRevision + blockModel.layoutRevision   // re-eval triggers
        // Opaque rows (media/table/divider) show membership via the
        // wash rectangle below, never via text rects over a hidden te.
        if (!cell.inSel || cell.isMedia || cell.isRecord || te.btype === 6) return []
        if (cell.inTable && editor.cellRect !== null) return []   // a cell rectangle washes whole cells (the record)
        var sp = (cell.logicalRow === cursor.loRow) ? Math.min(cursor.loCol, te.length) : 0
        var ep = (cell.logicalRow === cursor.hiRow) ? Math.min(cursor.hiCol, te.length) : te.length
        return editor.selectionRects(te, sp, ep)
    }
    Repeater {
        model: cell.selRects.length ? cell.selRects : 0
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
        // async poster decode wakes up.
        maxWidth: cell.lane.w      // the lane's width (the page's at top level) — no te.btype dep
        width: implicitWidth
        // Frame height = the model's authoritative value (same as the
        // cell reservation), so the rendered media never disagrees with
        // the reserved space.
        height: (blockModel.layoutRevision, blockModel.contentRevision,
                 blockModel.mediaDisplayHeight(cell.logicalRow))
        // Poster frame: the remembered playhead (0 until first play).
        posterFrame: cell.isVideoMedia
            ? (editor.videoPlayheadRev, editor.videoPlayheadFor(cell.logicalRow)) : 0
        pdfPage: cell.isPdfMedia
            ? (editor.pdfPageRev, editor.pdfPageFor(cell.logicalRow)) : 0
        // Stay the poster (correct frame) until the live surface is
        // ready — avoids a flash of the previous video's stale frame.
        isActivePlayer: cell.logicalRow === editor.videoPlayingRow && editor._videoSurfaceReady
    }

    // Media carries NO selection affordance of its own (ruling
    // 2026-08-19, superseding the accent wash AND the outline that
    // briefly replaced it): the full-row focus fill already says
    // "you are here", and media frames are annotated/interactive
    // surfaces whose content must render untinted. Range
    // membership shows through the row fill + dimmed media bars.


    Rectangle {  // code background — matches the syntax theme's fill
        visible: cell.active && !cell.isMedia && te.btype === 2
        anchors.fill: te; anchors.margins: -8
        z: -1   // behind the selection highlight (else it hides the selection)
        color: codeHl.backgroundColor.a > 0 ? codeHl.backgroundColor : Theme.colors.codeBg
        radius: Theme.dim.radius
        border.width: 1; border.color: Theme.colors.border
    }
    Rectangle {  // code language chip (2026-08-21): the block's language,
        // pinned to the PAGE-measure right edge (code can outgrow the
        // page; the chip stays where the default scroll shows it).
        // Click → the full language picker. No MouseArea — the central
        // mouse layer hit-tests it via cell.langChip.
        id: codeLangChip
        visible: cell.active && !cell.isMedia && te.btype === 2
        readonly property string lname: (blockModel.contentRevision,
            cell.active && te.btype === 2
                ? blockModel.codeLanguageName(cell.logicalRow) : "")
        readonly property bool hot: editor.codeChipHoverRow === cell.logicalRow
        x: cell.colLeft + cell.measure + 8 - width - 6
        y: te.y - 8 + 3
        width: langChipLbl.implicitWidth + 24
        height: 18
        radius: 0
        z: 2
        color: hot ? Theme.colors.surfaceHover : Theme.colors.surface
        border.width: 1; border.color: Theme.colors.border
        Text {
            id: langChipLbl
            anchors.verticalCenter: parent.verticalCenter; x: 7
            text: codeLangChip.lname !== "" ? codeLangChip.lname : "plain"
            color: codeLangChip.lname !== "" ? Theme.colors.textMuted
                                             : Theme.colors.textSubtle
            font.family: Theme.font.mono; font.pixelSize: 11
        }
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right; anchors.rightMargin: 6
            name: "caret-down"; size: 9; color: Theme.colors.textSubtle
        }
    }

    readonly property real colLeft: editor.leftEdge + lane.x + cellInset   // the page's left edge, plus the lane's offset

    TextEdit {
        id: te
        visible: !cell.isMedia && btype !== 6 && btype !== 10   // hidden for divider/record
        opacity: cell.colKind === 2 ? 0 : 1   // a check cell: its checkbox stands in for the text (still laid out)
        readOnly: true
        activeFocusOnPress: false
        selectByMouse: false
        // quote/list get a left indent; the decoration sits in it.
        // Ordered items reserve a 28px number slot + 6px gap —
        // right-aligned numbers up to "999." fit without touching
        // the text (the old 20px slot overflowed from item 10);
        // list nesting adds 24px per depth level.
        readonly property int bdepth: (blockModel.contentRevision,
                                       cell.active && (btype === 5 || btype === 8 || btype === 9)
                                           ? blockModel.depthForRow(cell.logicalRow) : 0)
        readonly property real deco: btype === 9 ? 34 + bdepth * 24
                                   : (btype === 5 || btype === 8) ? 22 + bdepth * 24
                                   : btype === 4 ? 22 : 0
        // task items (type 8): tri-state status 0 todo / 1 doing / 2 done
        readonly property int taskState: (blockModel.contentRevision,
                                          cell.active && btype === 8 ? blockModel.taskStateForRow(cell.logicalRow) : 0)
        x: cell.colLeft + deco
        // Code is UNCAPPED like tables (user ruling: capping code
        // is purely aesthetic and wrapping breaks it): no wrap,
        // natural width — at least the page measure so the themed
        // fill still spans the column for short snippets — and the
        // widest line joins the model's width cache below so the
        // page scrolls horizontally to hold it.
        width: btype === 2 ? Math.max(cell.measure, implicitWidth)
                           : cell.measure - deco
        y: btype === 2 ? 12 : 6   // code: centered in the taller (doubled-margin) cell
        // Quotes are upright Lora (serif + bar + muted colour
        // mark them); italic/bold come from spans so all four faces
        // are reachable, rather than forcing the whole block italic.
        text: (blockModel.contentRevision, cell.active ? blockModel.contentForRow(cell.logicalRow) : "")
        wrapMode: btype === 2 ? TextEdit.NoWrap : TextEdit.Wrap
        horizontalAlignment: cell.colAlign === 1 ? TextEdit.AlignHCenter
                           : cell.colAlign === 2 ? TextEdit.AlignRight : TextEdit.AlignLeft
        textFormat: TextEdit.PlainText
        // Width joins the measure-once cache (the tables' contract):
        // code reports its natural line width (+ the fill's 8px
        // overhang); every OTHER text type reports 0 so a block
        // leaving code releases the width its cached lines held.
        // Tables/media report via their own components.
        function reportW() {
            if (!cell.active || cell.isMedia) return
            blockModel.setMeasuredWidth(cell.logicalRow,
                btype === 2 ? implicitWidth + 16 : 0)
        }
        onImplicitWidthChanged: reportW()
        onBtypeChanged: reportW()
        Component.onCompleted: reportW()
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
        color: cell.cellFg !== "" ? cell.cellFg
             : btype === 1 ? Theme.colors.textBright
             : btype === 2 ? Theme.colors.codeText
             : btype === 4 ? Theme.colors.textMuted   // quote
             : (btype === 8 && taskState === 2) ? Theme.colors.textMuted   // done task
             : Theme.colors.text
        font.family: btype === 2 ? Theme.font.mono
                   : btype === 4 ? Theme.font.serif   // quote → Merriweather
                   : Theme.font.body                  // document body → Aspekta
        font.pixelSize: {
            var _ = blockModel.layoutRevision + blockModel.contentRevision   // deps
            if (btype === 2) return Theme.font.sizeMono
            if (btype !== 1 || !cell.active) return Theme.font.sizeBody
            return headingSizes[Math.max(1, Math.min(6, blockModel.levelForRow(cell.logicalRow)))]
        }
        font.bold: btype === 1 || cell.headerCell
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
        document: (te.btype === 0 || te.btype === 1 || te.btype === 4 || te.btype === 5 || te.btype === 8 || te.btype === 9) ? te.textDocument : null
        enabled: cell.active && (te.btype === 0 || te.btype === 1 || te.btype === 4 || te.btype === 5 || te.btype === 8 || te.btype === 9)
        highlightAsOverlay: true   // hlRects draws them below the selection
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
        visible: cursor.active && cell.isFocus && editor.caretOn && !cursor.hasSel && !cell.isMedia && te.btype !== 6 && te.btype !== 10
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
        radius: 0; color: Theme.colors.quoteBar
    }
    Text {  // list: bullet (hollow at odd depths — cheap level cue)
        visible: cell.active && te.btype === 5
        x: cell.colLeft + 6 + te.bdepth * 24; y: te.y
        text: te.bdepth % 2 ? "◦" : "•"
        color: Theme.colors.textMuted; font.pixelSize: Theme.font.sizeBody
    }
    Text {  // ordered list: computed number, right-aligned before the text
        visible: cell.active && te.btype === 9
        x: cell.colLeft + te.bdepth * 24; width: 28
        y: te.y
        horizontalAlignment: Text.AlignRight
        text: (blockModel.contentRevision,
               cell.active && te.btype === 9
                   ? blockModel.orderedNumberForRow(cell.logicalRow) + "." : "")
        color: Theme.colors.textMuted
        font.family: Theme.font.body; font.pixelSize: Theme.font.sizeBody
    }
    Item {  // task: tri-state checkbox (0 todo / 1 doing / 2 done)
        visible: cell.active && te.btype === 8
        x: cell.colLeft + 2 + te.bdepth * 24; y: te.y + Math.round((te.lineH - 14) / 2)
        width: 14; height: 14
        Rectangle {
            anchors.fill: parent
            radius: 0
            color: te.taskState === 2 ? Theme.colors.accent : "transparent"
            border.width: te.taskState === 2 ? 0 : 1.5
            border.color: te.taskState === 1 ? Theme.colors.accent : Theme.colors.textMuted
        }
        Rectangle {  // in-progress: centred dash
            visible: te.taskState === 1
            anchors.centerIn: parent
            width: 7; height: 2; radius: 0
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
    Item {  // table check cell (SR-4 S5): the painted tri-state box, the task item's recipe
        visible: cell.active && cell.colKind === 2
        x: cell.colLeft + 2; y: te.y + Math.round((te.lineH - 14) / 2)
        width: 14; height: 14
        Rectangle {
            anchors.fill: parent
            radius: 0
            color: cell.checkState === 2 ? Theme.colors.accent : "transparent"
            border.width: cell.checkState === 2 ? 0 : 1.5
            border.color: cell.checkState === 1 ? Theme.colors.accent : Theme.colors.textMuted
        }
        Rectangle {
            visible: cell.checkState === 1
            anchors.centerIn: parent
            width: 7; height: 2; radius: 0
            color: Theme.colors.accent
        }
        Text {
            visible: cell.checkState === 2
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
    Rectangle {  // multi-block band (user ruling 2026-09-09): when the
                 // selection spans more than one block, every row in it
                 // carries a faint full-width band under its content —
                 // the range reads as BLOCKS (the grain copy, delete and
                 // the rail drag act on). Single-row selections stay
                 // text-only; opaque rows add their wash on top.
        // Runs the FULL FIELD like the focused block's fill (page
        // and margins alike) — one row treatment, not two widths.
        visible: cell.active && cell.inSel && cursor.hasSel && cursor.loRow !== cursor.hiRow
                 && !(cell.inTable && editor.cellRect !== null)
                 // a lane block whose split row is selected whole rides its record's full-field band
                 && !(cell.inLane && (blockModel.contentRevision, blockModel.splitRowOf(cell.logicalRow)) >= cursor.loRow)
        z: -0.5                                  // above the block rules + focus fill, below text + rects
        radius: 0
        color: Theme.colors.selectionBand
        // A block in a lane bands only its lane — the neighbouring lane isn't selected.
        x: cell.inLane ? cell.colLeft : 0
        y: 0
        width: cell.inLane ? cell.measure : Math.max(flick.width, editor.contentSpan)
        height: cell.height
    }
    Rectangle {  // opaque-row range wash (2026-09-09): media/table/divider
                 // inside a document selection show membership with a
                 // translucent selection wash OVER the block. Never for
                 // a table's own cell selection (that collapses cursor).
        readonly property bool opaqueRow: te.btype === 3 || te.btype === 6
        visible: cell.active && cell.inSel && cursor.hasSel && opaqueRow
        z: 1
        radius: 0
        color: Theme.colors.selectionWash
        x: cell.colLeft
        y: te.btype === 3 ? 6 : cell.height / 2 - 8
        width: te.btype === 3 ? mediaHost.width : cell.measure
        height: te.btype === 3 ? mediaHost.height : 16
    }

    // (The left-gutter drag grip is GONE — user ruling 2026-07-12,
    // "too much like milkdown": block reorder lives on the
    // right-side ruler's number handles now.)
}
