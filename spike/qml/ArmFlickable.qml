import QtQuick
import QtQuick.Controls

// Arm B — Flickable + custom-positioned delegates driven by the Fenwick index.
// contentHeight IS the true total height, so the scrollbar is exact by
// construction (P3). A fixed pool of delegates is recycled by remapping each
// slot to logicalRow = firstRow + slot as contentY moves — no create/destroy.
// We own the visible-range calc and the anti-jump contentY compensation (P5).
Item {
    id: root
    property real colWidth: Math.min(width - 40, 760)
    readonly property int overscan: 6

    readonly property int firstVisible: blockModel.rowForY(flick.contentY)
    readonly property int lastVisible: Math.min(blockModel.count - 1,
                                       blockModel.rowForY(flick.contentY + flick.height - 1))
    readonly property int firstRow: Math.max(0, firstVisible - overscan)
    // Pool sized for the worst case (all short rows) + overscan; tall blocks
    // only ever need fewer, so an oversized pool is safe.
    readonly property int poolSize: Math.min(blockModel.count,
                                    Math.ceil(root.height / 38) + 2 * overscan + 4)
    readonly property int delegateCount: poolSize
    readonly property real barFraction: flick.contentHeight > flick.height
        ? flick.contentY / (flick.contentHeight - flick.height) : 0
    readonly property real trueFraction: barFraction   // exact: contentHeight == Fenwick total

    Rectangle { anchors.fill: parent; color: "#ffffff" }   // light page (dark-mode safe)

    function jumpToEnd() { flick.contentY = Math.max(0, flick.contentHeight - flick.height) }
    function jumpToStart() { flick.contentY = 0 }
    property alias scrollY: flick.contentY
    readonly property real maxScrollY: Math.max(0, flick.contentHeight - flick.height)

    // --- Logical cursor (P7/P8). Lives here, not in any delegate: a delegate
    // reflects it only while its logicalRow matches. Survives the target block
    // being unrealized — move() scrolls it in, then the recycled delegate claims
    // focus via onRowChanged.
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
    }

    function ensureVisible(rowIdx) {
        var y = blockModel.yForRow(rowIdx)
        var h = blockModel.heightForRow(rowIdx)
        if (y < flick.contentY) flick.contentY = y
        else if (y + h > flick.contentY + flick.height)
            flick.contentY = Math.min(flick.contentHeight - flick.height, y + h - flick.height)
    }

    // HUD telemetry for caret/selection (P7/P8).
    readonly property int caretRow: cursor.focusRow
    readonly property int caretCol: cursor.focusCol
    readonly property bool hasSelection: cursor.loRow !== cursor.hiRow || cursor.loCol !== cursor.hiCol
    readonly property string selSummary: hasSelection
        ? ("r" + cursor.loRow + ":" + cursor.loCol + " → r" + cursor.hiRow + ":" + cursor.hiCol
           + "  (" + (cursor.hiRow - cursor.loRow + 1) + " blocks)")
        : ("caret r" + cursor.focusRow + ":" + cursor.focusCol)

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: blockModel.totalHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        // Anti-jump (P5): if a block ABOVE the viewport settles to a new height,
        // shift contentY by the same delta so on-screen content stays put.
        Connections {
            target: blockModel
            function onHeightSettled(row, delta) {
                if (row < root.firstVisible) flick.contentY += delta
            }
        }

        Repeater {
            model: root.poolSize
            delegate: BlockDelegate {
                required property int index            // pool slot 0..poolSize-1
                // Modulo slot-recycling: slot owns the row in the visible window
                // with row % poolSize == its phase, so advancing firstRow by one
                // rebinds exactly ONE slot (the one that scrolled off), not all
                // ~40. This is the recycling ListView does internally; the naive
                // "firstRow + index" remaps every slot every frame → 40ms hitch.
                readonly property int logicalRow: root.firstRow
                    + (((index - root.firstRow) % root.poolSize) + root.poolSize) % root.poolSize
                readonly property bool active: logicalRow >= 0 && logicalRow < blockModel.count
                row: active ? logicalRow : 0
                blockType: active ? blockModel.typeForRow(logicalRow) : 0
                // contentRevision dependency: re-read text after an edit/delete
                // without disturbing position (which keys off layoutRevision).
                blockText: (blockModel.contentRevision, active ? blockModel.contentForRow(logicalRow) : "")
                contentWidth: root.colWidth
                cursorCtl: cursor
                visible: active
                x: (flick.width - width) / 2
                // y from the index; reference layoutRevision so it re-evaluates
                // when any height settles (comma operator: depend, then return).
                y: (blockModel.layoutRevision, active ? blockModel.yForRow(logicalRow) : 0)
                height: implicitHeight
            }
        }

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }
    }
}
