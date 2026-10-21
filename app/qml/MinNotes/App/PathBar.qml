// The PATH BAR (2.0 chrome, 2026-10-18 — replaces the files row + the components row): ONE row that reads as the
// hierarchy it is —  • File ▾  ›  Canvas  ›  Page 3 ▾  ›  Table 2 ▾.  The FILE segment's name IS the file switcher
// (its menu lists the open files, with a close affordance and "New file"); segments LEFT of the current level
// navigate up on click; the LAST segment (and the file) drop down to their siblings — pages / media elements under
// the canvas, pages + this page's tables / PDFs / videos under a page, the sibling frames under a frame. One level
// per mode holds (ruling 2026-10-17). The former tab-merge DRAG is gone: merging lives in File ▸ Merge File Into
// This Page… and the block menu's "Merge a file here…" (ruling 2026-10-18). Probe hooks keep the strip's names.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: bar
    property var editor: null
    height: Theme.dim.tabRowH
    color: Theme.colors.bgStrip
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border; z: 2 }

    // --- the document's facts (the same reads the strip made) ---
    readonly property var ids: (blockModel.structureRevision, blockModel.tableBlockIds())
    readonly property var pdfIds: (blockModel.structureRevision, blockModel.pdfBlockIds())
    readonly property var videoIds: (blockModel.structureRevision, blockModel.videoBlockIds())
    readonly property var elementIds: canvasMode ? (canvas.elementsRevision, canvas.fullFrameMediaIds()) : []
    readonly property string activeId: !!editor ? editor.activeFrameId : ""
    readonly property bool canvasMode: !!editor && editor.activeCanvas
    readonly property string elementId: !!editor ? editor.activeElementId : ""          // a media element's full-frame view
    readonly property bool frameOpen: !canvasMode && activeId !== "" && activeId !== "canvas"   // a table / PDF / video frame of the page
    readonly property bool elementOpen: canvasMode && elementId !== ""
    // Probe hooks (the strip's names).
    readonly property int pageTabCount: canvasMode ? pageRep.count : (canvas.documentOpen ? 1 : 0)
    readonly property int frameTabCount: canvasMode ? 0 : tableRep.count + pdfRep.count + videoRep.count
    readonly property int elementTabCount: canvasMode ? elemRep.count : 0
    function elementTabAt(i) { if (!compMenu.visible) compMenu.open(); return elemRep.itemAt(i) }
    readonly property var canvasTabItem: canvasSeg
    readonly property var navMenuItem: compMenu
    readonly property var fileMenuItem: fileMenu
    readonly property var fileSegment: fileSeg

    function _select(id) { if (editor) editor.setActiveTab(id) }
    function _selectPage(id) { if (id === "") return; canvas.focusPage = id; _select("") }
    function _addPage() { const id = canvas.addPage(); if (id !== "") _selectPage(id) }
    function _pageLabel(id, index) {
        var t = canvas.pageTitle(id)
        if (t === "" || t === "Untitled page") return "Page " + (index + 1)
        return t.length > 22 ? t.substring(0, 20) + "…" : t
    }
    function _pageIndexOf(id) { const a = canvas.pageIds; for (let i = 0; i < a.length; ++i) if (a[i] === id) return i; return 0 }
    function _mediaLabel(id, fallback) {
        var r = blockModel.rowForId(id)
        var n = r >= 0 ? blockModel.mediaFileName(r) : ""
        if (n === "") return fallback
        return n.length > 22 ? n.substring(0, 9) + "…" + n.substring(n.length - 12) : n
    }
    function _fullName(id, fallback) {
        var r = blockModel.rowForId(id)
        var n = r >= 0 ? blockModel.mediaFileName(r) : ""
        return n === "" ? fallback : n
    }
    function _elementLabel(id) {
        var n = canvas.mediaElementName(id)
        if (n === "") return canvas.mediaElementKind(id) === "video" ? "Video" : "PDF"
        return n.length > 22 ? n.substring(0, 9) + "…" + n.substring(n.length - 12) : n
    }
    function _frameLabel(id) {   // a frame: "Table N" by position, a PDF / video by file name
        const t = ids.indexOf(id)
        if (t >= 0) return "Table " + (t + 1)
        return _mediaLabel(id, pdfIds.indexOf(id) >= 0 ? "PDF" : "Video")
    }
    readonly property string currentPageLabel: (canvas.pagesRevision, blockModel.contentRevision, canvas.focusPage !== "" ? _pageLabel(canvas.focusPage, _pageIndexOf(canvas.focusPage)) : "Page")
    readonly property string lastLabel: elementOpen ? (canvas.elementsRevision, _elementLabel(elementId)) : frameOpen ? (blockModel.structureRevision, _frameLabel(activeId)) : ""

    // --- one segment of the path ---
    component Segment: Rectangle {
        id: seg
        property string label: ""
        property bool dropdown: false       // shows a caret; a click opens its menu
        property bool current: false        // the last segment: bright
        property bool menuOpen: false
        signal clicked()
        height: bar.height - 1
        width: segRow.implicitWidth + 20
        radius: Theme.dim.radiusSmall
        color: menuOpen ? Theme.colors.divider : (segMA.containsMouse ? Theme.colors.affordanceHover : Theme.colors.affordanceIdleClear)   // the title bar's hover (2026-10-21)
        Behavior on color { ColorAnimation { duration: Theme.dim.hoverMs } }
        Row {
            id: segRow
            anchors.centerIn: parent; spacing: 5
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: seg.label
                color: seg.current || segMA.containsMouse || seg.menuOpen ? Theme.colors.textBright : Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome   // the chrome tier (13): no reduced size any more
                elide: Text.ElideMiddle
                width: Math.min(implicitWidth, 220)
            }
            Icon { visible: seg.dropdown; anchors.verticalCenter: parent.verticalCenter; name: "caret-down"; size: 10
                   color: seg.current || segMA.containsMouse ? Theme.colors.textMuted : Theme.colors.textSubtle }
        }
        MouseArea { id: segMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: seg.clicked() }
    }
    component Chevron: Text {
        anchors.verticalCenter: parent.verticalCenter
        text: "›"; leftPadding: 4; rightPadding: 4
        color: Theme.colors.textSubtle; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
    }

    Row {
        id: path
        anchors.left: parent.left; anchors.leftMargin: 6; anchors.top: parent.top
        height: bar.height - 1
        spacing: 0
        Segment {   // the FILE: its name is the switcher
            id: fileSeg
            anchors.verticalCenter: parent.verticalCenter
            label: (blockModel.untitled || blockModel.dirty ? "• " : "") + blockModel.documentName
            dropdown: true
            current: false
            menuOpen: fileMenu.visible
            onClicked: fileMenu.visible ? fileMenu.close() : fileMenu.open()
            FlatToolTip { parent: fileSeg; x: 0; y: fileSeg.height + 6
                          visible: segMAHover.hovered && !fileMenu.visible && !!blockModel.documentPath && blockModel.documentPath.length > 0
                          text: blockModel.documentPath || "" }
            HoverHandler { id: segMAHover }
        }
        Chevron {}
        Segment {   // the CANVAS: the root of the file's contents
            id: canvasSeg
            anchors.verticalCenter: parent.verticalCenter
            label: qsTr("Canvas")
            readonly property bool isLast: bar.canvasMode && !bar.elementOpen
            dropdown: isLast
            current: isLast
            menuOpen: compMenu.visible && compMenu.parent === canvasSeg
            onClicked: { if (isLast) { compMenu.visible ? compMenu.close() : compMenu.open() } else bar._select("canvas") }
        }
        Chevron { visible: !bar.canvasMode }
        Segment {   // the PAGE (page mode)
            id: pageSeg
            visible: !bar.canvasMode
            anchors.verticalCenter: parent.verticalCenter
            label: bar.currentPageLabel
            readonly property bool isLast: !bar.canvasMode && !bar.frameOpen
            dropdown: isLast
            current: isLast
            menuOpen: compMenu.visible && compMenu.parent === pageSeg
            onClicked: { if (isLast) { compMenu.visible ? compMenu.close() : compMenu.open() } else bar._select("") }
        }
        Chevron { visible: bar.frameOpen || bar.elementOpen }
        Segment {   // a FRAME of the page, or a media ELEMENT's full-frame view
            id: lastSeg
            visible: bar.frameOpen || bar.elementOpen
            anchors.verticalCenter: parent.verticalCenter
            label: bar.lastLabel
            dropdown: true
            current: true
            menuOpen: frameMenu.visible
            onClicked: frameMenu.visible ? frameMenu.close() : frameMenu.open()
        }
    }

    // --- menus ---
    component SectionHeader: Text {
        property string title: ""
        text: title; color: Theme.colors.textMuted
        font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall; font.bold: true
        leftPadding: 10; topPadding: 8; bottomPadding: 2
    }
    component MenuItem: Rectangle {
        id: mi
        property string label: ""
        property string tabId: ""
        property string pageId: ""   // a page row: focus it, then the Document view
        property bool active: false
        signal picked()
        width: parent ? parent.width : 300; height: 28; radius: Theme.dim.radiusSmall
        color: itemMA.containsMouse ? Theme.colors.surfaceHover : (active ? Theme.colors.divider : Theme.colors.hoverIdle)
        Behavior on color { ColorAnimation { duration: Theme.dim.hoverMs } }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: 12
            anchors.right: parent.right; anchors.rightMargin: 10
            text: mi.label
            elide: Text.ElideMiddle
            color: mi.active ? Theme.colors.textBright : Theme.colors.text
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
        }
        MouseArea { id: itemMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: mi.picked() }
    }
    component DropMenu: Popup {
        id: dm
        padding: 1
        width: 320
        height: Math.min(380, contentItem.contentHeight + 2)
        x: 0; y: parent ? parent.height + 2 : 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.colors.surface; radius: Theme.dim.radius; border.width: 1; border.color: Theme.colors.border }
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.dim.popupMs } }
            onClosed: if (bar.editor) bar.editor.forceActiveFocus()
    }

    // The FILE menu: the open files (the active one marked, a dirty dot, an × to close) and New file.
    DropMenu {
        id: fileMenu
        parent: fileSeg
        contentItem: Flickable {
            clip: true; contentWidth: width; contentHeight: fileCol.implicitHeight; boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: MnScrollBar {}
            Column {
                id: fileCol
                width: parent.width
                SectionHeader { title: qsTr("Open files") }
                Repeater {
                    model: docs.models
                    delegate: MenuItem {
                        required property int index
                        required property var modelData
                        label: ((modelData.untitled || modelData.dirty) ? "• " : "") + modelData.documentName
                        active: index === docs.activeIndex
                        onPicked: { win.switchToTab(index); fileMenu.close() }
                        Rectangle {   // close ×, on hover
                            anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter
                            width: 18; height: 18; radius: Theme.dim.radiusSmall
                            visible: closeMA.containsMouse || parent.children[1].containsMouse
                            color: closeMA.containsMouse ? Theme.colors.divider : "transparent"
                            Icon { anchors.centerIn: parent; name: "x"; size: 11; color: closeMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted }
                            MouseArea { id: closeMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: { fileMenu.close(); win.requestCloseTab(parent.parent.index) } }
                        }
                    }
                }
                MenuItem { label: qsTr("+ New file"); onPicked: { fileMenu.close(); docs.newTab() } }
            }
        }
    }
    // The COMPONENT menu under the Canvas (canvas mode) or the page (page mode): pages (+ add), media elements,
    // and in page mode this page's tables / videos / PDFs.
    DropMenu {
        id: compMenu
        parent: bar.canvasMode ? canvasSeg : pageSeg
        contentItem: Flickable {
            clip: true; contentWidth: width; contentHeight: compCol.implicitHeight; boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: MnScrollBar {}
            Column {
                id: compCol
                width: parent.width
                SectionHeader { title: qsTr("Pages") }
                Repeater {
                    id: pageRep
                    model: (canvas.pagesRevision, canvas.pageIds)
                    delegate: MenuItem {
                        required property int index
                        required property string modelData
                        label: (canvas.pagesRevision, blockModel.contentRevision, bar._pageLabel(modelData, index))
                        active: !bar.canvasMode && bar.activeId === "" && canvas.focusPage === modelData
                        onPicked: { bar._selectPage(modelData); compMenu.close() }
                    }
                }
                MenuItem { label: qsTr("+ Add page"); visible: canvas.documentOpen; onPicked: { compMenu.close(); bar._addPage() } }
                SectionHeader { visible: bar.canvasMode && bar.elementIds.length > 0; title: qsTr("Media") }
                Repeater {
                    id: elemRep
                    model: bar.elementIds
                    delegate: MenuItem {
                        required property string modelData
                        label: (canvas.elementsRevision, canvas.mediaElementName(modelData)); tabId: modelData
                        active: bar.elementId === modelData
                        onPicked: { bar._select(modelData); compMenu.close() }
                    }
                }
                SectionHeader { visible: !bar.canvasMode && bar.ids.length > 0; title: qsTr("Tables") }
                Repeater {
                    id: tableRep
                    model: bar.canvasMode ? [] : bar.ids
                    delegate: MenuItem {
                        required property int index
                        required property string modelData
                        label: "Table " + (index + 1); active: bar.activeId === modelData
                        onPicked: { bar._select(modelData); compMenu.close() }
                    }
                }
                SectionHeader { visible: !bar.canvasMode && bar.videoIds.length > 0; title: qsTr("Videos") }
                Repeater {
                    id: videoRep
                    model: bar.canvasMode ? [] : bar.videoIds
                    delegate: MenuItem {
                        required property string modelData
                        label: bar._fullName(modelData, "Video"); active: bar.activeId === modelData
                        onPicked: { bar._select(modelData); compMenu.close() }
                    }
                }
                SectionHeader { visible: !bar.canvasMode && bar.pdfIds.length > 0; title: qsTr("PDFs") }
                Repeater {
                    id: pdfRep
                    model: bar.canvasMode ? [] : bar.pdfIds
                    delegate: MenuItem {
                        required property string modelData
                        label: bar._fullName(modelData, "PDF"); active: bar.activeId === modelData
                        onPicked: { bar._select(modelData); compMenu.close() }
                    }
                }
            }
        }
    }
    // The FRAME / ELEMENT menu: the siblings of the open frame (this page's frames) or of the open element (the
    // canvas's media elements), plus the way back up.
    DropMenu {
        id: frameMenu
        parent: lastSeg
        contentItem: Flickable {
            clip: true; contentWidth: width; contentHeight: frameCol.implicitHeight; boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: MnScrollBar {}
            Column {
                id: frameCol
                width: parent.width
                MenuItem { label: bar.elementOpen ? qsTr("↑ Canvas") : qsTr("↑ ") + bar.currentPageLabel
                           onPicked: { frameMenu.close(); bar._select(bar.elementOpen ? "canvas" : "") } }
                SectionHeader { visible: bar.elementOpen; title: qsTr("Media") }
                Repeater {
                    model: bar.elementOpen ? bar.elementIds : []
                    delegate: MenuItem {
                        required property string modelData
                        label: (canvas.elementsRevision, canvas.mediaElementName(modelData)); active: bar.elementId === modelData
                        onPicked: { bar._select(modelData); frameMenu.close() }
                    }
                }
                SectionHeader { visible: bar.frameOpen && bar.ids.length > 0; title: qsTr("Tables") }
                Repeater {
                    model: bar.frameOpen ? bar.ids : []
                    delegate: MenuItem {
                        required property int index
                        required property string modelData
                        label: "Table " + (index + 1); active: bar.activeId === modelData
                        onPicked: { bar._select(modelData); frameMenu.close() }
                    }
                }
                SectionHeader { visible: bar.frameOpen && bar.videoIds.length > 0; title: qsTr("Videos") }
                Repeater {
                    model: bar.frameOpen ? bar.videoIds : []
                    delegate: MenuItem {
                        required property string modelData
                        label: bar._fullName(modelData, "Video"); active: bar.activeId === modelData
                        onPicked: { bar._select(modelData); frameMenu.close() }
                    }
                }
                SectionHeader { visible: bar.frameOpen && bar.pdfIds.length > 0; title: qsTr("PDFs") }
                Repeater {
                    model: bar.frameOpen ? bar.pdfIds : []
                    delegate: MenuItem {
                        required property string modelData
                        label: bar._fullName(modelData, "PDF"); active: bar.activeId === modelData
                        onPicked: { bar._select(modelData); frameMenu.close() }
                    }
                }
            }
        }
    }
}
