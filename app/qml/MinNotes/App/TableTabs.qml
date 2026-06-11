import QtQuick

// Bottom tab strip — appears when the document contains a table or an inline PDF.
// The first tab is always the whole Document; each table gets a "Table N" tab and
// each PDF a tab labelled by its file name. Tabs are keyed by block id (so the
// active tab follows its block across reorders) while the label/position is
// derived live from current order.
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
    height: visible ? 30 : 0
    color: Theme.colors.surface

    // A divider line along the top edge.
    Rectangle { width: parent.width; height: 1; color: Theme.colors.border }

    // Media tabs are labelled by file name (PDFs and videos alike). Truncate
    // in the MIDDLE: production filenames share long prefixes and differ at
    // the tail (_v001 / _v002), so end-truncation makes sibling tabs twins.
    function _mediaLabel(id, fallback) {
        var r = blockModel.rowForId(id)
        var n = r >= 0 ? blockModel.mediaFileName(r) : ""
        if (n === "") return fallback
        return n.length > 22 ? n.substring(0, 9) + "…" + n.substring(n.length - 12) : n
    }

    // One tab. `active` highlights it; clicking selects it.
    component TabBtn: Rectangle {
        property string label: ""
        property bool active: false
        signal clicked()
        width: tabLabel.implicitWidth + 24
        height: tabs.height
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
            font.family: Theme.font.family; font.pixelSize: 13
        }
        MouseArea { id: tabMA; anchors.fill: parent; hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor; onClicked: parent.clicked() }
    }

    Row {
        anchors.left: parent.left; anchors.top: parent.top; height: parent.height
        TabBtn {
            label: "Document"
            active: tabs.activeId === ""
            onClicked: if (tabs.editor) tabs.editor.setActiveTab("")
        }
        Repeater {
            model: tabs.ids
            delegate: TabBtn {
                required property int index
                required property string modelData
                label: "Table " + (index + 1)
                active: tabs.activeId === modelData
                onClicked: if (tabs.editor) tabs.editor.setActiveTab(modelData)
            }
        }
        Repeater {
            model: tabs.pdfIds
            delegate: TabBtn {
                required property string modelData
                label: tabs._mediaLabel(modelData, "PDF")
                active: tabs.activeId === modelData
                onClicked: if (tabs.editor) tabs.editor.setActiveTab(modelData)
            }
        }
        Repeater {
            model: tabs.videoIds
            delegate: TabBtn {
                required property string modelData
                label: tabs._mediaLabel(modelData, "Video")
                active: tabs.activeId === modelData
                onClicked: if (tabs.editor) tabs.editor.setActiveTab(modelData)
            }
        }
        Repeater {
            model: tabs.sketchIds
            delegate: TabBtn {
                required property int index
                required property string modelData
                label: "Sketch " + (index + 1)
                active: tabs.activeId === modelData
                onClicked: if (tabs.editor) tabs.editor.setActiveTab(modelData)
            }
        }
    }
}
