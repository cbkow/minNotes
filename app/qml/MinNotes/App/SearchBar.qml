// Search (v2.0.1): ⌘F opens this strip under the toolbar — one field, the hit count, a close box — and the results
// drop down beneath it as a list grouped by source (This page · Pages · Cards · PDFs · Video notes · Comments; the
// current page's hits first, the engine's order). Live as you type (a short debounce); ↑ / ↓ walk, Return jumps,
// Escape closes and hands focus back to the editor. Scope = the whole file, never other files. House style: 30 px,
// chrome 13, squared, the strip tone; the list a surface with a 1-px border.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    required property var editor
    required property var canvasView
    property var inspector: null
    property bool open: false
    property var hits: []
    property int current: 0
    readonly property int count: hits.length
    readonly property var field: searchField        // probe
    readonly property var list: resultList          // probe
    property int jumps: 0                           // probe
    width: parent ? parent.width : 600
    height: open ? Theme.dim.tabRowH : 0
    visible: open
    color: Theme.colors.bgStrip
    clip: true

    function show() { open = true; if (searchField.text !== "") run(); searchField.forceActiveFocus(); searchField.selectAll() }
    function hide() { open = false; results.close(); if (editor) editor.forceActiveFocus() }
    function toggle() { if (open && searchField.activeFocus) hide(); else show() }
    function run() {
        const q = searchField.text.trim()
        hits = (q === "" || !canvas.documentOpen) ? [] : search.search(q, canvas.focusedPageId())
        current = 0
        if (hits.length > 0 && open) results.open(); else results.close()
    }
    function groupOf(h) {
        if (h.kind === "page") return h.pageId === canvas.focusedPageId() ? qsTr("This page") : qsTr("Pages")
        return h.kind === "card" ? qsTr("Cards") : h.kind === "pdf" ? qsTr("PDFs") : h.kind === "video" ? qsTr("Video notes") : qsTr("Comments")
    }
    // Jumping: a page or comment hit focuses the page and selects the match (the model re-points on the focus
    // change, so the row lookup waits a tick); a card hit returns to the canvas, selects it and brings the camera to
    // it; a PDF hit sets the page on its view key and opens the PDF's frame; a video note sets the player's
    // remembered playhead on its anchor key and opens the studio there.
    function jump(h) {
        if (!h || !editor) return
        jumps++
        if (h.kind === "card") {
            editor.setActiveTab("canvas")
            Qt.callLater(function() {
                if (!canvasView) return
                canvasView.select(h.elementId)
                const r = canvas.elementRect(h.elementId), cam = canvasView.readableCameraFor(r, h.elementId)
                if (cam.moves) canvasView.animateCameraTo(cam.x, cam.y, cam.z, null)
            })
            return
        }
        if (h.kind === "video" && h.key !== "") { editor.videoPlayheads[h.key] = h.frame; editor.videoPlayheadRev++ }
        if (h.kind === "pdf" && h.viewPath !== "") editor.setPdfPageKey(h.viewPath, h.page, h.page + 1)
        if ((h.kind === "pdf" || h.kind === "video") && h.elementId !== "") {   // a canvas media element's own frame
            editor.setActiveTab(h.elementId)
            return
        }
        if (canvas.focusPage !== h.pageId) canvas.focusPage = h.pageId
        Qt.callLater(function() {
            const r = blockModel.rowForId(h.blockId)
            if (r < 0) return
            if (h.kind === "pdf" || h.kind === "video") { editor.setActiveTab(h.blockId); return }
            editor.setActiveTab("")
            editor.selectInRow(r, h.kind === "page" ? h.col : 0, h.kind === "page" ? h.len : 0)
            if (h.kind === "comment" && inspector) inspector.showComments(h.threadId)
        })
    }
    function jumpCurrent() { if (hits.length > 0) jump(hits[Math.max(0, Math.min(current, hits.length - 1))]) }

    Timer { id: debounce; interval: 120; onTriggered: bar.run() }
    Connections { target: search; function onCacheUpdated() { if (bar.open) bar.run() } }

    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.colors.border }
    Row {
        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 6; spacing: 8
        Icon { anchors.verticalCenter: parent.verticalCenter; name: "magnifying-glass"; size: 14; color: Theme.colors.textMuted }
        TextInput {
            id: searchField
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 14 - 8 - countLabel.width - 8 - closeBox.width - 8
            clip: true; selectByMouse: true
            color: Theme.colors.text; selectionColor: Theme.colors.selectionBg
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
            onTextChanged: debounce.restart()
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Escape) { bar.hide(); event.accepted = true }
                else if (event.key === Qt.Key_Down) { if (bar.hits.length > 0) bar.current = Math.min(bar.hits.length - 1, bar.current + 1); event.accepted = true }
                else if (event.key === Qt.Key_Up) { bar.current = Math.max(0, bar.current - 1); event.accepted = true }
                else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { bar.jumpCurrent(); event.accepted = true }
            }
            Text {
                anchors.fill: parent; verticalAlignment: Text.AlignVCenter
                visible: searchField.text.length === 0
                text: qsTr("Search this file — pages, cards, PDFs, video notes, comments")
                color: Theme.colors.textSubtle; font: searchField.font; elide: Text.ElideRight
            }
        }
        Text {
            id: countLabel
            anchors.verticalCenter: parent.verticalCenter
            text: (search && search.indexing) ? qsTr("indexing PDFs…") : searchField.text.trim() === "" ? "" : bar.count === 0 ? qsTr("no hits") : bar.count === 1 ? qsTr("1 hit") : qsTr("%1 hits").arg(bar.count)
            color: Theme.colors.textMuted; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
        }
        Item {
            id: closeBox
            anchors.verticalCenter: parent.verticalCenter
            width: 22; height: 22
            Rectangle { anchors.fill: parent; radius: 0; color: closeMA.containsMouse ? Theme.colors.surfaceHover : "transparent" }
            Icon { anchors.centerIn: parent; name: "x"; size: 12; color: Theme.colors.textMuted }
            MouseArea { id: closeMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: bar.hide() }
        }
    }

    // The results, dropped under the strip over whatever is below (the Overlay, like the path bar's menus).
    Popup {
        id: results
        parent: Overlay.overlay
        x: bar.mapToItem(Overlay.overlay, 0, 0).x
        y: bar.mapToItem(Overlay.overlay, 0, 0).y + bar.height
        width: bar.width
        height: Math.min(440, resultList.contentHeight + 2)
        padding: 1; modal: false; focus: false
        closePolicy: Popup.NoAutoClose
        background: Rectangle { color: Theme.colors.surface; radius: 0; border.width: 1; border.color: Theme.colors.border }
        contentItem: ListView {
            id: resultList
            clip: true
            model: bar.hits
            currentIndex: bar.current
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
            boundsBehavior: Flickable.StopAtBounds
            delegate: Item {
                id: row
                required property int index
                required property var modelData
                readonly property string group: bar.groupOf(modelData)
                readonly property bool firstOfGroup: index === 0 || bar.groupOf(bar.hits[index - 1]) !== group
                width: resultList.width; height: (firstOfGroup ? 22 : 0) + 40
                Text {   // the group header
                    visible: row.firstOfGroup
                    x: 12; y: 4; text: row.group
                    color: Theme.colors.textSubtle; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall; font.bold: true
                }
                Rectangle {
                    y: row.firstOfGroup ? 22 : 0; width: parent.width; height: 40; radius: 0
                    color: row.index === bar.current ? Theme.colors.divider : rowMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
                    Text {
                        x: 12; y: 5; width: parent.width - 24; elide: Text.ElideRight
                        text: row.modelData.title
                        color: Theme.colors.text; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
                    }
                    Text {
                        x: 12; y: 21; width: parent.width - 24; elide: Text.ElideRight
                        textFormat: Text.StyledText
                        text: row.modelData.snippet
                        color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                    }
                    MouseArea { id: rowMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { bar.current = row.index; bar.jump(row.modelData) } }
                }
            }
        }
    }
}
