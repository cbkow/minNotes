import QtQuick
import QtQuick.Controls

// Modal editor for the cross-OS path mappings (the `pathMap` bridge). Each
// "volume" pairs a label with its root on Windows / macOS / Linux; referenced
// media under a volume is stored portably as {vol,rel} and resolved to this
// machine's root. Edits a LOCAL draft (a ListModel) so Cancel discards and Done
// commits the whole set at once via pathMap.setMappingsJson. Squared, family style.
Popup {
    id: dlg

    modal: true
    dim: true
    focus: true
    contentWidth: 460
    padding: 14
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.45) }
    background: Rectangle {
        color: Theme.colors.surface; radius: 0
        border.width: 1; border.color: Theme.colors.border
    }

    ListModel { id: draft }

    // Seed the draft from the persisted mappings and open.
    function open2() {
        draft.clear()
        var arr = []
        try { arr = JSON.parse(pathMap.mappingsJson()) } catch (e) { arr = [] }
        for (var i = 0; i < arr.length; ++i) {
            var m = arr[i]
            draft.append({ vid: m.id || pathMap.newId(),
                           rlabel: m.label || "", win: m.win || "",
                           mac: m.mac || "", lin: m.lin || "",
                           venabled: m.enabled === undefined ? true : !!m.enabled })
        }
        dlg.open()
    }
    function commit() {
        var arr = []
        for (var i = 0; i < draft.count; ++i) {
            var o = draft.get(i)
            // Drop a row with no roots AND no label (an empty leftover).
            if (!o.rlabel.trim() && !o.win.trim() && !o.mac.trim() && !o.lin.trim()) continue
            arr.push({ id: o.vid, label: o.rlabel, win: o.win, mac: o.mac,
                       lin: o.lin, enabled: o.venabled })
        }
        pathMap.setMappingsJson(JSON.stringify(arr))
        dlg.close()
        Toasts.show(qsTr("Path mappings saved"))
    }

    component MiniIcon: Item {
        property string name: ""
        signal tap()
        width: 18; height: 18
        HoverHandler { id: mh }
        Icon { anchors.centerIn: parent; name: parent.name; size: 14
               color: mh.hovered ? Theme.colors.text : Theme.colors.textMuted }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: parent.tap() }
    }

    // One labeled path field: a caption + a flat text box. `edited(text)` fires
    // on every keystroke so the draft stays current with no editingFinished timing.
    component PathField: Item {
        property string caption: ""
        property string value: ""
        property string placeholder: ""
        signal edited(string v)
        height: 24
        Text {
            id: cap; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
            width: 58; text: caption; color: Theme.colors.textMuted
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }
        Rectangle {
            anchors.left: cap.right; anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: 24; color: Theme.colors.codeBg
            border.width: 1; border.color: Theme.colors.border
            TextInput {
                id: ti
                anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6
                verticalAlignment: TextInput.AlignVCenter; clip: true
                text: value
                color: Theme.colors.text
                font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeMono
                onTextEdited: parent.parent.edited(text)
                Text {   // placeholder
                    anchors.fill: parent; verticalAlignment: Text.AlignVCenter
                    visible: ti.text.length === 0 && !ti.activeFocus
                    text: placeholder; color: Theme.colors.textSubtle
                    font: ti.font; elide: Text.ElideRight
                }
            }
        }
    }

    contentItem: Column {
        spacing: 12

        Text {
            text: qsTr("Path Mappings")
            color: Theme.colors.textBright
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody; font.bold: true
        }
        Text {
            width: parent.width
            text: qsTr("Map a shared volume's root on each OS so referenced media in "
                     + "documents resolves across machines.")
            wrapMode: Text.WordWrap
            color: Theme.colors.textMuted
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
        }

        // Scrollable list of volume cards.
        Flickable {
            id: flick
            width: parent.width
            height: Math.min(360, cards.implicitHeight)
            contentWidth: width; contentHeight: cards.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: MnScrollBar {}
            Column {
                id: cards
                width: flick.width
                spacing: 8
                Repeater {
                    model: draft
                    delegate: Rectangle {
                        id: card
                        required property int index
                        required property string vid
                        required property string rlabel
                        required property string win
                        required property string mac
                        required property string lin
                        required property bool venabled
                        width: cards.width
                        height: cardCol.implicitHeight + 16
                        color: Theme.colors.surfaceRaised
                        border.width: 1; border.color: Theme.colors.border
                        opacity: card.venabled ? 1.0 : 0.55

                        Column {
                            id: cardCol
                            anchors.fill: parent; anchors.margins: 8
                            spacing: 6
                            // header: enable toggle + label + remove
                            Row {
                                width: parent.width; spacing: 8
                                Rectangle {   // enabled checkbox (squared)
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 16; height: 16; radius: 0
                                    color: card.venabled ? Theme.colors.divider : "transparent"
                                    border.width: 1
                                    border.color: card.venabled ? Theme.colors.textBright : Theme.colors.border
                                    Icon { anchors.centerIn: parent; name: "check"; size: 11
                                           visible: card.venabled; color: Theme.colors.textBright }
                                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                                onClicked: draft.setProperty(card.index, "venabled", !card.venabled) }
                                }
                                Rectangle {   // label field
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 16 - 18 - 16; height: 24
                                    color: Theme.colors.codeBg
                                    border.width: 1; border.color: Theme.colors.border
                                    TextInput {
                                        id: lbl
                                        anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6
                                        verticalAlignment: TextInput.AlignVCenter; clip: true
                                        text: card.rlabel
                                        color: Theme.colors.text
                                        font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                                        onTextEdited: draft.setProperty(card.index, "rlabel", text)
                                        Text {
                                            anchors.fill: parent; verticalAlignment: Text.AlignVCenter
                                            visible: lbl.text.length === 0 && !lbl.activeFocus
                                            text: qsTr("Volume label"); color: Theme.colors.textSubtle
                                            font: lbl.font
                                        }
                                    }
                                }
                                MiniIcon { anchors.verticalCenter: parent.verticalCenter; name: "x"
                                           onTap: draft.remove(card.index) }
                            }
                            PathField { width: parent.width; caption: qsTr("Windows"); value: card.win
                                        placeholder: "Z:\\\\share"
                                        onEdited: (v) => draft.setProperty(card.index, "win", v) }
                            PathField { width: parent.width; caption: qsTr("macOS"); value: card.mac
                                        placeholder: "/Volumes/share"
                                        onEdited: (v) => draft.setProperty(card.index, "mac", v) }
                            PathField { width: parent.width; caption: qsTr("Linux"); value: card.lin
                                        placeholder: "/mnt/share"
                                        onEdited: (v) => draft.setProperty(card.index, "lin", v) }
                        }
                    }
                }
            }
        }

        FlatButton {
            width: parent.width
            text: qsTr("+ Add volume")
            onClicked: draft.append({ vid: pathMap.newId(), rlabel: "", win: "",
                                      mac: "", lin: "", venabled: true })
        }

        Rectangle { width: parent.width; height: 1; color: Theme.colors.divider }
        Row {
            anchors.right: parent.right
            spacing: 6
            FlatButton { width: 76; text: qsTr("Cancel"); onClicked: dlg.close() }
            FlatButton { width: 76; text: qsTr("Done"); variant: "primary"; onClicked: dlg.commit() }
        }
    }
}
