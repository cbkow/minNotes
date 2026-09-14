import QtQuick
import QtQuick.Controls

// Option picker for a choice cell or an inline choice chip. A root-overlay popup
// (above the document mouse layer, which is why a cell can't host it directly):
// lists the option set — click one to set the value, clear the selection, add a
// new (auto-coloured) option, or delete an existing one. Every edit goes through
// BlockModel, so undo / persistence / refresh are automatic; this is purely a
// control surface over that seam.
Popup {
    id: picker
    // SPAN MODE (DT-2, 2026-08-20): the control surface over an inline choice
    // chip — the option set lives in the span's payload, writes go through the
    // choice* invokables. sstart = the chip's range start (its stable address;
    // label swaps keep it).
    property int srow: -1
    property int sstart: -1
    readonly property bool spanMode: sstart >= 0
    // GRID MODE (SR-4 S6b): a typed cell of a table — (gridHead, gridR, gridC). The option set is
    // the column's (the header is authoritative), writes go through grid*. The add field doubles
    // as a type-to-filter: Up/Down move the highlight, Enter chooses it or adds the typed text.
    property int gridHead: -1
    property int gridR: -1
    property int gridC: -1
    readonly property bool gridMode: gridHead >= 0 && !spanMode
    property int hi: 0
    readonly property string filterText: gridMode ? addField.text.trim().toLowerCase() : ""
    readonly property var shown: filterText === "" ? options
        : options.filter(function(o) { return o.label.toLowerCase().indexOf(picker.filterText) >= 0 })
    function prefill(text) { addField.text = text; hi = 0 }
    // The table band, so a quick-add (new option + select it) is one undo step.
    function gridGroup(begin) {
        if (!begin) { blockModel.endGroup(); return }
        const recs = blockModel.tableRecords(gridHead)
        blockModel.beginGroup(gridHead, blockModel.splitRowLast(recs[recs.length - 1]))
    }

    padding: 4
    focus: true   // the Popup must hold focus or its TextInput can't receive keystrokes
    contentWidth: 208   // children fill parent.width, so the Popup needs an explicit content width
    // NOT CloseOnReleaseOutside: the picker opens during the cell's press (the mouse
    // layer holds the grab via preventStealing), so the release of that same click is
    // seen as "outside" and would dismiss it before it's visible. CloseOnPressOutside
    // still closes it on a fresh click elsewhere.
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: if (picker.gridMode || picker.options.length === 0) addField.forceActiveFocus()

    readonly property var spanPayload: spanMode
        ? (blockModel.contentRevision, JSON.parse(blockModel.choiceAt(srow, sstart) || "{}")) : null
    readonly property var options: gridMode
        ? (blockModel.contentRevision, blockModel.gridColumnOptions(gridHead, gridC))
        : spanMode
        ? ((spanPayload && spanPayload.o)
               ? spanPayload.o.map(function(o) {
                     return { id: o.id, label: o.l, color: o.c || "" } })
               : [])
        : []
    readonly property string selectedId: gridMode
        ? (blockModel.contentRevision, blockModel.gridCellChoice(gridHead, gridR, gridC))
        : spanMode
        ? ((spanPayload && spanPayload.v) ? spanPayload.v : "")
        : ""
    // Auto colour for a new option — rotates through a small palette by position.
    readonly property var palette: ["#c0563f", "#c08a3e", "#5a8f4e", "#3f7fa6",
                                    "#7b5ea7", "#a64f7e", "#6a737d"]
    signal editOptions()   // → the editor opens the modal option editor for this column

    background: Rectangle {
        color: Theme.colors.surface; radius: 0
        border.width: 1; border.color: Theme.colors.border
    }

    contentItem: Column {
        spacing: 1

        Repeater {
            model: picker.shown
            delegate: Rectangle {
                id: optRow
                required property var modelData
                required property int index
                width: parent.width; height: 26; radius: 0
                color: rowHover.hovered || (picker.gridMode && index === picker.hi) ? Theme.colors.surfaceHover : "transparent"
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
                    onClicked: {
                        if (picker.gridMode)
                            blockModel.gridSetCellChoice(picker.gridHead, picker.gridR, picker.gridC, optRow.modelData.id)
                        else if (picker.spanMode)
                            blockModel.setChoiceSelected(picker.srow, picker.sstart, optRow.modelData.id)
                        picker.close()
                    }
                }
            }
        }

        Rectangle { visible: picker.spanMode || picker.selectedId !== ""; width: parent.width; height: 1; color: Theme.colors.divider }
        Rectangle {   // clear the cell's selection / remove the inline chip
            visible: picker.spanMode || picker.selectedId !== ""
            width: parent.width; height: 24; radius: 0
            color: clearMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
            Text {
                anchors.verticalCenter: parent.verticalCenter; x: 24
                // Inline chips have no empty state (text == label), so the
                // action is REMOVAL (ruling 2026-08-20), and says so.
                text: picker.spanMode ? qsTr("Remove") : qsTr("Clear")
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
            }
            MouseArea {
                id: clearMA; anchors.fill: parent; hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (picker.gridMode)
                        blockModel.gridSetCellChoice(picker.gridHead, picker.gridR, picker.gridC, "")
                    else if (picker.spanMode) blockModel.removeChoiceAt(picker.srow, picker.sstart)
                    picker.close()
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
        Rectangle {   // add a new option (stays open so several can be added)
            width: parent.width; height: 26; radius: 0; color: "transparent"
            TextInput {
                id: addField
                anchors.fill: parent
                anchors.leftMargin: 8; anchors.rightMargin: 8
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                color: Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                onTextChanged: picker.hi = 0
                Keys.onUpPressed: if (picker.gridMode) picker.hi = Math.max(0, picker.hi - 1)
                Keys.onDownPressed: if (picker.gridMode) picker.hi = Math.min(picker.shown.length - 1, picker.hi + 1)
                onAccepted: {
                    var name = text.trim()
                    if (picker.gridMode) {                       // choose the highlighted match, else add
                        if (picker.shown.length > 0) {
                            blockModel.gridSetCellChoice(picker.gridHead, picker.gridR, picker.gridC,
                                                         picker.shown[Math.min(picker.hi, picker.shown.length - 1)].id)
                        } else if (name.length > 0) {
                            picker.gridGroup(true)
                            const id = blockModel.gridAddOption(picker.gridHead, picker.gridC, name,
                                                                picker.palette[picker.options.length % picker.palette.length])
                            if (id !== "") blockModel.gridSetCellChoice(picker.gridHead, picker.gridR, picker.gridC, id)
                            picker.gridGroup(false)
                        }
                        text = ""
                        picker.close()
                        return
                    }
                    if (name.length === 0) return
                    var col = picker.palette[picker.options.length % picker.palette.length]
                    if (picker.spanMode) {
                        // Quick-add SELECTS on an inline chip (the label
                        // follows) — the pick is made, close.
                        blockModel.choiceAddOption(picker.srow, picker.sstart, name, col)
                        text = ""
                        picker.close()
                    }
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
        Rectangle {   // open the full option editor (rename / colour / reorder / delete); grid mode too (S9)
            width: parent.width; height: 24; radius: 0
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
