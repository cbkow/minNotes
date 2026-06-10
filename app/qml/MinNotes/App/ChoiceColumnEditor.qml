import QtQuick
import QtQuick.Controls

// Modal editor for a choice column's option set. Edits a LOCAL draft (a ListModel)
// so Cancel discards and Done commits the whole set in ONE step via
// BlockModel.tableSetColumnOptions → mutateTable (one undo entry). Existing option
// ids are preserved, so cell selections survive a rename/recolor/reorder; deleting
// an option drops only the cells that referenced it. Colours: a curated swatch grid
// plus a full HSV picker (ColorPickerInline) for a custom colour.
Popup {
    id: editor
    property int row: -1
    property int c: -1
    property int editingColor: -1     // which draft row's colour is being edited (−1 = none)
    property bool customOpen: false
    property string editColor: ""     // reactive mirror of the editing row's colour (ListModel.get isn't reactive)

    // Entering colour-edit for a row: seed the mirror + the HSV picker from its colour.
    onEditingColorChanged: {
        editColor = (editingColor >= 0 ? (draft.get(editingColor).color || "") : "")
        if (editingColor >= 0 && editColor !== "") cpick.value = editColor
    }
    function setEditColor(hex) {
        if (editingColor < 0) return
        draft.setProperty(editingColor, "color", hex)
        editColor = hex
    }

    readonly property var palette: ["#c0563f", "#c08a3e", "#b3a13a", "#5a8f4e",
                                    "#3f9d8f", "#3f7fa6", "#4f63c0", "#7b5ea7",
                                    "#a64f7e", "#a6504f", "#6a737d", "#9aa0a6"]

    modal: true
    dim: true
    focus: true   // required for the rename TextInputs to receive keystrokes
    contentWidth: 320   // children fill parent.width, so the Popup needs an explicit content width
    padding: 12
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.45) }
    background: Rectangle {
        color: Theme.colors.surface; radius: 0   // squared, family style
        border.width: 1; border.color: Theme.colors.border
    }

    ListModel { id: draft }

    function open2(trow, col) {
        editor.row = trow; editor.c = col
        draft.clear()
        var opts = blockModel.tableColumnOptions(trow, col)
        for (var i = 0; i < opts.length; ++i)
            draft.append({ oid: opts[i].id, label: opts[i].label, color: opts[i].color })
        editor.editingColor = -1; editor.customOpen = false
        editor.open()
    }
    function nextColor() { return palette[draft.count % palette.length] }
    function colToHex(col) {
        function h2(x) { var s = Math.round(x * 255).toString(16); return s.length < 2 ? "0" + s : s }
        return "#" + h2(col.r) + h2(col.g) + h2(col.b)
    }
    function commit() {
        var arr = []
        for (var i = 0; i < draft.count; ++i) {
            var o = draft.get(i)
            if (o.label.trim().length === 0) continue   // drop blank-labelled options
            arr.push({ id: o.oid, label: o.label, color: o.color })
        }
        blockModel.tableSetColumnOptions(editor.row, editor.c, arr)
        editor.close()
    }

    // a small flat icon button
    component MiniIcon: Item {
        property string name: ""
        signal tap()
        width: 18; height: 18
        HoverHandler { id: mh }
        Icon { anchors.centerIn: parent; name: parent.name; size: 14
               color: mh.hovered ? Theme.colors.text : Theme.colors.textMuted }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: parent.tap() }
    }

    contentItem: Column {
        spacing: 10

        Text {
            text: qsTr("Edit options")
            color: Theme.colors.textBright
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody; font.bold: true
        }

        // option rows
        Column {
            width: parent.width; spacing: 3
            Repeater {
                model: draft
                delegate: Item {
                    id: optRow
                    required property int index
                    required property string oid
                    required property string label
                    required property string color   // maps to the model role (Item has no `color` to collide with)
                    width: parent.width; height: 28

                    Rectangle {   // row background / selection highlight
                        anchors.fill: parent
                        color: editor.editingColor === optRow.index ? Theme.colors.surfaceHover : "transparent"
                    }
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left; anchors.leftMargin: 2
                        anchors.right: parent.right; anchors.rightMargin: 2
                        spacing: 4

                        // colour swatch (click → edit this row's colour)
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16; height: 16
                            color: optRow.color !== "" ? optRow.color : Theme.colors.textMuted
                            border.width: editor.editingColor === optRow.index ? 2 : 1
                            border.color: editor.editingColor === optRow.index ? Theme.colors.accent : Theme.colors.border
                            MouseArea {
                                anchors.fill: parent; anchors.margins: -3; cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    editor.editingColor = (editor.editingColor === optRow.index) ? -1 : optRow.index
                                    editor.customOpen = false
                                }
                            }
                        }
                        // label (inline rename) — modelLabel keeps text synced after a
                        // user edit breaks the binding (reorder/reload re-forces it).
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: optRow.width - 92; height: 24
                            color: Theme.colors.codeBg
                            border.width: 1; border.color: Theme.colors.border
                            TextInput {
                                id: lf
                                anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6
                                verticalAlignment: TextInput.AlignVCenter; clip: true
                                text: optRow.label
                                // resync only when we're NOT the one editing (reorder/reload),
                                // so it never fights the user's keystrokes.
                                property string srcLabel: optRow.label
                                onSrcLabelChanged: if (!activeFocus) text = srcLabel
                                color: Theme.colors.text
                                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                                // commit every keystroke to the draft — no editingFinished timing to miss.
                                onTextEdited: if (optRow.index >= 0) draft.setProperty(optRow.index, "label", text)
                            }
                        }
                        // reorder + delete
                        MiniIcon { anchors.verticalCenter: parent.verticalCenter; name: "caret-up"
                                   onTap: if (optRow.index > 0) draft.move(optRow.index, optRow.index - 1, 1) }
                        MiniIcon { anchors.verticalCenter: parent.verticalCenter; name: "caret-down"
                                   onTap: if (optRow.index < draft.count - 1) draft.move(optRow.index, optRow.index + 1, 1) }
                        MiniIcon { anchors.verticalCenter: parent.verticalCenter; name: "x"
                                   onTap: { if (editor.editingColor === optRow.index) editor.editingColor = -1; draft.remove(optRow.index) } }
                    }
                }
            }
        }

        // add option — flat, full-width
        FlatButton {
            width: parent.width
            text: qsTr("+ Add option")
            onClicked: draft.append({ oid: "", label: qsTr("New option"), color: editor.nextColor() })
        }

        // colour editor for the selected row (preview + swatch grid + custom HSV)
        Column {
            visible: editor.editingColor >= 0
            width: parent.width; spacing: 8
            Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
            Row {   // current-colour preview + hex (reactive via editor.editColor)
                spacing: 8
                Rectangle {
                    width: 22; height: 22
                    color: editor.editColor !== "" ? editor.editColor : Theme.colors.textMuted
                    border.width: 1; border.color: Theme.colors.border
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: editor.editColor
                    color: Theme.colors.textMuted
                    font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
                }
            }
            Flow {
                width: parent.width; spacing: 6
                Repeater {
                    model: editor.palette.length
                    delegate: Rectangle {
                        required property int index
                        readonly property string sw: editor.palette[index]
                        width: 22; height: 22; color: sw
                        border.width: editor.editColor === sw ? 2 : 1
                        border.color: editor.editColor === sw ? Theme.colors.textBright : Theme.colors.border
                        MouseArea {
                            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                            onClicked: editor.setEditColor(sw)
                        }
                    }
                }
                Rectangle {   // custom-colour toggle (square, flat)
                    width: 22; height: 22; color: editor.customOpen ? Theme.colors.surfaceHover : "transparent"
                    border.width: 1; border.color: editor.customOpen ? Theme.colors.accent : Theme.colors.border
                    Icon { anchors.centerIn: parent; name: "eyedropper"; size: 13; color: Theme.colors.textMuted }
                    MouseArea {
                        anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            editor.customOpen = !editor.customOpen
                            if (editor.customOpen && editor.editColor !== "") cpick.value = editor.editColor
                        }
                    }
                }
            }
            ColorPickerInline {
                id: cpick
                visible: editor.customOpen
                onValueChanged: if (editor.customOpen && editor.editingColor >= 0)
                                    editor.setEditColor(editor.colToHex(value))
            }
        }

        // footer — flat buttons (Cancel default, Done primary)
        Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
        Row {
            anchors.right: parent.right
            spacing: 6
            FlatButton { width: 76; text: qsTr("Cancel"); onClicked: editor.close() }
            FlatButton { width: 76; text: qsTr("Done"); variant: "primary"; onClicked: editor.commit() }
        }
    }
}
