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

    readonly property int  defaultColWidth: 160
    readonly property int  cellPadH: 8
    readonly property int  cellPadV: 5

    readonly property int _rev: blockModel.contentRevision   // re-query trigger
    readonly property int rowCount:   active ? (tv._rev, blockModel.tableRows(logicalRow)) : 0
    readonly property int colCount:   active ? (tv._rev, blockModel.tableColumns(logicalRow)) : 0
    readonly property int headerRows: active ? (tv._rev, blockModel.tableHeaderRows(logicalRow)) : 0

    function colW(c) {
        var w = blockModel.tableColWidth(logicalRow, c)
        return w > 0 ? w : defaultColWidth
    }
    readonly property real contentW: {
        var s = 0
        for (var c = 0; c < colCount; ++c) s += tv.colW(c)
        return s + 1                        // + the shared left border
    }

    implicitWidth:  Math.min(contentW, maxWidth)
    implicitHeight: grid.implicitHeight + 1

    Flickable {
        id: hflick
        anchors.fill: parent
        contentWidth: tv.contentW
        contentHeight: grid.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick
        interactive: tv.contentW > tv.width    // horizontal scroll only when overflowing

        Column {
            id: grid
            Repeater {
                model: tv.rowCount
                delegate: Row {
                    id: gridRow
                    required property int index
                    readonly property int r: index
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
                            readonly property real contentH: cellText.implicitHeight + 2 * tv.cellPadV
                            width: tv.colW(c)
                            height: gridRow.rowHeight
                            onContentHChanged: gridRow.recompute()
                            Component.onCompleted: gridRow.recompute()
                            color: isHeader ? Theme.colors.surfaceHover : "transparent"
                            border.width: 1; border.color: Theme.colors.border
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
                                    var a = blockModel.tableColAlign(tv.logicalRow, cellRect.c)
                                    return a === 1 ? TextEdit.AlignHCenter
                                         : a === 2 ? TextEdit.AlignRight : TextEdit.AlignLeft
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
