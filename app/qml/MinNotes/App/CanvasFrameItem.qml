// A FRAME element on the canvas (2026-10-22): a GROUP (elastic — it hugs its children) or a BOARD (fixed, a grid
// later). A tinted, bordered rect with a title strip along the top; the children are ordinary elements painted above
// it (the frame sits directly under them in z). The title edits in place: a double-click on the frame (the view's open
// rung for a frame) opens the inline editor; Return commits, Esc cancels. Tint "" = the chrome's neutral.
import QtQuick

Item {
    id: fr
    required property var editor
    required property var cam
    property string elementId: ""
    anchors.fill: parent
    readonly property var spec: {
        canvas.elementsRevision
        try { return JSON.parse(elementId !== "" ? canvas.elementJson(elementId) : "{}") } catch (e) { return {} }
    }
    readonly property string title: spec.title || ""
    readonly property bool hasTint: !!spec.tint && spec.tint !== ""
    readonly property color tint: hasTint ? spec.tint : Theme.colors.frameNeutral   // untinted = the dark grey (user 2026-10-22)
    readonly property bool elastic: spec.sizing !== "fixed"
    readonly property real titleH: 28
    readonly property bool editing: titleEdit.visible   // probe
    readonly property var titleItem: titleText

    Rectangle {
        anchors.fill: parent
        radius: Theme.dim.radius
        color: Qt.rgba(fr.tint.r, fr.tint.g, fr.tint.b, fr.hasTint ? 0.10 : 0.18)
        border.width: 1; border.color: Qt.rgba(fr.tint.r, fr.tint.g, fr.tint.b, fr.hasTint ? 0.5 : 0.9)
    }
    Rectangle {   // the title strip
        x: 1; y: 1; width: parent.width - 2; height: fr.titleH
        radius: Theme.dim.radius
        color: Qt.rgba(fr.tint.r, fr.tint.g, fr.tint.b, fr.hasTint ? 0.12 : 0.22)
        Rectangle { anchors.bottom: parent.bottom; anchors.top: parent.verticalCenter; width: parent.width; color: parent.color }   // square the lower corners
        Text {
            id: titleText
            anchors.left: parent.left; anchors.leftMargin: 10; anchors.right: parent.right; anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            visible: !titleEdit.visible
            text: fr.title
            elide: Text.ElideRight
            color: fr.hasTint ? fr.tint : Theme.colors.textMuted
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome; font.bold: true
        }
        TextInput {
            id: titleEdit
            visible: false
            anchors.left: parent.left; anchors.leftMargin: 10; anchors.right: parent.right; anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.colors.textBright
            selectionColor: Theme.colors.accent; selectedTextColor: Theme.colors.textBright
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome; font.bold: true
            selectByMouse: true
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Escape) { fr.cancelTitle(); event.accepted = true }
                else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { fr.commitTitle(); event.accepted = true }
            }
            onActiveFocusChanged: if (!activeFocus && visible) fr.commitTitle()   // a click elsewhere commits
        }
    }
    function editTitle() {
        titleEdit.text = fr.title
        titleEdit.visible = true
        titleEdit.forceActiveFocus()
        titleEdit.selectAll()
    }
    function commitTitle() {
        if (!titleEdit.visible) return
        const t = titleEdit.text.trim()
        titleEdit.visible = false
        if (t !== "" && t !== fr.title && fr.elementId !== "") canvas.setFrameTitle(fr.elementId, t)
        if (fr.editor) fr.editor.forceActiveFocus()
    }
    function cancelTitle() { titleEdit.visible = false; if (fr.editor) fr.editor.forceActiveFocus() }
}
