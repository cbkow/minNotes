// The Inspector's FRAME section (2026-10-22): the one selected frame's title, tint and sizing — a GROUP (elastic:
// it hugs its children) or a BOARD (fixed: sized by hand; a grid lands here in stage 2). Shown only while the canvas
// is up and exactly one frame is selected; the rows below (Layers, History) slide down to make room.
import QtQuick

Item {
    id: fp
    property var editor: null
    readonly property var view: editor ? editor.canvasItem : null
    readonly property string frameId: (canvas.elementsRevision, view && view.selectedId !== "" && canvas.isFrame(view.selectedId) ? view.selectedId : "")
    readonly property var spec: {
        canvas.elementsRevision
        try { return JSON.parse(frameId !== "" ? canvas.elementJson(frameId) : "{}") } catch (e) { return {} }
    }
    readonly property string shownTitle: spec.title || ""
    onShownTitleChanged: if (!titleField.activeFocus) titleField.text = shownTitle
    readonly property var tints: ["", "#c04040", "#d08a2e", "#c7b24a", "#4a9a6a", "#3f7fc0", "#8a5ac0"]
    readonly property var titleFieldItem: titleField   // probe
    visible: frameId !== ""
    height: visible ? col.implicitHeight + 16 : 0
    clip: true

    Column {
        id: col
        x: 12; y: 8; width: parent.width - 24; spacing: 8
        Text { text: qsTr("Frame"); color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: 11; font.bold: true }
        Rectangle {   // the title
            width: parent.width; height: 26; radius: Theme.dim.radiusSmall
            color: Theme.colors.surfaceRecess; border.width: 1; border.color: titleField.activeFocus ? Theme.colors.accent : Theme.colors.border
            TextInput {
                id: titleField
                anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.colors.textBright; selectionColor: Theme.colors.accent; selectedTextColor: Theme.colors.textBright
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
                selectByMouse: true; clip: true
                Component.onCompleted: text = fp.shownTitle
                function commit() {
                    const t = text.trim()
                    if (fp.frameId !== "" && t !== "" && t !== fp.shownTitle) canvas.setFrameTitle(fp.frameId, t)
                    else text = fp.shownTitle
                    if (fp.editor) fp.editor.forceActiveFocus()
                }
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { commit(); event.accepted = true }
                    else if (event.key === Qt.Key_Escape) { text = fp.shownTitle; if (fp.editor) fp.editor.forceActiveFocus(); event.accepted = true }
                }
                onActiveFocusChanged: if (!activeFocus) commit()
            }
        }
        Row {   // the tint: none, then six
            spacing: 6
            Repeater {
                model: fp.tints
                delegate: Rectangle {
                    required property string modelData
                    readonly property bool on: (fp.spec.tint || "") === modelData
                    width: 18; height: 18; radius: 9
                    color: modelData === "" ? "transparent" : modelData
                    border.width: on ? 2 : 1; border.color: on ? Theme.colors.textBright : Theme.colors.border
                    Rectangle { visible: modelData === ""; anchors.centerIn: parent; width: 10; height: 1; rotation: -45; color: Theme.colors.textMuted }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: { canvas.setFrameField(fp.frameId, "tint", modelData); if (fp.editor) fp.editor.forceActiveFocus() } }
                }
            }
        }
        Row {   // the sizing
            spacing: 2
            FlatButton { text: qsTr("Group"); implicitHeight: 24; checked: fp.spec.sizing !== "fixed"; tooltip: qsTr("Elastic — the frame hugs its contents"); tooltipSide: "bottom"
                         onClicked: { canvas.setFrameField(fp.frameId, "sizing", "elastic"); if (fp.editor) fp.editor.forceActiveFocus() } }
            FlatButton { text: qsTr("Board"); implicitHeight: 24; checked: fp.spec.sizing === "fixed"; tooltip: qsTr("Fixed — sized by its handles"); tooltipSide: "bottom"
                         onClicked: { canvas.setFrameField(fp.frameId, "sizing", "fixed"); if (fp.editor) fp.editor.forceActiveFocus() } }
        }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border }
}
