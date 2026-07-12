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
    property bool suspended: false         // resize in flight → covers drop their render
    signal showGrid()                      // emitted when the board can't render
    signal openCard(int r, int c)          // double-click → the grid, cell focused
    signal laneMenuRequested(int li, real bx, real by)   // lane-header right-click (kb coords)
    signal editClosed()                    // inline title edit done → refocus the editor

    readonly property int laneW: 260
    readonly property int laneGap: 14
    readonly property int headerH: 34
    readonly property int cardH: 40        // minimum / ghost height
    readonly property int cardGap: 8
    readonly property int cardPad: 8
    readonly property int titleH: 18
    readonly property int fieldH: 16

    // lanes: [{key, label, color, cards:[{r, title}]}]. key = option id for a
    // choice column, "0"/"1"/"2" for a check column, "" = the unset lane.
    // Rebuilt whole (imperatively) on any content change — tables are small and
    // the board is a flat projection, so a full rebuild beats incremental state.
    property var lanes: []
    property int titleCol: -1              // the card-title column (first text col)
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
        kb.titleCol = tc
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
        var cardW = laneW - 16
        for (var r = hdr; r < nr; ++r) {
            var key = kind === 2 ? String(blockModel.tableCellCheck(row, r, groupCol))
                                 : blockModel.tableCellChoice(row, r, groupCol)
            var lane = byKey[key] !== undefined ? byKey[key] : byKey[""]
            if (lane === undefined) continue
            var title = tc >= 0 ? blockModel.tableCell(row, r, tc) : ""
            // The card carries the WHOLE row: the row's first cell image as a
            // cover (height precomputed from the intrinsic dims, so drag
            // geometry never waits on an async load) + one field line per
            // remaining column — text when non-empty, a selected choice as
            // dot+label, a check as its glyph + the column's header name.
            var imgUrl = "", imgH = 0
            var fields = []
            for (var c2 = 0; c2 < nc; ++c2) {
                if (imgUrl === "" && blockModel.tableCellMedia(row, r, c2) !== "") {
                    imgUrl = blockModel.tableCellMediaUrl(row, r, c2)
                    imgH = Math.round(cardW * 9 / 16)   // 16:9 cover; crop fills the rest
                }
                if (c2 === groupCol || c2 === tc) continue
                var k2 = blockModel.tableColumnKind(row, c2)
                if (k2 === 1) {
                    if (blockModel.tableCellChoice(row, r, c2) !== "")
                        fields.push({kind: 1, text: blockModel.tableCellChoiceLabel(row, r, c2),
                                     color: blockModel.tableCellChoiceColor(row, r, c2), check: 0})
                } else if (k2 === 2) {
                    fields.push({kind: 2, text: hdr > 0 ? blockModel.tableCell(row, 0, c2) : "",
                                 color: "", check: blockModel.tableCellCheck(row, r, c2)})
                } else {
                    var t2 = blockModel.tableCell(row, r, c2)
                    if (t2.trim().length > 0)
                        fields.push({kind: 0, text: t2.split("\n")[0], color: "", check: 0})
                }
            }
            var ch = imgH + cardPad + titleH + fields.length * fieldH + cardPad
            lane.cards.push({r: r, h: ch, imgUrl: imgUrl, imgH: imgH, fields: fields,
                             bar: blockModel.tableRowBg(row, r),   // row colour → card edge bar
                             title: title.length > 0 ? title.split("\n")[0]
                                                     : "Row " + (r - hdr + 1)})
        }
        lanes = ls
        var maxH = 0
        for (var mi = 0; mi < ls.length; ++mi) {
            var s = 0
            for (var ci = 0; ci < ls[mi].cards.length; ++ci) s += ls[mi].cards[ci].h + cardGap
            maxH = Math.max(maxH, s)
        }
        kb.implicitWidth = ls.length * (laneW + laneGap) - laneGap
        kb.implicitHeight = headerH + Math.max(cardH + cardGap, maxH) + 60
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

    // Cards are variable-height (cover image + field lines), so gap hit-tests
    // and the insertion-line y walk the lane's precomputed card heights.
    function gapInLane(li, by) {
        var cards = lanes[li].cards
        var y = headerH
        for (var i = 0; i < cards.length; ++i) {
            if (by < y + cards[i].h / 2) return i
            y += cards[i].h + cardGap
        }
        return cards.length
    }
    function gapY(li, idx) {
        if (li < 0 || li >= lanes.length) return headerH
        var cards = lanes[li].cards
        var y = headerH
        for (var i = 0; i < idx && i < cards.length; ++i) y += cards[i].h + cardGap
        return y - cardGap / 2
    }
    function updateDrop(bx, by) {
        if (lanes.length === 0) { dropLane = -1; return }
        dropLane = Math.max(0, Math.min(lanes.length - 1,
                       Math.floor((bx + laneGap / 2) / (laneW + laneGap))))
        dropIdx = gapInLane(dropLane, by)
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

    // --- Add a card to a lane: insert the row right after the lane's last card
    // (so the grid order matches the board), set its status, ONE undo step; then
    // open the inline title editor on the new card.
    property int editRow: -1
    function addCard(li) {
        if (li < 0 || li >= lanes.length) return
        var lane = lanes[li]
        var row = logicalRow
        var at = lane.cards.length > 0 ? lane.cards[lane.cards.length - 1].r + 1
                                       : blockModel.tableRows(row)
        var kind = blockModel.tableColumnKind(row, groupCol)
        blockModel.beginGroup(row, row)
        blockModel.tableInsertRow(row, at)
        if (kind === 2) blockModel.tableSetCellCheck(row, at, groupCol, parseInt(lane.key))
        else if (lane.key !== "") blockModel.tableSetCellChoice(row, at, groupCol, lane.key)
        blockModel.endGroup()
        if (titleCol >= 0) editRow = at
    }
    function commitTitle(r, t) {
        if (editRow < 0) return
        editRow = -1
        if (titleCol >= 0 && t.length > 0) blockModel.tableSetCell(logicalRow, r, titleCol, t)
        kb.editClosed()
    }
    function cancelEdit() {
        editRow = -1
        kb.editClosed()
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
                    radius: 0   // squared — family style has no rounded corners
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
                MouseArea {   // lane header: right-click → lane menu
                    x: 0; y: 0; width: parent.width; height: kb.headerH
                    acceptedButtons: Qt.RightButton
                    onClicked: (m) => {
                        var p = mapToItem(kb, m.x, m.y)
                        kb.laneMenuRequested(laneItem.index, p.x, p.y)
                    }
                }
                Column {
                    id: cardsCol
                    x: 8; y: kb.headerH
                    width: parent.width - 16
                    spacing: kb.cardGap
                    Repeater {
                        model: laneItem.modelData.cards
                        delegate: Rectangle {
                            id: card
                            required property var modelData
                            width: parent.width
                            height: card.modelData.h
                            radius: 0
                            color: Theme.colors.surfaceHover
                            border.width: 1; border.color: Theme.colors.border
                            opacity: kb.dragRow === card.modelData.r ? 0.35 : 1
                            clip: true
                            Rectangle {   // resize placeholder for the cover
                                visible: kb.suspended && card.modelData.imgUrl !== ""
                                width: parent.width; height: card.modelData.imgH
                                color: "transparent"
                                border.width: 1; border.color: Theme.colors.divider   // quiet grey frame
                            }
                            Image {   // cover: the row's first cell image
                                id: coverImg
                                visible: card.modelData.imgUrl !== "" && !kb.suspended
                                width: parent.width; height: card.modelData.imgH
                                source: card.modelData.imgUrl
                                asynchronous: true; cache: false
                                fillMode: Image.PreserveAspectCrop
                                sourceSize.width: Math.round(kb.laneW * Screen.devicePixelRatio)
                                smooth: true
                                onVisibleChanged: if (visible) coverFade.restart()
                                NumberAnimation { id: coverFade; target: coverImg; property: "opacity"
                                                  from: 0; to: 1; duration: 200; easing.type: Easing.OutCubic }
                            }
                            Rectangle {   // row colour → a slim left edge bar
                                visible: card.modelData.bar !== ""
                                x: 0; y: 0; width: 3; height: parent.height
                                color: card.modelData.bar !== "" ? card.modelData.bar : "transparent"
                                z: 3
                            }
                            Column {
                                x: 10; y: card.modelData.imgH + kb.cardPad
                                width: parent.width - 20
                                Text {
                                    width: parent.width; height: kb.titleH
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                    text: card.modelData.title
                                    // invisible (not collapsed) while the inline
                                    // editor overlays it — Column must not reflow
                                    opacity: kb.editRow === card.modelData.r ? 0 : 1
                                    color: Theme.colors.text
                                    font.family: Theme.font.family; font.pixelSize: 13
                                }
                                Repeater {
                                    model: card.modelData.fields
                                    delegate: Item {
                                        id: fieldRow
                                        required property var modelData
                                        width: parent.width; height: kb.fieldH
                                        Rectangle {   // choice: a squared chip, like the grid's
                                            visible: fieldRow.modelData.kind === 1
                                            height: 14
                                            width: Math.min(parent.width, chipText.implicitWidth + 12)
                                            anchors.verticalCenter: parent.verticalCenter
                                            readonly property color _oc: fieldRow.modelData.color !== ""
                                                ? Qt.color(fieldRow.modelData.color) : Theme.colors.textMuted
                                            color: Qt.rgba(_oc.r, _oc.g, _oc.b, 0.28)
                                            border.width: 1
                                            border.color: Qt.rgba(_oc.r, _oc.g, _oc.b, 0.55)
                                            Text {
                                                id: chipText
                                                anchors.verticalCenter: parent.verticalCenter
                                                x: 6; width: Math.min(implicitWidth, parent.width - 12)
                                                elide: Text.ElideRight
                                                text: fieldRow.modelData.text
                                                color: Theme.colors.text
                                                font.family: Theme.font.family; font.pixelSize: 11
                                            }
                                        }
                                        Item {   // check: mini tri-state glyph
                                            visible: fieldRow.modelData.kind === 2
                                            width: 10; height: 10
                                            anchors.verticalCenter: parent.verticalCenter
                                            Rectangle {
                                                anchors.fill: parent; radius: 0
                                                color: fieldRow.modelData.check === 2 ? Theme.colors.accent : "transparent"
                                                border.width: fieldRow.modelData.check === 2 ? 0 : 1
                                                border.color: fieldRow.modelData.check === 1
                                                              ? Theme.colors.accent : Theme.colors.textMuted
                                            }
                                            Rectangle {   // in-progress dash
                                                visible: fieldRow.modelData.check === 1
                                                anchors.centerIn: parent; width: 5; height: 1.5
                                                color: Theme.colors.accent
                                            }
                                            Text {   // done check
                                                visible: fieldRow.modelData.check === 2
                                                anchors.centerIn: parent
                                                text: "✓"; color: Theme.colors.textBright
                                                font.pixelSize: 8; font.bold: true
                                            }
                                        }
                                        Text {
                                            visible: fieldRow.modelData.kind !== 1   // choice text lives in its chip
                                            x: fieldRow.modelData.kind !== 0 ? 14 : 0
                                            width: parent.width - x
                                            anchors.verticalCenter: parent.verticalCenter
                                            elide: Text.ElideRight
                                            text: fieldRow.modelData.text
                                            color: Theme.colors.textMuted
                                            font.family: Theme.font.family; font.pixelSize: 12
                                        }
                                    }
                                }
                            }
                            TextInput {   // inline title editor (fresh cards)
                                visible: kb.editRow === card.modelData.r
                                x: 10; width: parent.width - 20
                                y: card.modelData.imgH + kb.cardPad
                                height: kb.titleH
                                verticalAlignment: TextInput.AlignVCenter
                                color: Theme.colors.text
                                selectionColor: Theme.colors.selectionBg
                                selectedTextColor: Theme.colors.textBright
                                font.family: Theme.font.family; font.pixelSize: 13
                                clip: true
                                z: 6
                                onVisibleChanged: if (visible) { text = ""; forceActiveFocus() }
                                onAccepted: kb.commitTitle(card.modelData.r, text)
                                onActiveFocusChanged: if (!activeFocus && visible) kb.commitTitle(card.modelData.r, text)
                                Keys.onEscapePressed: kb.cancelEdit()
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true; preventStealing: true
                                enabled: kb.editRow !== card.modelData.r
                                cursorShape: kb.dragRow >= 0 ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                onDoubleClicked: kb.openCard(card.modelData.r,
                                                             kb.titleCol >= 0 ? kb.titleCol : 0)
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
                Rectangle {   // + Add card (also the empty-lane affordance)
                    x: 8; width: parent.width - 16; height: 26
                    y: kb.headerH + cardsCol.height + (laneItem.modelData.cards.length > 0 ? kb.cardGap : 0)
                    color: addCardMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
                    border.width: 1
                    border.color: addCardMA.containsMouse ? Theme.colors.border : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter; x: 10
                        text: "+ Add"
                        color: addCardMA.containsMouse ? Theme.colors.text : Theme.colors.textSubtle
                        font.family: Theme.font.family; font.pixelSize: 12
                    }
                    MouseArea {
                        id: addCardMA
                        anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: kb.addCard(laneItem.index)
                    }
                }
                Rectangle {   // insertion line at the drop gap
                    visible: kb.dragRow >= 0 && kb.dropLane === laneItem.index
                    x: 8; width: parent.width - 16; height: 3; radius: 0
                    y: kb.gapY(laneItem.index, kb.dropIdx) - 1
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
        radius: 0; z: 100
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
