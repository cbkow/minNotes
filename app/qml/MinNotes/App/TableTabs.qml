import QtQuick

// Bottom tab strip — appears only when the document contains a table. The first
// tab is always the whole Document; each table gets a tab in appearance order
// ("Table N"). Tabs are keyed by block id (so the active tab follows its table
// across reorders) while the label/position is derived live from current order.
Rectangle {
    id: tabs
    property var editor: null

    readonly property var ids: (blockModel.layoutRevision, blockModel.contentRevision,
                                blockModel.tableBlockIds())
    visible: ids.length > 0
    height: visible ? 30 : 0
    color: Theme.colors.surface

    // A divider line along the top edge.
    Rectangle { width: parent.width; height: 1; color: Theme.colors.border }

    // One tab. `active` highlights it; clicking selects it.
    component TabBtn: Rectangle {
        property string label: ""
        property bool active: false
        signal clicked()
        width: tabLabel.implicitWidth + 24
        height: tabs.height
        color: active ? Theme.colors.bg : (tabMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
        Rectangle {   // active underline (accent)
            visible: parent.active
            anchors.bottom: parent.bottom; width: parent.width; height: 2
            color: Theme.colors.accent
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
            active: !!tabs.editor && tabs.editor.activeTableId === ""
            onClicked: if (tabs.editor) tabs.editor.activeTableId = ""
        }
        Repeater {
            model: tabs.ids
            delegate: TabBtn {
                required property int index
                required property string modelData
                label: "Table " + (index + 1)
                active: !!tabs.editor && tabs.editor.activeTableId === modelData
                onClicked: if (tabs.editor) tabs.editor.activeTableId = modelData
            }
        }
    }
}
