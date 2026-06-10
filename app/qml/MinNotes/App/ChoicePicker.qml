import QtQuick
import QtQuick.Controls

// Option picker for a choice-column table cell. A root-overlay popup (above the
// document mouse layer, which is why a table cell can't host it directly): lists
// the column's shared option set — click one to set the cell, clear the
// selection, add a new (auto-coloured) option, or delete an existing one. Every
// edit goes through BlockModel.table* → mutateTable, so undo / persistence /
// refresh are automatic; this is purely a control surface over that seam.
Popup {
    id: picker
    property int row: -1     // table block row
    property int r: -1       // cell row
    property int c: -1       // column

    padding: 4
    focus: true   // the Popup must hold focus or its TextInput can't receive keystrokes
    contentWidth: 208   // children fill parent.width, so the Popup needs an explicit content width
    // NOT CloseOnReleaseOutside: the picker opens during the cell's press (the mouse
    // layer holds the grab via preventStealing), so the release of that same click is
    // seen as "outside" and would dismiss it before it's visible. CloseOnPressOutside
    // still closes it on a fresh click elsewhere.
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: if (picker.options.length === 0) addField.forceActiveFocus()

    readonly property var options: (row >= 0)
        ? (blockModel.contentRevision, blockModel.tableColumnOptions(row, c)) : []
    readonly property string selectedId: (row >= 0)
        ? (blockModel.contentRevision, blockModel.tableCellChoice(row, r, c)) : ""
    // Auto colour for a new option — rotates through a small palette by position.
    readonly property var palette: ["#c0563f", "#c08a3e", "#5a8f4e", "#3f7fa6",
                                    "#7b5ea7", "#a64f7e", "#6a737d"]
    signal editOptions()   // → the editor opens the modal option editor for this column

    background: Rectangle {
        color: Theme.colors.surface; radius: 6
        border.width: 1; border.color: Theme.colors.border
    }

    contentItem: Column {
        spacing: 1

        Repeater {
            model: picker.options
            delegate: Rectangle {
                id: optRow
                required property var modelData
                width: parent.width; height: 26; radius: 4
                color: rowHover.hovered ? Theme.colors.surfaceHover : "transparent"
                HoverHandler { id: rowHover }

                Rectangle {   // option colour dot
                    anchors.verticalCenter: parent.verticalCenter; x: 6
                    width: 12; height: 12; radius: 6
                    color: optRow.modelData.color !== "" ? optRow.modelData.color : Theme.colors.textMuted
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    x: 24; width: parent.width - 44; elide: Text.ElideRight
                    text: optRow.modelData.label
                    color: Theme.colors.text
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                }
                Text {   // check on the current selection
                    visible: optRow.modelData.id === picker.selectedId
                    anchors.verticalCenter: parent.verticalCenter
                    x: parent.width - 22
                    text: "✓"; color: Theme.colors.accent
                    font.pixelSize: Theme.font.sizeBody
                }
                MouseArea {   // select the option
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { blockModel.tableSetCellChoice(picker.row, picker.r, picker.c, optRow.modelData.id); picker.close() }
                }
            }
        }

        Rectangle { visible: picker.selectedId !== ""; width: parent.width; height: 1; color: Theme.colors.divider }
        Rectangle {   // clear the cell's selection
            visible: picker.selectedId !== ""
            width: parent.width; height: 24; radius: 4
            color: clearMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
            Text {
                anchors.verticalCenter: parent.verticalCenter; x: 24
                text: "Clear"; color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
            }
            MouseArea {
                id: clearMA; anchors.fill: parent; hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: { blockModel.tableSetCellChoice(picker.row, picker.r, picker.c, ""); picker.close() }
            }
        }

        Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
        Rectangle {   // add a new option (stays open so several can be added)
            width: parent.width; height: 26; radius: 4; color: "transparent"
            TextInput {
                id: addField
                anchors.fill: parent
                anchors.leftMargin: 8; anchors.rightMargin: 8
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                color: Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                onAccepted: {
                    var name = text.trim()
                    if (name.length === 0) return
                    var col = picker.palette[picker.options.length % picker.palette.length]
                    blockModel.tableAddOption(picker.row, picker.c, name, col)
                    text = ""
                }
                Text {   // placeholder
                    visible: addField.text.length === 0 && !addField.activeFocus
                    anchors.fill: parent; verticalAlignment: Text.AlignVCenter
                    text: qsTr("+ New option")
                    color: Theme.colors.textMuted
                    font: addField.font
                }
            }
        }
        Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
        Rectangle {   // open the full option editor (rename / colour / reorder / delete)
            width: parent.width; height: 24; radius: 4
            color: editMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
            Text {
                anchors.verticalCenter: parent.verticalCenter; x: 24
                text: qsTr("Edit options…"); color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
            }
            MouseArea {
                id: editMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: { picker.close(); picker.editOptions() }
            }
        }
    }
}
