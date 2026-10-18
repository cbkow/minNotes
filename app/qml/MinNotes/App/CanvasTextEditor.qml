// Inline editing of a TEXT shape element (2.0): a TextEdit in SCREEN space over the element's rect,
// the glyphs at the chip's size times the zoom. Opened by the text tool (a tap makes a fresh chip
// and edits it) or a double-tap on a chip; Escape or a click elsewhere commits — the element takes
// the text and grows to fit it; an empty chip is removed.
import QtQuick

Item {
    id: ed
    required property var view
    property string editingId: ""
    readonly property bool shown: editingId !== ""
    readonly property rect world: shown ? (canvas.geometryRevision, canvas.elementRect(editingId)) : Qt.rect(0, 0, 0, 0)
    readonly property point tl: (view.cam.rev, view.cam.worldToScreen(world.x, world.y))
    readonly property var spec: {
        canvas.elementsRevision
        try { return JSON.parse(shown ? canvas.elementJson(editingId) : "{}") } catch (e) { return {} }
    }
    readonly property real fontSize: (spec.fontSize ? spec.fontSize : Theme.font.sizeBody)
    readonly property bool plain: !spec.fill || spec.fill === "transparent"
    readonly property color fill: !plain ? spec.fill : "transparent"
    readonly property color textColor: plain ? (spec.color ? spec.color : Theme.colors.text)
                                             : ((fill.r * 0.299 + fill.g * 0.587 + fill.b * 0.114) > 0.6 ? "black" : "white")
    x: tl.x; y: tl.y
    width: Math.max(40, world.width * view.cam.zoom); height: Math.max(20, world.height * view.cam.zoom)
    visible: shown
    z: 30

    function begin(id) {
        if (editingId !== "" && editingId !== id) commit()
        editingId = id
        te.text = spec.text || ""
        te.forceActiveFocus()
        te.cursorPosition = te.length
    }
    function commit() {
        if (editingId === "") return
        const id = editingId
        editingId = ""
        te.focus = false   // the Editor is a focus scope: without this its sub-focus stays on this TextEdit and keys never reach the canvas
        const text = te.text
        if (text.trim() === "") { canvas.removeElement(id); if (view.selectedId === id) view.clearSelection(); view.editor.forceActiveFocus(); return }
        const s = JSON.parse(canvas.elementJson(id) || "{}")
        s.text = text
        canvas.setElementJson(id, JSON.stringify(s))
        // Grow the chip to its text (world px: the TextEdit laid out at fontSize × zoom).
        const r = canvas.elementRect(id), pad = Math.round(fontSize * 0.3)
        const w = Math.max(40, te.contentWidth / view.cam.zoom + 2 * pad + 4), h = Math.max(fontSize * 1.4, te.contentHeight / view.cam.zoom + 2 * pad)
        if (Math.abs(w - r.width) > 1 || Math.abs(h - r.height) > 1) canvas.setElementRect(id, Qt.rect(r.x, r.y, w, h), "text:" + id)
        view.editor.forceActiveFocus()
    }
    function cancel() { editingId = ""; te.focus = false; view.editor.forceActiveFocus() }

    Rectangle { anchors.fill: parent; color: ed.fill; border.width: 1; border.color: Theme.colors.accent; radius: 0 }
    TextEdit {
        id: te
        anchors.fill: parent; anchors.margins: Math.round(ed.fontSize * 0.3 * ed.view.cam.zoom)
        wrapMode: TextEdit.NoWrap
        color: ed.textColor
        font.family: Theme.font.body; font.pixelSize: Math.max(4, ed.fontSize * ed.view.cam.zoom)
        selectByMouse: true
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Escape) { ed.commit(); event.accepted = true }
            else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) { ed.commit(); event.accepted = true }
        }
        onActiveFocusChanged: if (!activeFocus && ed.editingId !== "") ed.commit()
    }
}
