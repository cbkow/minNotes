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
    readonly property var aspects: [["16:9", 9 / 16], ["1:1", 1], ["9:16", 16 / 9], ["4:3", 3 / 4]]   // a board's aspect (its width stays; no sizes — an infinite canvas has no resolution)
    function setField(key, value) { canvas.setFrameField(frameId, key, value); if (editor) editor.forceActiveFocus() }
    function aspect(ratio) {
        const r = canvas.elementRect(frameId)
        canvas.beginBatch(qsTr("Board aspect"))
        canvas.setFrameField(frameId, "sizing", "fixed")
        canvas.setFrameField(frameId, "aspect", ratio)
        canvas.setElementRect(frameId, Qt.rect(r.x, r.y, r.width, Math.max(120, Math.round(r.width * ratio))))
        canvas.endBatch()
        if (editor) editor.forceActiveFocus()
    }
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
        Row {   // the tint: none, then six — the colour flyout's wide clips (18 tall, the small radius, a hairline; selected = bright border)
            spacing: 4
            Repeater {
                model: fp.tints
                delegate: Rectangle {
                    required property string modelData
                    readonly property bool on: (fp.spec.tint || "") === modelData
                    width: Math.floor((col.width - 6 * 4) / 7); height: 18; radius: Theme.dim.radiusSmall
                    color: modelData === "" ? "transparent" : modelData
                    border.width: 1; border.color: on ? Theme.colors.textBright : Theme.colors.border
                    Rectangle { visible: modelData === ""; anchors.centerIn: parent; width: 12; height: 1; rotation: -45; color: Theme.colors.textSubtle }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: fp.setField("tint", modelData) }
                }
            }
        }
        Row {   // the sizing
            spacing: 2
            FlatButton { text: qsTr("Group"); implicitHeight: 24; checked: fp.spec.sizing !== "fixed"; tooltip: qsTr("Elastic — the frame hugs its contents"); tooltipSide: "bottom"
                         onClicked: fp.setField("sizing", "elastic") }
            FlatButton { text: qsTr("Board"); implicitHeight: 24; checked: fp.spec.sizing === "fixed"; tooltip: qsTr("Fixed — sized by its handles"); tooltipSide: "bottom"
                         onClicked: fp.setField("sizing", "fixed") }
        }
        Row {   // a board's aspect: the width stays, the height follows
            visible: fp.spec.sizing === "fixed"
            spacing: 2
            Repeater {
                model: fp.aspects
                delegate: FlatButton { required property var modelData; text: modelData[0]; implicitHeight: 22
                                       checked: fp.spec.aspect !== undefined && Math.abs(fp.spec.aspect - modelData[1]) < 0.001
                                       onClicked: fp.aspect(modelData[1]) }
            }
        }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border }
}
