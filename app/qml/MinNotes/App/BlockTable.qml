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

    // Context-menu target highlight: "" none, "column" or "row" at hiIndex; danger
    // tints it red. Driven by the editor when a table menu item is hovered.
    property string hiScope: ""
    property int    hiIndex: -1
    property bool   hiDanger: false

    // Drag-drop target cell (an image being dragged over it); −1 = none.
    property int dropR: -1
    property int dropC: -1

    // Header sort affordance: every first-header-row cell reserves a right-edge
    // slot with a sort glyph (the CLICK zone lives in the editor's mouse
    // handlers; this is display only). sortCol marks the last-sorted column of
    // this table (session state, passed in) with its direction in accent.
    property int  sortCol: -1
    property bool sortAsc: true

    TextMetrics { id: metrics; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody }
    // Per-column auto width = widest cell line (capped). Computed IMPERATIVELY (a
    // binding would self-loop: it writes metrics.text and reads metrics.advanceWidth).
    property var autoW: []
    function recomputeAutoW() {
        if (!active) return
        var ws = []
        for (var c = 0; c < colCount; ++c) {
            var kind = blockModel.tableColumnKind(logicalRow, c)
            var mw = 0
            // Header rows stay text in every column kind; body cells only carry
            // measurable text in a text column (choice/check render a chip/glyph).
            // The first header row also reserves the sort-glyph slot.
            var textRows = (kind === 0) ? rowCount : headerRows
            for (var r = 0; r < textRows; ++r) {
                var pad = (r === 0 && headerRows > 0) ? 18 : 0
                var lines = blockModel.tableCell(logicalRow, r, c).split("\n")
                for (var li = 0; li < lines.length; ++li) {
                    metrics.text = lines[li]
                    if (metrics.advanceWidth + pad > mw) mw = metrics.advanceWidth + pad
                }
            }
            if (kind === 1) {
                // Choice column: fit the widest option's chip (label + chip pad 16,
                // minus the 6 the shared formula re-adds) so chips never elide.
                var opts = blockModel.tableColumnOptions(logicalRow, c)
                for (var oi = 0; oi < opts.length; ++oi) {
                    metrics.text = opts[oi].label
                    if (metrics.advanceWidth + 10 > mw) mw = metrics.advanceWidth + 10
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

    // Display width of a cell image (matches the cell delegate's imgW): an explicit
    // override capped to the column, else the intrinsic width capped to the column.
    function cellImageDispW(r, c) {
        var colInner = tv.colW(c) - 2 * cellPadH
        var iw = blockModel.tableCellMediaW(logicalRow, r, c)
        var dw = blockModel.tableCellMediaDw(logicalRow, r, c)
        return dw > 0 ? Math.min(dw, colInner) : Math.min(colInner, iw > 0 ? iw : colInner)
    }
    // The cell image's rect in THIS item's coords (accounts for h-scroll) — used by
    // the editor to place the root-overlay resize handles. Empty if no image.
    function cellImageRect(r, c) {
        if (blockModel.tableCellMedia(logicalRow, r, c) === "") return Qt.rect(0, 0, 0, 0)
        var iw = blockModel.tableCellMediaW(logicalRow, r, c)
        var ih = blockModel.tableCellMediaH(logicalRow, r, c)
        var w = cellImageDispW(r, c)
        var h = (iw > 0 && ih > 0) ? Math.round(w * ih / iw) : 0
        var x = columnLeftX(c) - hflick.contentX + cellPadH
        var y = rowTopY(r) + cellPadV
        return Qt.rect(x, y, w, h)
    }

    // Column-resize geometry (px in this item's coords; accounts for scroll).
    function columnLeftX(c) { var x = 0; for (var i = 0; i < c; ++i) x += tv.colW(i); return x }
    function rowTopY(r) { var y = 0; for (var j = 0; j < r; ++j) { var it = rowRep.itemAt(j); y += it ? it.rowHeight : 24 } return y }
    function rowHeightAt(r) { var it = rowRep.itemAt(r); return it ? it.rowHeight : 24 }
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
                            // Optional inline image: descriptor + resolved URL + intrinsic
                            // dims (known up front, so the row reserves height with no jump).
                            readonly property string cmedia: (blockModel.contentRevision,
                                                              blockModel.tableCellMedia(tv.logicalRow, gridRow.r, c))
                            readonly property string cmediaUrl: cmedia !== ""
                                ? (blockModel.contentRevision, blockModel.tableCellMediaUrl(tv.logicalRow, gridRow.r, c)) : ""
                            readonly property int cmediaW: cmedia !== "" ? blockModel.tableCellMediaW(tv.logicalRow, gridRow.r, c) : 0
                            readonly property int cmediaH: cmedia !== "" ? blockModel.tableCellMediaH(tv.logicalRow, gridRow.r, c) : 0
                            readonly property int cmediaDw: cmedia !== "" ? (blockModel.contentRevision,
                                                            blockModel.tableCellMediaDw(tv.logicalRow, gridRow.r, c)) : 0
                            // Display width: an explicit per-image override (capped to the
                            // column), else the intrinsic width capped to the column.
                            readonly property real imgW: cmedia !== ""
                                ? (cmediaDw > 0 ? Math.min(cmediaDw, width - 2 * tv.cellPadH)
                                                : Math.min(width - 2 * tv.cellPadH, cmediaW > 0 ? cmediaW : width)) : 0
                            readonly property real imgH: (cmedia !== "" && cmediaW > 0 && cmediaH > 0)
                                ? Math.round(imgW * cmediaH / cmediaW) : 0
                            readonly property real contentH: cellText.implicitHeight
                                + (imgH > 0 ? imgH + tv.cellPadV : 0) + 2 * tv.cellPadV
                            property alias teItem: cellText
                            // Effective cell colours (cell → row → column cascade), "" = none.
                            readonly property string cellBg: (blockModel.contentRevision,
                                                              blockModel.tableCellBg(tv.logicalRow, gridRow.r, c))
                            readonly property string cellFg: (blockModel.contentRevision,
                                                              blockModel.tableCellFg(tv.logicalRow, gridRow.r, c))
                            // Inline spans for this cell (rich text); drives the
                            // per-cell highlighter, which attaches only when non-empty.
                            readonly property var cspans: (blockModel.contentRevision,
                                                           blockModel.tableCellSpans(tv.logicalRow, gridRow.r, c))
                            // Choice column: body cells render a dropdown chip instead
                            // of editable text. csel = selected option id ("" = none).
                            readonly property int ckind: (blockModel.contentRevision,
                                                          blockModel.tableColumnKind(tv.logicalRow, c))
                            readonly property bool isChoice: ckind === 1 && !isHeader
                            readonly property bool isCheck: ckind === 2 && !isHeader
                            // first header row carries the sort glyph slot
                            readonly property bool sortSlot: isHeader && gridRow.r === 0
                            readonly property string csel: isChoice ? (blockModel.contentRevision,
                                                           blockModel.tableCellChoice(tv.logicalRow, gridRow.r, c)) : ""
                            readonly property string cselLabel: (isChoice && csel !== "")
                                ? blockModel.tableCellChoiceLabel(tv.logicalRow, gridRow.r, c) : ""
                            readonly property string cselColor: (isChoice && csel !== "")
                                ? blockModel.tableCellChoiceColor(tv.logicalRow, gridRow.r, c) : ""
                            readonly property int ccheck: isCheck ? (blockModel.contentRevision,
                                                          blockModel.tableCellCheck(tv.logicalRow, gridRow.r, c)) : 0
                            width: tv.colW(c)
                            height: gridRow.rowHeight
                            onContentHChanged: gridRow.recompute()
                            Component.onCompleted: gridRow.recompute()
                            color: isSelected ? Theme.colors.selectionBg
                                 : cellBg !== "" ? cellBg
                                 : isHeader   ? Theme.colors.surfaceHover : "transparent"
                            // Single-hairline grid: each cell draws only its right
                            // + bottom edge; the table frame draws the top + left.
                            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.colors.border }
                            Rectangle { anchors.bottom: parent.bottom; height: 1; width: parent.width; color: Theme.colors.border }

                            // Drag-drop target: an image being dragged onto this cell.
                            Rectangle {
                                visible: tv.dropR === gridRow.r && tv.dropC === cellRect.c
                                anchors.fill: parent; z: 3
                                color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.16)
                                border.width: 2; border.color: Theme.colors.accent
                            }

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
                            Image {   // inline cell image (above any text)
                                id: cellImg
                                visible: cellRect.cmedia !== "" && cellRect.cmediaUrl !== ""
                                x: tv.cellPadH; y: tv.cellPadV
                                width: cellRect.imgW; height: cellRect.imgH
                                source: cellRect.cmediaUrl
                                asynchronous: true; cache: false
                                fillMode: Image.PreserveAspectFit
                                sourceSize.width: Math.round(cellRect.imgW * Screen.devicePixelRatio)
                                smooth: true
                            }
                            // Selected-state tint over a cell image (it's opaque, like
                            // a media block — its selection needs its own affordance).
                            Rectangle {
                                visible: cellRect.cmedia !== "" && (cellRect.isFocusedCell || cellRect.isSelected)
                                x: cellImg.x; y: cellImg.y; width: cellImg.width; height: cellImg.height
                                z: 2
                                color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.22)
                            }
                            TextEdit {
                                id: cellText
                                visible: !cellRect.isChoice && !cellRect.isCheck   // typed cells render their own glyph
                                x: tv.cellPadH
                                y: tv.cellPadV + (cellRect.imgH > 0 ? cellRect.imgH + tv.cellPadV : 0)
                                width: parent.width - 2 * tv.cellPadH - (cellRect.sortSlot ? 18 : 0)
                                readOnly: true; selectByMouse: false; activeFocusOnPress: false
                                textFormat: TextEdit.PlainText
                                wrapMode: TextEdit.Wrap
                                text: (blockModel.contentRevision,
                                       blockModel.tableCell(tv.logicalRow, gridRow.r, cellRect.c))
                                color: cellRect.cellFg !== "" ? cellRect.cellFg : Theme.colors.text
                                font.family: Theme.font.family
                                font.pixelSize: Theme.font.sizeBody
                                font.bold: cellRect.isHeader
                                horizontalAlignment: {
                                    var a = (tv._rev, blockModel.tableColAlign(tv.logicalRow, cellRect.c))
                                    return a === 1 ? TextEdit.AlignHCenter
                                         : a === 2 ? TextEdit.AlignRight : TextEdit.AlignLeft
                                }
                            }
                            // Per-cell inline formatting (bold/italic/underline/strike/
                            // code/link), rendered the same way blocks are. Attaches to
                            // the cell document ONLY when there are spans, so plain cells
                            // carry no idle highlighter.
                            InlineMarkdownHighlighter {
                                document: cellRect.cspans.length > 0 ? cellText.textDocument : null
                                enabled: cellRect.cspans.length > 0
                                markerColor: Theme.colors.accent
                                selectedMarkerColor: Theme.colors.textBright
                                codeColor: Theme.colors.inlineCodeText
                                linkColor: Theme.colors.accent
                                codeFontFamily: Theme.font.mono
                                spans: cellRect.cspans
                            }
                            // Choice cell: a soft colored pill (dropdown chip) showing
                            // the selected option, else a faint caret-down placeholder.
                            // Clicks route through the editor's central handler.
                            Rectangle {
                                visible: cellRect.isChoice
                                x: tv.cellPadH; y: tv.cellPadV
                                height: Math.max(16, cellText.implicitHeight - 2)
                                width: Math.min(parent.width - 2 * tv.cellPadH,
                                                cellRect.csel !== "" ? chipLabel.implicitWidth + 16 : 22)
                                radius: height / 2
                                readonly property color _oc: cellRect.cselColor !== ""
                                    ? Qt.color(cellRect.cselColor) : Theme.colors.textMuted
                                color: cellRect.csel !== "" ? Qt.rgba(_oc.r, _oc.g, _oc.b, 0.28) : "transparent"
                                border.width: 1
                                border.color: cellRect.csel !== "" ? Qt.rgba(_oc.r, _oc.g, _oc.b, 0.55)
                                                                   : Theme.colors.border
                                Text {
                                    id: chipLabel
                                    visible: cellRect.csel !== ""
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: 8
                                    width: Math.min(implicitWidth, parent.width - 14)
                                    elide: Text.ElideRight
                                    text: cellRect.cselLabel
                                    color: Theme.colors.text
                                    font.family: Theme.font.family
                                    font.pixelSize: Theme.font.sizeBody
                                }
                                Icon {
                                    visible: cellRect.csel === ""
                                    anchors.centerIn: parent
                                    name: "caret-down"; size: 12; color: Theme.colors.textMuted
                                }
                            }
                            // Check column: tri-state task checkbox (0 todo / 1 doing
                            // / 2 done) — the same glyph as a block task list. Clicks
                            // cycle it via the editor's central handler.
                            Item {
                                visible: cellRect.isCheck
                                x: tv.cellPadH
                                y: tv.cellPadV + Math.max(0, (cellText.implicitHeight - 14) / 2)
                                width: 14; height: 14
                                Rectangle {
                                    anchors.fill: parent; radius: 3
                                    color: cellRect.ccheck === 2 ? Theme.colors.accent : "transparent"
                                    border.width: cellRect.ccheck === 2 ? 0 : 1.5
                                    border.color: cellRect.ccheck === 1 ? Theme.colors.accent : Theme.colors.textMuted
                                }
                                Rectangle {   // in-progress dash
                                    visible: cellRect.ccheck === 1
                                    anchors.centerIn: parent; width: 7; height: 2; radius: 1
                                    color: Theme.colors.accent
                                }
                                Text {   // done check
                                    visible: cellRect.ccheck === 2
                                    anchors.centerIn: parent
                                    text: "✓"; color: Theme.colors.textBright
                                    font.pixelSize: 11; font.bold: true
                                }
                            }
                            Icon {   // header sort slot (display only; clicks land in the editor)
                                visible: cellRect.sortSlot
                                anchors.right: parent.right; anchors.rightMargin: tv.cellPadH - 2
                                anchors.verticalCenter: parent.verticalCenter
                                name: cellRect.c === tv.sortCol
                                      ? (tv.sortAsc ? "sort-ascending" : "sort-descending")
                                      : "arrows-down-up"
                                size: 12
                                color: cellRect.c === tv.sortCol ? Theme.colors.accent : Theme.colors.textSubtle
                            }
                            // in-cell caret (focused cell, no selection)
                            Rectangle {
                                visible: cellRect.isFocusedCell && tv.caretOn && tv.selFrom === tv.selTo
                                         && !cellRect.isChoice && !cellRect.isCheck
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

        // Context-menu target highlight (column / row); scrolls with the content.
        Rectangle {
            visible: tv.hiScope === "column" && tv.hiIndex >= 0 && tv.hiIndex < tv.colCount
            x: tv.columnLeftX(tv.hiIndex); y: 0
            width: tv.colW(tv.hiIndex); height: grid.implicitHeight; z: 5
            readonly property color _c: tv.hiDanger ? Theme.colors.error : Theme.colors.accent
            color: Qt.rgba(_c.r, _c.g, _c.b, 0.14)
            border.width: 1; border.color: Qt.rgba(_c.r, _c.g, _c.b, 0.6)
        }
        Rectangle {
            visible: tv.hiScope === "row" && tv.hiIndex >= 0 && tv.hiIndex < tv.rowCount
            x: 0; y: tv.rowTopY(tv.hiIndex)
            width: tv.contentW; height: tv.rowHeightAt(tv.hiIndex); z: 5
            readonly property color _c: tv.hiDanger ? Theme.colors.error : Theme.colors.accent
            color: Qt.rgba(_c.r, _c.g, _c.b, 0.14)
            border.width: 1; border.color: Qt.rgba(_c.r, _c.g, _c.b, 0.6)
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
