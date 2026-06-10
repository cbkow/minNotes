import QtQuick

// Kanban board over ONE table block, grouped by a choice or checkmark column —
// a second full-frame projection of the same dataset the grid tab shows. Lanes
// are the column's options in option order (+ a trailing "No status" lane for
// unset cells; a check column has fixed To do / Doing / Done lanes). Cards are
// the body rows in document order; a card's title is its first text-column
// cell. Dragging a card to another lane sets the grouping cell; dropping
// between cards also reorders the underlying row — both inside ONE undo step
// (beginGroup/endGroup). This view exists only in the full-frame tab (a
// dedicated mode with no document mouse layer above it), so cards own their
// MouseAreas directly; nothing here routes through the central handler.
Item {
    id: kb
    property int  logicalRow: -1
    property int  groupCol: -1
    property bool active: false
    signal showGrid()                      // emitted when the board can't render

    readonly property int laneW: 260
    readonly property int laneGap: 14
    readonly property int headerH: 34
    readonly property int cardH: 40
    readonly property int cardGap: 8

    // lanes: [{key, label, color, cards:[{r, title}]}]. key = option id for a
    // choice column, "0"/"1"/"2" for a check column, "" = the unset lane.
    // Rebuilt whole (imperatively) on any content change — tables are small and
    // the board is a flat projection, so a full rebuild beats incremental state.
    property var lanes: []
    function recompute() {
        if (!active || groupCol < 0 || logicalRow < 0) { lanes = []; return }
        var row = logicalRow
        var kind = blockModel.tableColumnKind(row, groupCol)
        if (kind === 0) {                  // column reverted to text (undo / edit)
            lanes = []
            if (active) kb.showGrid()
            return
        }
        var hdr = blockModel.tableHeaderRows(row)
        var nr = blockModel.tableRows(row)
        var nc = blockModel.tableColumns(row)
        // Card title column: the first text column that isn't the grouping one.
        var tc = -1
        for (var c = 0; c < nc; ++c)
            if (c !== groupCol && blockModel.tableColumnKind(row, c) === 0) { tc = c; break }
        var ls = []
        if (kind === 2) {
            ls = [{key: "0", label: "To do", color: "", cards: []},
                  {key: "1", label: "Doing", color: "", cards: []},
                  {key: "2", label: "Done",  color: "", cards: []}]
        } else {
            var opts = blockModel.tableColumnOptions(row, groupCol)
            for (var i = 0; i < opts.length; ++i)
                ls.push({key: opts[i].id, label: opts[i].label, color: opts[i].color, cards: []})
            ls.push({key: "", label: "No status", color: "", cards: []})
        }
        var byKey = {}
        for (var li = 0; li < ls.length; ++li) byKey[ls[li].key] = ls[li]
        for (var r = hdr; r < nr; ++r) {
            var key = kind === 2 ? String(blockModel.tableCellCheck(row, r, groupCol))
                                 : blockModel.tableCellChoice(row, r, groupCol)
            var lane = byKey[key] !== undefined ? byKey[key] : byKey[""]
            if (lane === undefined) continue
            var title = tc >= 0 ? blockModel.tableCell(row, r, tc) : ""
            lane.cards.push({r: r, title: title.length > 0 ? title.split("\n")[0]
                                                           : "Row " + (r - hdr + 1)})
        }
        lanes = ls
        var maxCards = 0
        for (var mi = 0; mi < ls.length; ++mi) maxCards = Math.max(maxCards, ls[mi].cards.length)
        kb.implicitWidth = ls.length * (laneW + laneGap) - laneGap
        kb.implicitHeight = headerH + Math.max(1, maxCards) * (cardH + cardGap) + 60
    }
    onActiveChanged: recompute()
    onGroupColChanged: recompute()
    onLogicalRowChanged: recompute()
    Component.onCompleted: recompute()
    Connections {
        target: blockModel
        function onContentChangedSpike() { if (kb.active) kb.recompute() }
    }

    // Card drag state (board coords). A press arms; a small move starts the drag.
    property int    dragRow: -1
    property string dragTitle: ""
    property real   dragX: 0
    property real   dragY: 0
    property int    dropLane: -1
    property int    dropIdx: -1
    property var    _pressP: null
    property int    _armedRow: -1
    property string _armedTitle: ""

    function updateDrop(bx, by) {
        if (lanes.length === 0) { dropLane = -1; return }
        dropLane = Math.max(0, Math.min(lanes.length - 1,
                       Math.floor((bx + laneGap / 2) / (laneW + laneGap))))
        dropIdx = Math.max(0, Math.min(lanes[dropLane].cards.length,
                       Math.round((by - headerH) / (cardH + cardGap))))
    }
    function commitCardDrop() {
        if (dragRow >= 0 && dropLane >= 0 && dropLane < lanes.length) {
            var lane = lanes[dropLane]
            var kind = blockModel.tableColumnKind(logicalRow, groupCol)
            // Insertion target among the lane's cards with the dragged card taken
            // out (its rendered slot would shift everything below it by one).
            var rowsIn = [], dragPos = -1
            for (var i = 0; i < lane.cards.length; ++i) {
                if (lane.cards[i].r === dragRow) { dragPos = i; continue }
                rowsIn.push(lane.cards[i].r)
            }
            var idx = dropIdx
            if (dragPos >= 0 && dragPos < dropIdx) idx -= 1
            idx = Math.max(0, Math.min(idx, rowsIn.length))
            // Underlying row to land on: before rowsIn[idx], else after the lane's
            // last card; an empty lane keeps the row where it is. moveRow wants a
            // post-removal index, hence the -1 when moving downward.
            var to = -1
            if (rowsIn.length > 0) {
                to = idx < rowsIn.length ? rowsIn[idx] : rowsIn[rowsIn.length - 1] + 1
                if (to > dragRow) to -= 1
            }
            blockModel.beginGroup(logicalRow, logicalRow)
            if (kind === 2) blockModel.tableSetCellCheck(logicalRow, dragRow, groupCol, parseInt(lane.key))
            else if (kind === 1) blockModel.tableSetCellChoice(logicalRow, dragRow, groupCol, lane.key)
            if (to >= 0 && to !== dragRow) blockModel.tableMoveRow(logicalRow, dragRow, to)
            blockModel.endGroup()
        }
        dragRow = -1; _armedRow = -1; dropLane = -1; dropIdx = -1
    }

    Row {
        spacing: kb.laneGap
        Repeater {
            model: kb.lanes
            delegate: Item {
                id: laneItem
                required property var modelData
                required property int index
                width: kb.laneW
                height: Math.max(kb.headerH + kb.cardH + kb.cardGap, kb.height)

                Rectangle {   // lane backdrop (lit while a card is dragged over it)
                    anchors.fill: parent
                    radius: 8
                    color: (kb.dragRow >= 0 && kb.dropLane === laneItem.index)
                           ? Theme.colors.surfaceHover : "transparent"
                    border.width: 1
                    border.color: (kb.dragRow >= 0 && kb.dropLane === laneItem.index)
                                  ? Theme.colors.accent : Theme.colors.border
                }
                Row {   // header: colour dot + label + count
                    x: 10; height: kb.headerH
                    spacing: 7
                    Rectangle {
                        width: 9; height: 9; radius: 4.5
                        anchors.verticalCenter: parent.verticalCenter
                        color: laneItem.modelData.color !== "" ? laneItem.modelData.color
                             : laneItem.modelData.key === "1" ? Theme.colors.accent
                             : laneItem.modelData.key === "2" ? Theme.colors.accent
                             : Theme.colors.textMuted
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: laneItem.modelData.label
                        color: Theme.colors.text
                        font.family: Theme.font.family; font.pixelSize: 13; font.bold: true
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: laneItem.modelData.cards.length
                        color: Theme.colors.textMuted
                        font.family: Theme.font.family; font.pixelSize: 12
                    }
                }
                Column {
                    x: 8; y: kb.headerH
                    width: parent.width - 16
                    spacing: kb.cardGap
                    Repeater {
                        model: laneItem.modelData.cards
                        delegate: Rectangle {
                            id: card
                            required property var modelData
                            width: parent.width
                            height: kb.cardH
                            radius: 6
                            color: Theme.colors.surfaceHover
                            border.width: 1; border.color: Theme.colors.border
                            opacity: kb.dragRow === card.modelData.r ? 0.35 : 1
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                x: 10; width: parent.width - 20
                                elide: Text.ElideRight
                                text: card.modelData.title
                                color: Theme.colors.text
                                font.family: Theme.font.family; font.pixelSize: 13
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true; preventStealing: true
                                cursorShape: kb.dragRow >= 0 ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                onPressed: (m) => {
                                    kb._pressP = mapToItem(kb, m.x, m.y)
                                    kb._armedRow = card.modelData.r
                                    kb._armedTitle = card.modelData.title
                                }
                                onPositionChanged: (m) => {
                                    var p = mapToItem(kb, m.x, m.y)
                                    if (kb.dragRow < 0 && kb._armedRow >= 0 && kb._pressP
                                        && Math.abs(p.x - kb._pressP.x) + Math.abs(p.y - kb._pressP.y) > 6) {
                                        kb.dragRow = kb._armedRow
                                        kb.dragTitle = kb._armedTitle
                                    }
                                    if (kb.dragRow >= 0) {
                                        kb.dragX = p.x; kb.dragY = p.y
                                        kb.updateDrop(p.x, p.y)
                                    }
                                }
                                onReleased: kb.commitCardDrop()
                                onCanceled: { kb.dragRow = -1; kb._armedRow = -1; kb.dropLane = -1 }
                            }
                        }
                    }
                }
                Rectangle {   // insertion line at the drop gap
                    visible: kb.dragRow >= 0 && kb.dropLane === laneItem.index
                    x: 8; width: parent.width - 16; height: 3; radius: 1
                    y: kb.headerH + kb.dropIdx * (kb.cardH + kb.cardGap) - kb.cardGap / 2 - 1
                    color: Theme.colors.accent
                    z: 5
                }
            }
        }
    }

    Rectangle {   // floating ghost card under the cursor
        visible: kb.dragRow >= 0
        x: kb.dragX + 10; y: kb.dragY + 8
        width: kb.laneW - 16; height: kb.cardH
        radius: 6; z: 100
        color: Theme.colors.surfaceHover
        border.width: 1; border.color: Theme.colors.accent
        opacity: 0.92
        Text {
            anchors.verticalCenter: parent.verticalCenter
            x: 10; width: parent.width - 20
            elide: Text.ElideRight
            text: kb.dragTitle
            color: Theme.colors.text
            font.family: Theme.font.family; font.pixelSize: 13
        }
    }
}
