import QtQuick
import QtQuick.Controls

// The COMPONENTS row (2.0; under the files row since the chrome re-organisation, 2026-10-18 — it was the bottom
// strip): ONE LEVEL PER MODE (ruling 2026-10-17). Canvas mode shows canvas things:
// "Canvas" · one tab per PAGE (its title) · "+". Page mode shows page things: "Canvas" (the way back)
// · this page's tab (its Document view) · its frame tabs — each table a "Table N" tab, each PDF /
// video a tab labelled by its file name. The navigator menu (≡) lists everything in both modes.
// Frame tabs are keyed by block id (so the active tab follows its block across reorders) while the
// label/position is derived live from current order.
//
// The strip is a SINGLE row that clips when it overflows; a menu button pinned at
// the right opens a scrollable, sectioned popup (Tables / Videos / PDFs)
// listing every tab by its FULL filename — so large docs stay navigable without
// wrapping the strip into many rows.
Rectangle {
    id: tabs
    property var editor: null

    // Table tabs: the tables by their head record's id (SR-4 S9).
    readonly property var ids: (blockModel.structureRevision, blockModel.tableBlockIds())
    readonly property var pdfIds: (blockModel.structureRevision,
                                   blockModel.pdfBlockIds())
    readonly property var videoIds: (blockModel.structureRevision,
                                     blockModel.videoBlockIds())
    // Canvas mode's second kind of tab (phase A3): the canvas's own media elements with a full-frame view.
    readonly property var elementIds: canvasMode ? (canvas.elementsRevision, canvas.fullFrameMediaIds()) : []
    readonly property string activeId: !!editor ? editor.activeFrameId : ""
    readonly property bool canvasMode: !!editor && editor.activeCanvas
    // Probe hooks: the tab counts per level and the Canvas tab item.
    readonly property int pageTabCount: canvasMode ? pageRep.count : (canvas.documentOpen ? 1 : 0)
    readonly property int frameTabCount: canvasMode ? 0 : tableRep.count + pdfRep.count + videoRep.count
    readonly property int elementTabCount: canvasMode ? elemRep.count : 0
    function elementTabAt(i) { return elemRep.itemAt(i) }
    readonly property var canvasTabItem: canvasTab
    readonly property var navMenuItem: tabMenu   // probe (the chrome phase checks it opens DOWNWARD)
    visible: true   // always (ruling 2026-10-16): the Canvas tab lives here for every document
    readonly property int rowH: Theme.dim.componentRowH   // 24 (was 34)
    height: visible ? rowH : 0
    color: Theme.colors.bg              // the dark strip; the active tab is the content's colour (ruling 2026-10-18, like the files row)

    // A divider line along the BOTTOM edge (the row sits above the content now).
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border; z: 1 }

    function _select(id) { if (editor) editor.setActiveTab(id) }
    // A page tab: focus the page FIRST (the manager re-points blockModel), then the Document view.
    function _selectPage(id) { if (id === "") return; canvas.focusPage = id; _select("") }
    function _addPage() { const id = canvas.addPage(); if (id !== "") _selectPage(id) }
    function _pageLabel(id, index) {
        var t = canvas.pageTitle(id)
        if (t === "" || t === "Untitled page") return "Page " + (index + 1)
        return t.length > 22 ? t.substring(0, 20) + "…" : t
    }

    // Media tabs are labelled by file name (PDFs and videos alike). Truncate
    // in the MIDDLE: production filenames share long prefixes and differ at
    // the tail (_v001 / _v002), so end-truncation makes sibling tabs twins.
    function _mediaLabel(id, fallback) {
        var r = blockModel.rowForId(id)
        var n = r >= 0 ? blockModel.mediaFileName(r) : ""
        if (n === "") return fallback
        return n.length > 22 ? n.substring(0, 9) + "…" + n.substring(n.length - 12) : n
    }
    function _elementLabel(id) {
        var n = canvas.mediaElementName(id)
        if (n === "") return canvas.mediaElementKind(id) === "video" ? "Video" : "PDF"
        return n.length > 22 ? n.substring(0, 9) + "…" + n.substring(n.length - 12) : n
    }
    // The overflow menu shows the WHOLE name (the whole point of the menu).
    function _fullName(id, fallback) {
        var r = blockModel.rowForId(id)
        var n = r >= 0 ? blockModel.mediaFileName(r) : ""
        return n === "" ? fallback : n
    }

    // One tab. `active` highlights it; clicking selects it. `tooltip` shows the
    // full name on hover (media labels are truncated in the strip; defaults to
    // the label for the numbered Table/Document tabs).
    component TabBtn: Rectangle {
        id: btn
        property string label: ""
        property string tooltip: label
        property bool active: false
        signal clicked()
        width: tabLabel.implicitWidth + 24
        height: tabs.rowH
        // Whole tabs only: a tab that doesn't fully fit left of the menu
        // button hides completely (opacity keeps its Row slot — visible:false
        // would re-flow the Row and un-hide it, a loop). A sliced label
        // colliding with the menu button read as overlapping text. Hidden
        // tabs stay reachable through the navigator menu, which lights up.
        readonly property bool fullyVisible: x + width <= rowClip.width + 0.5
        opacity: fullyVisible ? 1 : 0
        enabled: fullyVisible
        color: active ? Theme.colors.surfaceRaised : "transparent"   // the active tab = the toolbar's colour (it joins the row below); no underline
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.colors.bgAlt2; visible: !btn.active }
        Text {
            id: tabLabel
            anchors.centerIn: parent; text: parent.label
            color: parent.active || tabMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall   // the tab tier (12)
        }
        MouseArea { id: tabMA; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor; onClicked: parent.clicked() }
        // Full name on hover — opens BELOW the row (it sits under the files row now). Left-aligned so
        // long filenames extend rightward.
        FlatToolTip {
            parent: btn
            x: 0
            y: btn.height + 6
            visible: tabMA.containsMouse && btn.tooltip.length > 0
            text: btn.tooltip
        }
    }

    // Tab row — clips when it overflows the space left of the menu button.
    Item {
        id: rowClip
        anchors.left: parent.left; anchors.top: parent.top
        width: parent.width - menuBtn.width
        height: tabs.rowH
        clip: true
        Row {
            id: row
            anchors.left: parent.left; anchors.top: parent.top; height: parent.height
            TabBtn {   // the file's canvas: the way back from a page, the view itself on the canvas
                id: canvasTab
                label: "Canvas"
                active: tabs.activeId === "canvas"
                onClicked: tabs._select("canvas")
            }
            // --- canvas mode: the pages, "+" ---
            Repeater {
                id: pageRep
                model: tabs.canvasMode ? (canvas.pagesRevision, canvas.pageIds) : []
                delegate: TabBtn {
                    required property int index
                    required property string modelData
                    label: (canvas.pagesRevision, tabs._pageLabel(modelData, index))
                    tooltip: (canvas.pagesRevision, canvas.pageTitle(modelData))
                    active: false
                    onClicked: tabs._selectPage(modelData)
                }
            }
            TabBtn {   // add a page (lands focused, ready to type)
                label: "+"
                tooltip: "Add a page"
                visible: tabs.canvasMode && canvas.documentOpen
                onClicked: tabs._addPage()
            }
            Repeater {   // the canvas's videos / PDFs: each opens its element-hosted full-frame view
                id: elemRep
                model: tabs.elementIds
                delegate: TabBtn {
                    required property string modelData
                    label: (canvas.elementsRevision, tabs._elementLabel(modelData))
                    tooltip: (canvas.elementsRevision, canvas.mediaElementName(modelData))
                    active: tabs.activeId === modelData
                    onClicked: tabs._select(modelData)
                }
            }
            // --- page mode: this page, then its frames ---
            TabBtn {
                visible: !tabs.canvasMode && canvas.documentOpen
                label: (canvas.pagesRevision, blockModel.contentRevision, tabs._pageLabel(canvas.focusPage, Math.max(0, canvas.pageIds.indexOf(canvas.focusPage))))
                tooltip: (canvas.pagesRevision, blockModel.contentRevision, canvas.pageTitle(canvas.focusPage))
                active: tabs.activeId === ""
                onClicked: tabs._select("")
            }
            Repeater {
                id: tableRep
                model: tabs.canvasMode ? [] : tabs.ids
                delegate: TabBtn {
                    required property int index
                    required property string modelData
                    label: "Table " + (index + 1)
                    active: tabs.activeId === modelData
                    onClicked: tabs._select(modelData)
                }
            }
            Repeater {
                id: pdfRep
                model: tabs.canvasMode ? [] : tabs.pdfIds
                delegate: TabBtn {
                    required property string modelData
                    label: tabs._mediaLabel(modelData, "PDF")
                    tooltip: tabs._fullName(modelData, "PDF")
                    active: tabs.activeId === modelData
                    onClicked: tabs._select(modelData)
                }
            }
            Repeater {
                id: videoRep
                model: tabs.canvasMode ? [] : tabs.videoIds
                delegate: TabBtn {
                    required property string modelData
                    label: tabs._mediaLabel(modelData, "Video")
                    tooltip: tabs._fullName(modelData, "Video")
                    active: tabs.activeId === modelData
                    onClicked: tabs._select(modelData)
                }
            }
        }
    }

    // Overflow / navigator menu at the end of the row — always present, so the
    // full-filename list is one click away even when the tabs all fit.
    Rectangle {
        id: menuBtn
        anchors.right: parent.right; anchors.top: parent.top
        width: tabs.rowH; height: tabs.rowH   // square
        color: tabMenu.visible ? Theme.colors.surfaceRaised : "transparent"
        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.colors.bgAlt2 }
        Icon { anchors.centerIn: parent; name: "list"; size: 14
               color: tabMenu.visible ? Theme.colors.textBright : Theme.colors.textMuted }
        // Overflow underline (white, the tab language): some tabs are hidden —
        // the full list lives in this menu.
        Rectangle {
            visible: row.width > rowClip.width
            anchors.bottom: parent.bottom; width: parent.width; height: 2
            color: Theme.colors.textBright
        }
        MouseArea { id: menuMA; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tabMenu.visible ? tabMenu.close() : tabMenu.open() }
    }

    // --- The navigator popup: scrollable, sectioned, full filenames. Opens
    // DOWNWARD (the row lives under the files row) and right-aligns to the
    // menu button.
    component SectionHeader: Text {
        property string title: ""
        text: title; color: Theme.colors.textMuted
        font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall; font.bold: true
        leftPadding: 10; topPadding: 8; bottomPadding: 2
    }
    component MenuItem: Rectangle {
        property string label: ""
        property string tabId: ""
        property string pageId: ""   // a page row: focus it, then the Document view
        width: menuCol.width; height: 28
        readonly property bool active: pageId !== "" ? (tabs.activeId === "" && canvas.focusPage === pageId)
                                                     : tabs.activeId === tabId
        color: itemMA.containsMouse ? Theme.colors.surfaceHover
             : (active ? Theme.colors.divider : "transparent")
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: 12
            anchors.right: parent.right; anchors.rightMargin: 10
            text: parent.label
            elide: Text.ElideMiddle   // keep the distinguishing tail (_v001/_v002)
            color: parent.active ? Theme.colors.textBright : Theme.colors.text
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
        }
        MouseArea { id: itemMA; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { if (parent.pageId !== "") tabs._selectPage(parent.pageId); else tabs._select(parent.tabId); tabMenu.close() } }
    }

    Popup {
        id: tabMenu
        padding: 1
        width: 320
        height: Math.min(360, menuCol.implicitHeight + 2)
        x: tabs.width - width
        y: tabs.height + 2
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Flickable {
            id: flick
            clip: true
            contentWidth: width
            contentHeight: menuCol.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: MnScrollBar {}
            Column {
                id: menuCol
                width: flick.width
                MenuItem { label: "Canvas"; tabId: "canvas" }
                SectionHeader { title: "Pages" }
                Repeater {
                    model: (canvas.pagesRevision, canvas.pageIds)
                    delegate: MenuItem {
                        required property int index
                        required property string modelData
                        label: (canvas.pagesRevision, blockModel.contentRevision, tabs._pageLabel(modelData, index)); pageId: modelData
                    }
                }
                SectionHeader { visible: tabs.canvasMode && tabs.elementIds.length > 0; title: "Media" }
                Repeater {
                    model: tabs.elementIds
                    delegate: MenuItem {
                        required property string modelData
                        label: (canvas.elementsRevision, canvas.mediaElementName(modelData)); tabId: modelData
                    }
                }
                SectionHeader { visible: !tabs.canvasMode && tabs.ids.length > 0; title: "Tables" }
                Repeater {
                    model: tabs.canvasMode ? [] : tabs.ids
                    delegate: MenuItem {
                        required property int index
                        required property string modelData
                        label: "Table " + (index + 1); tabId: modelData
                    }
                }
                SectionHeader { visible: !tabs.canvasMode && tabs.videoIds.length > 0; title: "Videos" }
                Repeater {
                    model: tabs.canvasMode ? [] : tabs.videoIds
                    delegate: MenuItem {
                        required property string modelData
                        label: tabs._fullName(modelData, "Video"); tabId: modelData
                    }
                }
                SectionHeader { visible: !tabs.canvasMode && tabs.pdfIds.length > 0; title: "PDFs" }
                Repeater {
                    model: tabs.canvasMode ? [] : tabs.pdfIds
                    delegate: MenuItem {
                        required property string modelData
                        label: tabs._fullName(modelData, "PDF"); tabId: modelData
                    }
                }
            }
        }
    }
}
