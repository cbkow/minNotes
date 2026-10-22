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
    readonly property bool grid: spec.layout === "grid"
    readonly property var cellAspects: [["16:9", 9 / 16], ["4:3", 3 / 4], ["1:1", 1], ["9:16", 16 / 9], ["2.39", 1 / 2.39]]
    readonly property var boardPresets: [["16:9", 960, 540], ["4:3", 960, 720], ["9:16", 540, 960], ["2.39", 960, 402], ["A4", 960, 679], ["A4 ↕", 679, 960], ["Letter", 960, 742], ["Letter ↕", 742, 960]]
    function setField(key, value) { canvas.setFrameField(frameId, key, value); if (editor) editor.forceActiveFocus() }
    function preset(w, h) { const r = canvas.elementRect(frameId); canvas.setFrameField(frameId, "sizing", "fixed"); canvas.setElementRect(frameId, Qt.rect(r.x, r.y, w, h)); if (editor) editor.forceActiveFocus() }
    component Stepper: Row {   // − n +
        property string label: ""
        property int value: 0
        property int min: 1
        property int max: 12
        property int stepBy: 1
        signal changed(int v)
        spacing: 4
        Text { anchors.verticalCenter: parent.verticalCenter; text: label; width: 58; color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
        FlatButton { iconName: "minus"; implicitWidth: 22; implicitHeight: 22; iconSize: 11; enabled_: value > min; onClicked: changed(Math.max(min, value - stepBy)) }
        Text { anchors.verticalCenter: parent.verticalCenter; width: 26; horizontalAlignment: Text.AlignHCenter; text: value; color: Theme.colors.textBright; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall }
        FlatButton { iconName: "plus"; implicitWidth: 22; implicitHeight: 22; iconSize: 11; enabled_: value < max; onClicked: changed(Math.min(max, value + stepBy)) }
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
            FlatButton { text: qsTr("Group"); implicitHeight: 24; checked: fp.spec.sizing !== "fixed"; enabled_: !fp.grid; tooltip: fp.grid ? qsTr("A grid frame is fixed") : qsTr("Elastic — the frame hugs its contents"); tooltipSide: "bottom"
                         onClicked: fp.setField("sizing", "elastic") }
            FlatButton { text: qsTr("Board"); implicitHeight: 24; checked: fp.spec.sizing === "fixed"; tooltip: qsTr("Fixed — sized by its handles, or a preset below"); tooltipSide: "bottom"
                         onClicked: fp.setField("sizing", "fixed") }
        }
        Flow {   // board size presets (fixed only)
            visible: fp.spec.sizing === "fixed"
            width: parent.width; spacing: 2
            Repeater {
                model: fp.boardPresets
                delegate: FlatButton { required property var modelData; text: modelData[0]; implicitHeight: 22; tooltip: modelData[1] + " × " + modelData[2]; tooltipSide: "bottom"
                                       onClicked: fp.preset(modelData[1], modelData[2]) }
            }
        }
        Row {   // the layout
            spacing: 2
            FlatButton { text: qsTr("Free"); implicitHeight: 24; checked: !fp.grid; tooltip: qsTr("Children sit where you put them"); tooltipSide: "bottom"; onClicked: fp.setField("layout", "free") }
            FlatButton { text: qsTr("Grid"); implicitHeight: 24; checked: fp.grid; tooltip: qsTr("Children fill uniform cells in reading order; the board's height follows"); tooltipSide: "bottom"; onClicked: fp.setField("layout", "grid") }
        }
        Column {   // the grid's rows
            visible: fp.grid
            spacing: 6
            Stepper { label: qsTr("Columns"); value: fp.spec.cols || 3; min: 1; max: 12; onChanged: (v) => fp.setField("cols", v) }
            Stepper { label: qsTr("Gap"); value: fp.spec.gap !== undefined ? fp.spec.gap : 16; min: 0; max: 96; stepBy: 4; onChanged: (v) => fp.setField("gap", v) }
            Flow {   // the cells' aspect (wraps: five buttons do not fit beside the label)
                width: col.width; spacing: 2
                Text { height: 22; verticalAlignment: Text.AlignVCenter; text: qsTr("Cells"); width: 58; color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
                Repeater {
                    model: fp.cellAspects
                    delegate: FlatButton { required property var modelData; text: modelData[0]; implicitHeight: 22
                                           checked: Math.abs((fp.spec.cellAspect !== undefined ? fp.spec.cellAspect : 9 / 16) - modelData[1]) < 0.001
                                           onClicked: fp.setField("cellAspect", modelData[1]) }
                }
            }
        }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.colors.border }
}
