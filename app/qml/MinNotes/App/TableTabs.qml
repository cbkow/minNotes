import QtQuick
import QtQuick.Controls

// Bottom tab strip — appears when the document contains a table or an inline PDF.
// The first tab is always the whole Document; each table gets a "Table N" tab and
// each PDF/video a tab labelled by its file name. Tabs are keyed by block id (so
// the active tab follows its block across reorders) while the label/position is
// derived live from current order.
//
// The strip is a SINGLE row that clips when it overflows; a menu button pinned at
// the right opens a scrollable, sectioned popup (Tables / Videos / PDFs / Sketches)
// listing every tab by its FULL filename — so large docs stay navigable without
// wrapping the strip into many rows.
Rectangle {
    id: tabs
    property var editor: null

    readonly property var ids: (blockModel.layoutRevision, blockModel.contentRevision,
                                blockModel.tableBlockIds())
    readonly property var pdfIds: (blockModel.layoutRevision, blockModel.contentRevision,
                                   blockModel.pdfBlockIds())
    readonly property var videoIds: (blockModel.layoutRevision, blockModel.contentRevision,
                                     blockModel.videoBlockIds())
    readonly property var sketchIds: (blockModel.layoutRevision, blockModel.contentRevision,
                                      blockModel.sketchBlockIds())
    readonly property string activeId: !!editor ? editor.activeFrameId : ""
    visible: ids.length > 0 || pdfIds.length > 0 || videoIds.length > 0 || sketchIds.length > 0
    readonly property int rowH: 34
    height: visible ? rowH : 0
    color: Theme.colors.surfaceRaised   // sidebar tone — the item-tab strip is chrome, like the Inspector

    // A divider line along the top edge.
    Rectangle { width: parent.width; height: 1; color: Theme.colors.border; z: 1 }

    function _select(id) { if (editor) editor.setActiveTab(id) }

    // Media tabs are labelled by file name (PDFs and videos alike). Truncate
    // in the MIDDLE: production filenames share long prefixes and differ at
    // the tail (_v001 / _v002), so end-truncation makes sibling tabs twins.
    function _mediaLabel(id, fallback) {
        var r = blockModel.rowForId(id)
        var n = r >= 0 ? blockModel.mediaFileName(r) : ""
        if (n === "") return fallback
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
    // the label for the numbered Table/Sketch/Document tabs).
    component TabBtn: Rectangle {
        id: btn
        property string label: ""
        property string tooltip: label
        property bool active: false
        signal clicked()
        width: tabLabel.implicitWidth + 36
        height: tabs.rowH
        // active fill matches the table toolbar's checked Table/Board segment
        color: active ? Theme.colors.divider : (tabMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
        Rectangle {   // active underline (white — accent stays reserved for real highlights)
            visible: parent.active
            anchors.bottom: parent.bottom; width: parent.width; height: 2
            color: Theme.colors.textBright
        }
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.colors.border }
        Text {
            id: tabLabel
            anchors.centerIn: parent; text: parent.label
            color: parent.active ? Theme.colors.textBright : Theme.colors.textMuted
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
        }
        MouseArea { id: tabMA; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor; onClicked: parent.clicked() }
        // Full name on hover — opens above the strip (it sits at the window's
        // bottom edge). Left-aligned so long filenames extend rightward.
        FlatToolTip {
            parent: btn
            x: 0
            y: -implicitHeight - 6
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
            TabBtn {
                label: "Document"
                active: tabs.activeId === ""
                onClicked: tabs._select("")
            }
            Repeater {
                model: tabs.ids
                delegate: TabBtn {
                    required property int index
                    required property string modelData
                    label: "Table " + (index + 1)
                    active: tabs.activeId === modelData
                    onClicked: tabs._select(modelData)
                }
            }
            Repeater {
                model: tabs.pdfIds
                delegate: TabBtn {
                    required property string modelData
                    label: tabs._mediaLabel(modelData, "PDF")
                    tooltip: tabs._fullName(modelData, "PDF")
                    active: tabs.activeId === modelData
                    onClicked: tabs._select(modelData)
                }
            }
            Repeater {
                model: tabs.videoIds
                delegate: TabBtn {
                    required property string modelData
                    label: tabs._mediaLabel(modelData, "Video")
                    tooltip: tabs._fullName(modelData, "Video")
                    active: tabs.activeId === modelData
                    onClicked: tabs._select(modelData)
                }
            }
            Repeater {
                model: tabs.sketchIds
                delegate: TabBtn {
                    required property int index
                    required property string modelData
                    label: "Sketch " + (index + 1)
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
        width: 34; height: tabs.rowH
        color: tabMenu.visible ? Theme.colors.divider
             : (menuMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.colors.border }
        Icon { anchors.centerIn: parent; name: "list"; size: 16
               color: tabMenu.visible ? Theme.colors.textBright : Theme.colors.textMuted }
        MouseArea { id: menuMA; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tabMenu.visible ? tabMenu.close() : tabMenu.open() }
    }

    // --- The navigator popup: scrollable, sectioned, full filenames. Opens
    // upward (the strip lives at the window's bottom) and right-aligns to the
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
        width: menuCol.width; height: 28
        readonly property bool active: tabs.activeId === tabId
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
                    onClicked: { tabs._select(parent.tabId); tabMenu.close() } }
    }

    Popup {
        id: tabMenu
        padding: 1
        width: 320
        height: Math.min(360, menuCol.implicitHeight + 2)
        x: tabs.width - width
        y: -height - 2
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
                MenuItem { label: "Document"; tabId: "" }
                SectionHeader { visible: tabs.ids.length > 0; title: "Tables" }
                Repeater {
                    model: tabs.ids
                    delegate: MenuItem {
                        required property int index
                        required property string modelData
                        label: "Table " + (index + 1); tabId: modelData
                    }
                }
                SectionHeader { visible: tabs.videoIds.length > 0; title: "Videos" }
                Repeater {
                    model: tabs.videoIds
                    delegate: MenuItem {
                        required property string modelData
                        label: tabs._fullName(modelData, "Video"); tabId: modelData
                    }
                }
                SectionHeader { visible: tabs.pdfIds.length > 0; title: "PDFs" }
                Repeater {
                    model: tabs.pdfIds
                    delegate: MenuItem {
                        required property string modelData
                        label: tabs._fullName(modelData, "PDF"); tabId: modelData
                    }
                }
                SectionHeader { visible: tabs.sketchIds.length > 0; title: "Sketches" }
                Repeater {
                    model: tabs.sketchIds
                    delegate: MenuItem {
                        required property int index
                        required property string modelData
                        label: "Sketch " + (index + 1); tabId: modelData
                    }
                }
            }
        }
    }
}
