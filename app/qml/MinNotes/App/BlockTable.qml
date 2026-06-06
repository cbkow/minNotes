import QtQuick

// Passive render of a table block — a grid of read-only cells driven entirely by
// BlockModel's table query seam (re-queried on contentRevision). NO interaction
// here: keys and mouse route through Editor's central handlers in later phases,
// exactly like every other block is a passive layout surface. Named BlockTable
// (not TableView) to avoid clashing with QtQuick.Controls.TableView.
//
// Layout is a strict one-way pipeline so heights never form a binding loop:
// column widths are concrete numbers (stored, else a default) → each cell wraps
// at its column width and reports an intrinsic implicitHeight → the row height is
// the max of its cells → the table's implicitHeight is the sum of rows. The block
// delegate reads that implicitHeight; nothing feeds back into width.
Item {
    id: tv
    property int  logicalRow: -1
    property bool active: false
    property real maxWidth: 760            // available width handed down by the editor

    // Focus + in-cell caret/selection, driven by the editor's table sub-cursor.
    property bool focused: false
    property bool caretOn: true
    property int  focusR: -1
    property int  focusC: -1
    property int  caretPos: 0
    property int  selFrom: 0
    property int  selTo: 0
    // Rectangular cell-range selection (−1 = none); inclusive corners.
    property int  rangeR0: -1
    property int  rangeC0: -1
    property int  rangeR1: -1
    property int  rangeC1: -1
    function inRange(r, c) {
        if (rangeR0 < 0) return false
        return r >= Math.min(rangeR0, rangeR1) && r <= Math.max(rangeR0, rangeR1)
            && c >= Math.min(rangeC0, rangeC1) && c <= Math.max(rangeC0, rangeC1)
    }

    readonly property int  defaultColWidth: 160
    readonly property int  cellPadH: 8
    readonly property int  cellPadV: 5

    readonly property int _rev: blockModel.contentRevision   // re-query trigger
    readonly property int rowCount:   active ? (tv._rev, blockModel.tableRows(logicalRow)) : 0
    readonly property int colCount:   active ? (tv._rev, blockModel.tableColumns(logicalRow)) : 0
    readonly property int headerRows: active ? (tv._rev, blockModel.tableHeaderRows(logicalRow)) : 0

    // Width auto-sizing: only MANUAL widths (stored >0 in the grid) are persisted;
    // a 0 means auto, computed live from content here so it never pollutes undo.
    // Capped at maxColW so a long pasted column wraps instead of scrolling forever.
    readonly property int minColW: 48
    readonly property int maxColW: 360
    // A live resize preview (one column) while dragging a border (set by the editor).
    property int resizeCol: -1
    property int resizeW: 0

    TextMetrics { id: metrics; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody }
    // Per-column auto width = widest cell line (capped). Computed IMPERATIVELY (a
    // binding would self-loop: it writes metrics.text and reads metrics.advanceWidth).
    property var autoW: []
    function recomputeAutoW() {
        if (!active) return
        var ws = []
        for (var c = 0; c < colCount; ++c) {
            var mw = 0
            for (var r = 0; r < rowCount; ++r) {
                var lines = blockModel.tableCell(logicalRow, r, c).split("\n")
                for (var li = 0; li < lines.length; ++li) {
                    metrics.text = lines[li]
                    if (metrics.advanceWidth > mw) mw = metrics.advanceWidth
                }
            }
            ws.push(Math.round(Math.max(minColW, Math.min(maxColW, mw + 2 * cellPadH + 6))))
        }
        autoW = ws
    }
    onActiveChanged: recomputeAutoW()
    onColCountChanged: recomputeAutoW()
    onRowCountChanged: recomputeAutoW()
    Component.onCompleted: recomputeAutoW()
    Connections {
        target: blockModel
        function onContentChangedSpike() { if (tv.active) tv.recomputeAutoW() }
    }

    function colW(c) {
        if (c === resizeCol) return resizeW                       // live drag preview
        var w = blockModel.tableColWidth(logicalRow, c)
        if (w > 0) return w                                       // manual / pinned
        return (autoW && c < autoW.length) ? autoW[c] : defaultColWidth
    }
    readonly property real contentW: {
        var s = 0
        for (var c = 0; c < colCount; ++c) s += tv.colW(c)
        return s + 1                        // + the shared left border
    }

    implicitWidth:  Math.min(contentW, maxWidth)
    implicitHeight: grid.implicitHeight + 1

    // Scroll position, driven by the editor's root-overlay scrollbar (an inner
    // ScrollBar would sit under the document mouse layer and never get drags).
    property alias scrollX: hflick.contentX
    readonly property bool overflowing: contentW > width

    // Hit-test a point (in this item's coords) → {r, c, pos}. Used by the editor's
    // central mouse handler to place the table caret (delegates can't own a
    // MouseArea — the document's mouse layer sits above them).
    function cellAtPoint(px, py) {
        var cx = px + hflick.contentX, accX = 0, c = colCount - 1
        for (var i = 0; i < colCount; ++i) { accX += tv.colW(i); if (cx < accX) { c = i; break } }
        var accY = 0, r = rowCount - 1
        for (var j = 0; j < rowCount; ++j) {
            var ri = rowRep.itemAt(j); var h = ri ? ri.rowHeight : 24
            if (py < accY + h) { r = j; break }
            accY += h
        }
        var pos = 0
        var rowItem = rowRep.itemAt(r)
        var te = rowItem ? rowItem.cellTe(c) : null
        if (te) { var lp = te.mapFromItem(tv, px, py); pos = te.positionAt(lp.x, lp.y) }
        return { r: Math.max(0, r), c: Math.max(0, c), pos: pos }
    }

    // Column-resize geometry (px in this item's coords; accounts for scroll).
    function columnLeftX(c) { var x = 0; for (var i = 0; i < c; ++i) x += tv.colW(i); return x }
    function columnBorderAt(px) {     // → column index if px is near its right border, else −1
        var cx = px + hflick.contentX, acc = 0
        for (var c = 0; c < colCount; ++c) { acc += tv.colW(c); if (Math.abs(cx - acc) <= 5) return c }
        return -1
    }
    function widthForDrag(c, px) {     // new width as the border is dragged to px
        return Math.round(Math.max(minColW, Math.min(800, (px + hflick.contentX) - columnLeftX(c))))
    }

    Flickable {
        id: hflick
        anchors.fill: parent
        contentWidth: tv.contentW
        contentHeight: grid.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick
        interactive: tv.contentW > tv.width    // horizontal scroll only when overflowing

        // outer frame top + left edges (cells supply right + bottom)
        Rectangle { x: 0; y: 0; width: grid.width; height: 1; color: Theme.colors.border; z: 1 }
        Rectangle { x: 0; y: 0; width: 1; height: grid.height; color: Theme.colors.border; z: 1 }

        Column {
            id: grid
            Repeater {
                id: rowRep
                model: tv.rowCount
                delegate: Row {
                    id: gridRow
                    required property int index
                    readonly property int r: index
                    function cellTe(c) { var it = cellRep.itemAt(c); return it ? it.teItem : null }
                    // Row height = tallest cell's content, computed into a PLAIN
                    // number (not the positioner's implicitHeight, which derives
                    // from child heights — binding a cell's height to it loops).
                    property real rowHeight: 8
                    function recompute() {
                        var m = 8
                        for (var i = 0; i < cellRep.count; ++i) {
                            var it = cellRep.itemAt(i)
                            if (it) m = Math.max(m, it.contentH)
                        }
                        rowHeight = m
                    }
                    Repeater {
                        id: cellRep
                        model: tv.colCount
                        delegate: Rectangle {
                            id: cellRect
                            required property int index
                            readonly property int c: index
                            readonly property bool isHeader: gridRow.r < tv.headerRows
                            readonly property bool isFocusedCell: tv.focused && gridRow.r === tv.focusR && c === tv.focusC
                            readonly property bool isSelected: tv.inRange(gridRow.r, c)
                            readonly property real contentH: cellText.implicitHeight + 2 * tv.cellPadV
                            property alias teItem: cellText
                            width: tv.colW(c)
                            height: gridRow.rowHeight
                            onContentHChanged: gridRow.recompute()
                            Component.onCompleted: gridRow.recompute()
                            color: isSelected ? Theme.colors.selectionBg
                                 : isHeader   ? Theme.colors.surfaceHover : "transparent"
                            // Single-hairline grid: each cell draws only its right
                            // + bottom edge; the table frame draws the top + left.
                            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.colors.border }
                            Rectangle { anchors.bottom: parent.bottom; height: 1; width: parent.width; color: Theme.colors.border }

                            // in-cell selection highlight (behind glyphs); single
                            // visual line — fine for short cell text.
                            Rectangle {
                                visible: cellRect.isFocusedCell && tv.selFrom !== tv.selTo
                                z: -1; color: Theme.colors.selectionBg
                                readonly property rect a: cellText.positionToRectangle(Math.min(tv.selFrom, cellText.length))
                                readonly property rect b: cellText.positionToRectangle(Math.min(tv.selTo, cellText.length))
                                x: cellText.x + a.x; y: cellText.y + a.y
                                width: Math.max(2, b.x - a.x)
                                height: a.height > 0 ? a.height : 18
                            }
                            TextEdit {
                                id: cellText
                                x: tv.cellPadH; y: tv.cellPadV
                                width: parent.width - 2 * tv.cellPadH
                                readOnly: true; selectByMouse: false; activeFocusOnPress: false
                                textFormat: TextEdit.PlainText
                                wrapMode: TextEdit.Wrap
                                text: (blockModel.contentRevision,
                                       blockModel.tableCell(tv.logicalRow, gridRow.r, cellRect.c))
                                color: Theme.colors.text
                                font.family: Theme.font.family
                                font.pixelSize: Theme.font.sizeBody
                                font.bold: cellRect.isHeader
                                horizontalAlignment: {
                                    var a = (tv._rev, blockModel.tableColAlign(tv.logicalRow, cellRect.c))
                                    return a === 1 ? TextEdit.AlignHCenter
                                         : a === 2 ? TextEdit.AlignRight : TextEdit.AlignLeft
                                }
                            }
                            // in-cell caret (focused cell, no selection)
                            Rectangle {
                                visible: cellRect.isFocusedCell && tv.caretOn && tv.selFrom === tv.selTo
                                z: 1; color: Theme.colors.accent; width: 2
                                readonly property rect cr: cellText.positionToRectangle(Math.min(tv.caretPos, cellText.length))
                                x: cellText.x + cr.x; y: cellText.y + cr.y
                                height: cr.height > 0 ? cr.height : 18
                            }
                        }
                    }
                }
            }
        }
    }

    // Right-edge indicator when the grid is clipped by the column width (i.e. more
    // table off to the right): gives the clip a clean edge and cues the scroll.
    // Hidden once scrolled to the far end (the real grid border takes over there).
    Rectangle {
        visible: tv.overflowing && hflick.contentX < tv.contentW - tv.width - 0.5
        anchors.right: parent.right
        y: 0; width: 2; height: grid.implicitHeight
        color: Theme.colors.border
    }
}
