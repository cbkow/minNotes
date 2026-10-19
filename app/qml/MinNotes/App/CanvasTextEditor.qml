// Inline editing of a TEXT shape element (2.0): a TextEdit in SCREEN space over the element's rect, the glyphs at the
// chip's size times the zoom. Opened by the text tool (a tap makes a fresh chip and edits it) or a double-tap on a chip;
// Escape, ⌘Return or a click elsewhere commits; an empty chip is removed. THE BOX FOLLOWS THE TEXT LIVE (2026-10-19):
// auto width = no wrap, the box grows right and down as you type; a pinned width (`fixedW`, set by a handle drag) wraps
// the text and grows the box down — `CanvasView.chipSize` is the one measure the renderer's padding agrees with, so
// nothing jumps on commit. One history row per edit session (a batch from begin to commit; a fresh chip that ends empty
// leaves no row at all).
import QtQuick

Item {
    id: ed
    required property var view
    property string editingId: ""
    property bool batchOpen: false
    property bool fresh: false          // the chip was added inside this batch (newTextAt): empty → discard the batch, no row
    readonly property bool shown: editingId !== ""
    readonly property rect world: shown ? (canvas.geometryRevision, canvas.elementRect(editingId)) : Qt.rect(0, 0, 0, 0)
    readonly property point tl: (view.cam.rev, view.cam.worldToScreen(world.x, world.y))
    readonly property var spec: {
        canvas.elementsRevision
        try { return JSON.parse(shown ? canvas.elementJson(editingId) : "{}") } catch (e) { return {} }
    }
    readonly property real fontSize: (spec.fontSize ? spec.fontSize : Theme.font.sizeBody)
    readonly property bool fixedW: !!spec.fixedW
    readonly property bool plain: !spec.fill || spec.fill === "transparent"
    readonly property color fill: !plain ? spec.fill : "transparent"
    readonly property color textColor: plain ? (spec.color ? spec.color : Theme.colors.text)
                                             : ((fill.r * 0.299 + fill.g * 0.587 + fill.b * 0.114) > 0.6 ? "black" : "white")
    x: tl.x; y: tl.y
    width: Math.max(8, world.width * view.cam.zoom); height: Math.max(8, world.height * view.cam.zoom)
    visible: shown
    z: 30

    function begin(id, inBatch) {
        if (editingId !== "" && editingId !== id) commit()
        if (!inBatch) canvas.beginBatch(qsTr("Edit text"))
        batchOpen = true; fresh = !!inBatch
        editingId = id
        te.text = spec.text || ""
        te.forceActiveFocus()
        te.cursorPosition = te.length
        grow()
    }
    function grow() {   // the box follows the text: wider (auto) / taller (both) — world px from the typed text at the chip's font
        if (editingId === "") return
        const s = view.chipSize(Object.assign({}, spec, { text: te.text }), world.width)
        const r = canvas.elementRect(editingId)
        if (Math.abs(s.width - r.width) > 0.5 || Math.abs(s.height - r.height) > 0.5) canvas.setElementRect(editingId, Qt.rect(r.x, r.y, s.width, s.height), "text:" + editingId)
    }
    function finishBatch(discard) { if (!batchOpen) return; batchOpen = false; if (discard) canvas.discardBatch(); else canvas.endBatch() }
    function commit() {
        if (editingId === "") return
        const id = editingId, text = te.text
        grow()
        editingId = ""
        te.focus = false   // the Editor is a focus scope: without this its sub-focus stays on this TextEdit and keys never reach the canvas
        if (text.trim() === "") {
            canvas.removeElement(id)
            finishBatch(fresh)   // a fresh chip: added, sized and removed in one batch = nothing happened; an existing chip's removal stays undoable
            if (view.selectedId === id) view.clearSelection()
            view.editor.forceActiveFocus(); return
        }
        const s = JSON.parse(canvas.elementJson(id) || "{}")
        if ((s.text || "") !== text) { s.text = text; canvas.setElementJson(id, JSON.stringify(s)) }
        finishBatch(false)
        view.editor.forceActiveFocus()
    }
    function cancel() { editingId = ""; te.focus = false; finishBatch(false); view.editor.forceActiveFocus() }

    Rectangle { anchors.fill: parent; color: ed.fill; border.width: 1; border.color: Theme.colors.accent; radius: 0 }
    TextEdit {
        id: te
        anchors.fill: parent; anchors.margins: Math.round(ed.fontSize * 0.3 * ed.view.cam.zoom)
        wrapMode: ed.fixedW ? TextEdit.Wrap : TextEdit.NoWrap
        color: ed.textColor
        font.family: Theme.font.body; font.pixelSize: Math.max(4, ed.fontSize * ed.view.cam.zoom)
        selectByMouse: true
        onContentWidthChanged: ed.grow()
        onContentHeightChanged: ed.grow()
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Escape) { ed.commit(); event.accepted = true }
            else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) { ed.commit(); event.accepted = true }
        }
        onActiveFocusChanged: if (!activeFocus && ed.editingId !== "") ed.commit()
    }
}
