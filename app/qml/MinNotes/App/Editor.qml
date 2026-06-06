import QtQuick
import QtQuick.Controls

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
    Component.onCompleted: forceActiveFocus()
    property real colWidth: Math.min(width - 40, Theme.dim.columnWidth)
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
    readonly property real gutterX: (flick.width - colWidth) / 2   // == cell.colLeft

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
            var lr = loRow, lc = loCol
            blockModel.deleteRange(anchorRow, anchorCol, focusRow, focusCol)
            anchorRow = lr; anchorCol = lc; focusRow = lr; focusCol = lc
            goalX = -1
            root.ensureVisible(lr)
            sync()
        }
        function backspace() {
            if (hasSel) { deleteSelection(); return }
            if (focusCol > 0) {
                blockModel.deleteRange(focusRow, focusCol - 1, focusRow, focusCol)
                setCaret(focusRow, focusCol - 1)
            } else if (focusRow > 0) {
                var pl = blockModel.contentForRow(focusRow - 1).length
                blockModel.deleteRange(focusRow - 1, pl, focusRow, 0)
                setCaret(focusRow - 1, pl)
                root.ensureVisible(focusRow - 1)
            }
        }
        function insertChar(ch) {
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
        if (y < flick.contentY) flick.contentY = y
        else if (y + h > flick.contentY + flick.height)
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

    // --- Central navigation. Uses the focus block's text layout for vertical
    // moves; crosses boundaries at the text edges. Single focus holder → the
    // caret the user sees and the row the keys act on can never diverge.
    function navRight(shift) {
        cursor.resetGoalX(); cursor.clearMarks()
        var fb = root.focusBlockItem, n = blockModel.count
        if (fb && cursor.focusCol < fb.length) cursor.move(cursor.focusRow, cursor.focusCol + 1, shift)
        else if (cursor.focusRow < n - 1) cursor.move(cursor.focusRow + 1, 0, shift)
    }
    function navLeft(shift) {
        cursor.resetGoalX(); cursor.clearMarks()
        if (cursor.focusCol > 0) cursor.move(cursor.focusRow, cursor.focusCol - 1, shift)
        else if (cursor.focusRow > 0)
            cursor.move(cursor.focusRow - 1, blockModel.contentForRow(cursor.focusRow - 1).length, shift)
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
        else if (cursor.focusRow < n - 1)                 // cross into the next block at goal-x
            cursor.move(cursor.focusRow + 1, colAtGoalX(cursor.focusRow + 1, 2), shift)
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
        if (k === Qt.Key_Escape) {
            // Cancel the current op: block-drag (revert, no move) → text-drag →
            // collapse selection → disarm format toggle.
            if (root.blockDragging) { root.blockDragging = false; root.blockDragRow = -1; root.dropGap = -1 }
            else if (root.dragging) { root.dragging = false }
            else if (cursor.hasSel) { cursor.setCaret(cursor.focusRow, cursor.focusCol) }
            else if (cursor.activeMarks !== 0) { cursor.clearMarks() }
            event.accepted = true
        }
        else if (cmd && k === Qt.Key_Z && shift) { blockModel.redo(); event.accepted = true }
        else if (cmd && k === Qt.Key_Z) { blockModel.undo(); event.accepted = true }
        else if (cmd && k === Qt.Key_Y) { blockModel.redo(); event.accepted = true }
        else if (cmd && k === Qt.Key_B) { applyFormat("bold"); event.accepted = true }
        else if (cmd && k === Qt.Key_I) { applyFormat("italic"); event.accepted = true }
        else if (cmd && k === Qt.Key_U) { applyFormat("underline"); event.accepted = true }
        else if (cmd && shift && k === Qt.Key_X) { applyFormat("strike"); event.accepted = true }
        else if (cmd && k === Qt.Key_Backslash) { clearFormatting(); event.accepted = true }
        else if (k === Qt.Key_Right) { navRight(shift); event.accepted = true }
        else if (k === Qt.Key_Left) { navLeft(shift); event.accepted = true }
        else if (k === Qt.Key_Down) { navDown(shift); event.accepted = true }
        else if (k === Qt.Key_Up) { navUp(shift); event.accepted = true }
        else if (k === Qt.Key_Backspace || k === Qt.Key_Delete) { cursor.backspace(); event.accepted = true }
        else if (k === Qt.Key_Return || k === Qt.Key_Enter) { cursor.splitLine(); event.accepted = true }
        else if (event.text.length === 1 && event.text >= " ") { cursor.insertChar(event.text); event.accepted = true }
    }

    Timer { interval: 530; running: true; repeat: true; onTriggered: root.caretOn = !root.caretOn }

    // --- HUD telemetry (same surface as the other arms) ---
    readonly property int firstVisible: blockModel.rowForY(flick.contentY)
    readonly property int lastVisible: Math.min(blockModel.count - 1,
                                       blockModel.rowForY(flick.contentY + flick.height - 1))
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
                readonly property bool isMedia: active && blockModel.typeForRow(logicalRow) === 3
                readonly property bool isFocus: active && logicalRow === cursor.focusRow
                readonly property bool inSel: active && logicalRow >= cursor.loRow && logicalRow <= cursor.hiRow
                readonly property Item teItem: te    // layout oracle, for hit-testing

                width: flick.width
                visible: active
                y: (blockModel.layoutRevision, active ? blockModel.yForRow(logicalRow) : 0)
                height: 12 + (isMedia ? root.colWidth * 0.5 : (te.btype === 6 ? 18 : te.implicitHeight))

                onHeightChanged: if (active) blockModel.setMeasuredHeight(logicalRow, height)
                onIsFocusChanged: if (isFocus) root.focusBlockItem = te
                Component.onCompleted: {
                    if (active) blockModel.setMeasuredHeight(logicalRow, height)
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

                Rectangle {  // media block
                    visible: cell.isMedia
                    anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 6 }
                    height: visible ? width * 0.5 : 0
                    radius: 4
                    color: Qt.hsla((cell.logicalRow % 23) / 23, 0.45, 0.5, 1)
                    Text { anchors.centerIn: parent; color: "white"; text: "▶ " + (cell.active ? blockModel.contentForRow(cell.logicalRow) : ""); font.pixelSize: 14 }
                }

                Rectangle {  // code background
                    visible: cell.active && !cell.isMedia && blockModel.typeForRow(cell.logicalRow) === 2
                    anchors.fill: te; anchors.margins: -8
                    color: Theme.colors.codeBg; radius: Theme.dim.radius
                    border.width: 1; border.color: Theme.colors.border
                }

                readonly property real colLeft: (width - root.colWidth) / 2

                TextEdit {
                    id: te
                    visible: !cell.isMedia && btype !== 6   // hidden for divider
                    readOnly: true
                    activeFocusOnPress: false
                    selectByMouse: false
                    // quote/list get a left indent; the decoration sits in it.
                    readonly property real deco: (btype === 4 || btype === 5) ? 22 : 0
                    x: cell.colLeft + deco
                    width: root.colWidth - deco
                    y: 6
                    // Quotes are upright Merriweather (serif + bar + muted colour
                    // mark them); italic/bold come from spans so all four faces
                    // are reachable, rather than forcing the whole block italic.
                    text: (blockModel.contentRevision, cell.active ? blockModel.contentForRow(cell.logicalRow) : "")
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.PlainText
                    // Revision deps: re-evaluate type/level when autoformat changes
                    // a block in place (Q_INVOKABLE isn't reactive). contentRevision
                    // propagates reliably (the text binding uses it), so include it.
                    readonly property int btype: (blockModel.layoutRevision, blockModel.contentRevision,
                                                  cell.active ? blockModel.typeForRow(cell.logicalRow) : 0)
                    readonly property var headingSizes: [26, 30, 26, 22, 19, 17, 16]   // index by level (1–6)
                    color: btype === 1 ? Theme.colors.textBright
                         : btype === 2 ? Theme.colors.codeText
                         : btype === 4 ? Theme.colors.textMuted   // quote
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
                }

                // Inline markdown styling: applies bold/italic/mono char formats
                // to te's PlainText document and dims the markers in place. No
                // HTML, identity caret positions. Off for code (markdown is
                // literal inside a fence) and non-text blocks.
                InlineMarkdownHighlighter {
                    document: te.textDocument
                    enabled: cell.active && (te.btype === 0 || te.btype === 4 || te.btype === 5)
                    markerColor: Theme.colors.accent
                    selectedMarkerColor: Theme.colors.textBright
                    codeColor: Theme.colors.inlineCodeText
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

                Rectangle {  // caret
                    visible: cursor.active && cell.isFocus && root.caretOn && !cursor.hasSel && !cell.isMedia && te.btype !== 6
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
                Rectangle {  // divider: horizontal rule
                    visible: cell.active && te.btype === 6
                    x: cell.colLeft; y: cell.height / 2 - 1
                    width: root.colWidth; height: 1
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
            acceptedButtons: Qt.LeftButton
            preventStealing: true
            hoverEnabled: true
            property bool overGrip: false
            cursorShape: root.blockDragging ? Qt.ClosedHandCursor
                       : (overGrip ? Qt.OpenHandCursor : Qt.IBeamCursor)

            onPressed: (m) => {
                root.forceActiveFocus()
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
                var h = root.hitTest(m.x, m.y)
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
                if (root.dragging) {
                    root.dragX = m.x; root.dragViewY = m.y - flick.contentY
                    var h = root.hitTest(m.x, m.y)
                    cursor.move(h.row, h.col, true)
                    return
                }
                // hover (not pressed): light the row's grip; gutter → grab cursor.
                root.hoverRow = blockModel.rowForY(m.y)
                mouse.overGrip = root.inGutter(m.x)
            }
            onExited: { root.hoverRow = -1; mouse.overGrip = false }
            onReleased: { if (root.blockDragging) root.commitBlockDrag(); else root.dragging = false }
            onCanceled: {
                if (root.blockDragging) { root.blockDragging = false; root.blockDragRow = -1; root.dropGap = -1 }
                else root.dragging = false
            }
            onDoubleClicked: (m) => {
                // End the press-drag the 2nd press armed, so a tiny mouse jitter
                // before release can't re-extend the selection back to the click
                // point (which collapsed the word to word-start→cursor).
                root.dragging = false
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

    // --- Block-drag overlays (viewport-fixed, on top of the document) ---
    // Drop-indicator line at the insertion gap.
    Rectangle {
        visible: root.blockDragging && root.dropGap >= 0
        x: root.gutterX; width: root.colWidth; height: 2; radius: 1
        y: (blockModel.layoutRevision, root.gapY(root.dropGap)) - flick.contentY - 1
        color: Theme.colors.accent
        z: 50
    }
    // Floating ghost of the dragged block — an accent outline with a barely-there
    // translucent fill (the document shows through), following the cursor.
    Rectangle {
        visible: root.blockDragging
        x: root.gutterX; width: root.colWidth; height: 30
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
}
