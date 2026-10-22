// The canvas's LAYERS panel (2026-10-21, the Inspector's top section while the canvas is up): the z order as rows,
// front-most first, straight from CanvasDocument.layerRows() — ink strokes read as one row per contiguous run
// ("Ink · 12"), so the stack the panel shows is the stack the canvas paints. Stateless beyond scroll: rows are
// derived on every revision. Click selects a row's elements, ⇧-click adds / removes them, double-click goes live
// (the canvas's double-click), right-click opens the canvas's context menu; drag a row to reorder (one history
// row). A partly selected run shows a dim fill.
import QtQuick
import QtQuick.Controls

Item {
    id: lp
    property var editor: null
    readonly property var view: editor ? editor.canvasItem : null
    readonly property var rows: (canvas.elementsRevision, canvas.pagesRevision, blockModel.contentRevision,
                                 canvas.documentOpen ? canvas.layerRows() : [])
    readonly property int rowH: 26
    readonly property int rowGap: 1
    readonly property int inset: 12   // the rows' side margin (the list itself is full-width so its scrollbar sits where the history's does)
    property bool fromPanel: false   // a selection the panel itself made — no scroll-follow
    // Drag-to-reorder state: the rows being dragged and the gap (0..rows.length) the drop line sits in.
    property var dragIds: []
    property int dropIndex: -1
    readonly property bool dragging: dropIndex >= 0
    readonly property alias list: list
    function iconFor(kind) {
        switch (kind) {
        case "page": return "file-text"
        case "image": return "image"
        case "video": return "film-strip"
        case "pdf": return "file-pdf"
        case "audio": return "music-note"
        case "folder": return "folder"
        case "bookmark": return "link"
        case "text": return "text-t"
        case "shape": return "polygon"
        case "ink": return "scribble"
        case "connector": return "flow-arrow"
        case "frame": return "frame-corners"
        default: return "file"
        }
    }
    function selectedCount(ids) {
        if (!view) return 0
        let c = 0
        for (let i = 0; i < ids.length; ++i) if (view.isSelected(ids[i])) ++c
        return c
    }
    function selectRow(ids, add) {
        if (!view) return
        fromPanel = true
        if (add) { const a = view.selectedIds.slice(); for (const id of ids) { const i = a.indexOf(id); if (i >= 0) a.splice(i, 1); else a.push(id) } view.selectMany(a) }
        else view.selectMany(ids)
        fromPanel = false
        if (editor) editor.forceActiveFocus()
    }
    function gapAt(y) {   // the drop gap nearest a y in list-content coordinates
        return Math.max(0, Math.min(rows.length, Math.round(y / (rowH + rowGap))))
    }
    function drop() {
        const gap = dropIndex, ids = dragIds
        dropIndex = -1; dragIds = []
        if (gap < 0 || !ids.length) return
        const anchor = gap < rows.length ? rows[gap].id : ""
        if (ids.indexOf(anchor) >= 0) return   // dropped onto itself
        canvas.placeAbove(ids, anchor)
    }
    // Scroll-follow: a selection made on the canvas brings its row into view.
    Connections {
        target: lp.view
        function onSelectedIdsChanged() {
            if (lp.fromPanel || !lp.view || lp.view.selectedIds.length === 0) return
            const id = lp.view.selectedIds[0]
            for (let i = 0; i < lp.rows.length; ++i)
                if (lp.rows[i].ids.indexOf(id) >= 0) { list.positionViewAtIndex(i, ListView.Contain); return }
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        spacing: lp.rowGap
        model: lp.rows
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: MnScrollBar {}
        delegate: Item {   // full-width (a vertical ListView owns its delegates' x); the row itself is inset
            id: row
            required property var modelData
            required property int index
            readonly property int selN: (lp.view ? lp.view.selectedIds : [], lp.selectedCount(modelData.ids))
            readonly property bool full: selN > 0 && selN === modelData.ids.length
            readonly property bool partial: selN > 0 && !full
            readonly property bool isMenuRow: true
            // A frame row carries its tint (2026-10-22): a bar at the left, the glyph and the name in it, the selected fill; its children a thread
            readonly property bool isFrame: modelData.kind === "frame"
            readonly property bool tinted: isFrame && !!modelData.tint
            readonly property color tint: tinted ? modelData.tint : Theme.colors.textMuted
            readonly property color childTint: modelData.parentTint ? modelData.parentTint : Theme.colors.border
            width: list.width; height: lp.rowH
            Rectangle {
            x: lp.inset + 14 * (row.modelData.depth || 0); width: parent.width - 2 * lp.inset - 14 * (row.modelData.depth || 0); height: lp.rowH   // a frame's children indent (2026-10-22)
            radius: Theme.dim.radiusSmall
            color: row.full ? (row.tinted ? Qt.rgba(row.tint.r, row.tint.g, row.tint.b, 0.35) : Theme.colors.divider)
                 : row.partial ? Qt.rgba(Theme.colors.divider.r, Theme.colors.divider.g, Theme.colors.divider.b, 0.5)
                 : row.isFrame ? (ma.containsMouse && !lp.dragging ? Theme.colors.surfaceHover : Qt.rgba(row.tint.r, row.tint.g, row.tint.b, row.tinted ? 0.10 : 0.06))
                 : (ma.containsMouse && !lp.dragging ? Theme.colors.surfaceHover : Theme.colors.hoverIdle)
            Rectangle { visible: row.isFrame; x: 0; y: 4; width: 3; height: parent.height - 8; radius: 1; color: row.tint }
            Rectangle { visible: (row.modelData.depth || 0) > 0; x: -9; y: -lp.rowGap; width: 2; height: parent.height + lp.rowGap; color: row.childTint; opacity: 0.8 }
            Behavior on color { ColorAnimation { duration: Theme.dim.hoverMs } }
            opacity: lp.dragging && lp.dragIds.indexOf(row.modelData.id) >= 0 ? 0.4 : 1
            Icon {
                id: glyph
                x: 8; anchors.verticalCenter: parent.verticalCenter
                name: lp.iconFor(row.modelData.kind); size: 13
                color: row.full ? Theme.colors.textBright : row.tinted ? row.tint : Theme.colors.textMuted
            }
            Text {
                anchors.left: glyph.right; anchors.leftMargin: 7
                anchors.right: meta.left; anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.name
                elide: Text.ElideRight
                color: row.full ? Theme.colors.textBright : row.tinted ? row.tint : Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome; font.bold: row.isFrame
            }
            Text {
                id: meta
                anchors.right: parent.right; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.kind === "ink" ? row.modelData.count : ""
                color: Theme.colors.textSubtle
                font.family: Theme.font.mono; font.pixelSize: 10
            }
            MouseArea {
                id: ma
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                property real pressY: 0
                property bool moved: false
                onPressed: (mouse) => { pressY = mouse.y; moved = false }
                onPositionChanged: (mouse) => {
                    if (!(mouse.buttons & Qt.LeftButton)) return
                    if (!moved && Math.abs(mouse.y - pressY) < 4) return
                    if (!moved) {
                        moved = true
                        // Drag the row's elements — or the whole selection when the row is part of it.
                        lp.dragIds = row.full && lp.view && lp.view.selectedIds.length > row.modelData.ids.length ? lp.view.selectedIds.slice() : row.modelData.ids.slice()
                    }
                    const p = ma.mapToItem(list.contentItem, mouse.x, mouse.y)
                    lp.dropIndex = lp.gapAt(p.y)
                }
                onReleased: (mouse) => {
                    if (moved) { lp.drop(); return }
                    if (mouse.button === Qt.RightButton) {
                        if (!row.full) lp.selectRow(row.modelData.ids, false)
                        if (lp.view) { const p = ma.mapToItem(lp.view, mouse.x, mouse.y); lp.view.contextMenuAt(p.x, p.y) }
                        return
                    }
                    lp.selectRow(row.modelData.ids, (mouse.modifiers & Qt.ShiftModifier) !== 0)
                }
                onCanceled: { lp.dropIndex = -1; lp.dragIds = [] }
                onDoubleClicked: (mouse) => { if (mouse.button === Qt.LeftButton && lp.view) { lp.selectRow(row.modelData.ids, false); lp.view.liveSelected() } }
            }
            }
        }
        // The drop line: in the gap the pointer is nearest, above the first row through below the last.
        Rectangle {
            visible: lp.dragging
            x: lp.inset + 4; width: list.width - 2 * lp.inset - 8; height: 2
            y: lp.dropIndex * (lp.rowH + lp.rowGap) - 1 - list.contentY
            color: Theme.colors.textBright
            radius: 1
        }
    }
    Text {   // an empty canvas
        visible: lp.rows.length === 0
        x: lp.inset; y: 4
        text: qsTr("Nothing on the canvas yet.")
        color: Theme.colors.textMuted
        font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
    }
}
