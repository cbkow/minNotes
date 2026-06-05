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
    property real colWidth: Math.min(width - 40, 760)
    readonly property int overscan: 6
    property Item focusBlockItem: null    // the read-only TextEdit of the focus row
    property bool caretOn: true

    // Mouse drag-select state. dragX is content-x; dragViewY is viewport-y (so
    // edge auto-scroll keeps extending under the held cursor as content moves).
    property bool dragging: false
    property real dragX: 0
    property real dragViewY: 0

    Rectangle { anchors.fill: parent; color: "#ffffff" }
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

        function setCaret(r, col) { anchorRow = r; anchorCol = col; focusRow = r; focusCol = col }
        function move(r, col, extend) {
            focusRow = r; focusCol = col
            if (!extend) { anchorRow = r; anchorCol = col }
            root.ensureVisible(r)
        }
        function deleteSelection() {
            var lr = loRow, lc = loCol
            blockModel.deleteRange(anchorRow, anchorCol, focusRow, focusCol)
            anchorRow = lr; anchorCol = lc; focusRow = lr; focusCol = lc
            root.ensureVisible(lr)
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
            blockModel.insertText(focusRow, focusCol, ch)
            setCaret(focusRow, focusCol + ch.length)
        }
        function splitLine() {
            if (hasSel) deleteSelection()
            blockModel.splitBlock(focusRow, focusCol)
            setCaret(focusRow + 1, 0)
            root.ensureVisible(focusRow + 1)
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
        var fb = root.focusBlockItem, n = blockModel.count
        if (fb && cursor.focusCol < fb.length) cursor.move(cursor.focusRow, cursor.focusCol + 1, shift)
        else if (cursor.focusRow < n - 1) cursor.move(cursor.focusRow + 1, 0, shift)
    }
    function navLeft(shift) {
        if (cursor.focusCol > 0) cursor.move(cursor.focusRow, cursor.focusCol - 1, shift)
        else if (cursor.focusRow > 0)
            cursor.move(cursor.focusRow - 1, blockModel.contentForRow(cursor.focusRow - 1).length, shift)
    }
    function navDown(shift) {
        var fb = root.focusBlockItem, n = blockModel.count
        if (!fb) return
        var r = fb.positionToRectangle(Math.min(cursor.focusCol, fb.length))
        var lh = r.height > 0 ? r.height : 18
        if (r.y < fb.contentHeight - lh * 1.5) cursor.move(cursor.focusRow, fb.positionAt(r.x, r.y + lh * 1.5), shift)
        else if (cursor.focusRow < n - 1) cursor.move(cursor.focusRow + 1, 0, shift)
    }
    function navUp(shift) {
        var fb = root.focusBlockItem
        if (!fb) return
        var r = fb.positionToRectangle(Math.min(cursor.focusCol, fb.length))
        var lh = r.height > 0 ? r.height : 18
        if (r.y > lh * 0.5) cursor.move(cursor.focusRow, fb.positionAt(r.x, r.y - lh * 0.5), shift)
        else if (cursor.focusRow > 0)
            cursor.move(cursor.focusRow - 1, blockModel.contentForRow(cursor.focusRow - 1).length, shift)
    }

    Keys.onPressed: (event) => {
        var shift = (event.modifiers & Qt.ShiftModifier) !== 0
        var k = event.key
        if (k === Qt.Key_Right) { navRight(shift); event.accepted = true }
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
                height: 12 + (isMedia ? root.colWidth * 0.5 : te.implicitHeight)

                onHeightChanged: if (active) blockModel.setMeasuredHeight(logicalRow, height)
                onIsFocusChanged: if (isFocus) root.focusBlockItem = te
                Component.onCompleted: {
                    if (active) blockModel.setMeasuredHeight(logicalRow, height)
                    if (isFocus) root.focusBlockItem = te
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
                        color: "#b3d4fc"
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
                    anchors.fill: te; anchors.margins: -6
                    color: "#1e1e28"; radius: 4
                }

                TextEdit {
                    id: te
                    visible: !cell.isMedia
                    readOnly: true
                    activeFocusOnPress: false
                    selectByMouse: false
                    x: (parent.width - width) / 2
                    width: root.colWidth
                    y: 6
                    text: (blockModel.contentRevision, cell.active ? blockModel.contentForRow(cell.logicalRow) : "")
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.PlainText
                    readonly property int btype: cell.active ? blockModel.typeForRow(cell.logicalRow) : 0
                    color: btype === 2 ? "#d4d4e8" : "#1a1a22"
                    font.family: btype === 2 ? "Menlo" : "Helvetica Neue"
                    font.pixelSize: btype === 1 ? 26 : 15
                    font.bold: btype === 1
                }

                Rectangle {  // caret
                    visible: cell.isFocus && root.caretOn && !cursor.hasSel && !cell.isMedia
                    color: "#1a1a22"
                    width: 2
                    property rect cr: te.positionToRectangle(Math.min(cursor.focusCol, te.length))
                    x: te.x + cr.x
                    y: te.y + cr.y
                    height: cr.height > 0 ? cr.height : 18
                    z: 2
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
            cursorShape: Qt.IBeamCursor

            onPressed: (m) => {
                root.forceActiveFocus()
                var h = root.hitTest(m.x, m.y)
                if (m.modifiers & Qt.ShiftModifier) cursor.move(h.row, h.col, true)
                else cursor.setCaret(h.row, h.col)
                root.dragging = true
                root.dragX = m.x; root.dragViewY = m.y - flick.contentY
            }
            onPositionChanged: (m) => {
                if (!root.dragging) return
                root.dragX = m.x; root.dragViewY = m.y - flick.contentY
                var h = root.hitTest(m.x, m.y)
                cursor.move(h.row, h.col, true)
            }
            onReleased: root.dragging = false
            onCanceled: root.dragging = false
            onDoubleClicked: (m) => {
                // select the word under the cursor
                var h = root.hitTest(m.x, m.y)
                var t = blockModel.contentForRow(h.row)
                var s = h.col, e = h.col
                while (s > 0 && /\w/.test(t.charAt(s - 1))) s--
                while (e < t.length && /\w/.test(t.charAt(e))) e++
                cursor.setCaret(h.row, s); cursor.move(h.row, e, true)
            }
        }

        // Edge auto-scroll while drag-selecting near the top/bottom of the view.
        Timer {
            interval: 16; repeat: true; running: root.dragging
            onTriggered: {
                var margin = 44, sp = 0
                if (root.dragViewY < margin) sp = -Math.max(6, margin - root.dragViewY)
                else if (root.dragViewY > flick.height - margin) sp = Math.max(6, root.dragViewY - (flick.height - margin))
                if (sp === 0) return
                flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + sp))
                var cy = root.dragViewY + flick.contentY      // content point under the held cursor
                var h = root.hitTest(root.dragX, cy)
                cursor.move(h.row, h.col, true)
            }
        }

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }
    }
}
