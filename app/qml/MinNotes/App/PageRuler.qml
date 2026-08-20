import QtQuick

// PageRuler — the always-visible page-width instrument above the document
// (PLAN-page-width M4; user rulings 2026-08-19: always visible, detents
// only). A 14px hairline strip riding the horizontal pan: baseline
// hairline, faint detent ticks, and a bracket handle at the page's right
// edge. Press-drag anywhere to scrub the DOCUMENT's width between detents
// — live reflow preview while dragging, ONE undoable model txn on release
// (a plain click on a tick is the degenerate drag and just jumps).
// Numbers are ALWAYS visible (user ruling 2026-08-19: they identify what
// the strip is for) in mono-10, the block-ruler voice.
// Accent shows only during the drag (sanctioned drag-feedback use); the
// editor tints the ink gutters while dragging so the marginalia zones
// travel visibly with the edge. Dimmed + inert while a draw tool is armed
// (the media-bar pattern — no geometry changes mid-annotation).
Rectangle {
    id: ruler
    property var editor: null
    readonly property var detents: [760, 880, 1000, 1200, 1400, 1600]

    visible: !!editor && editor.activeFrameId === ""   // Document view only
    // 14px instrument + breathing room (ruling 2026-08-20: the flush strip
    // felt cramped) — pad above the numbers and between ticks and baseline.
    // Height is the SHARED token: the Inspector's top bar mirrors this band.
    readonly property int pad: 4
    height: visible ? Theme.dim.rulerHeight : 0
    color: Theme.colors.surface   // match the LeftRail (ruling 2026-08-20)
    readonly property bool inked: !!editor && editor.inkMode
    opacity: inked ? 0.4 : 1
    enabled: !inked && blockModel.documentOpen

    readonly property real cx: editor ? editor.viewContentX : 0
    readonly property real left0: editor ? editor.leftEdge : 120
    readonly property real curW: editor ? editor.pageWidth : 760
    property bool dragging: false
    property int dragW: 760

    function xFor(w) { return left0 + w - cx }
    function nearestDetent(px) {   // item x → the closest detent width
        var w = px + cx - left0
        var best = detents[0], bd = Math.abs(w - best)
        for (var i = 1; i < detents.length; ++i) {
            var d = Math.abs(w - detents[i])
            if (d < bd) { bd = d; best = detents[i] }
        }
        return best
    }
    function commit(w) {
        if (!editor || w === Math.round(blockModel.pageWidth)) return
        var hadInk = editor.inkStrokeCount > 0
        blockModel.setPageWidth(w)   // one undo step (ink migration + width)
        Toasts.show(hadInk
            ? qsTr("Page width %1 — in-column ink may have shifted (⌘Z reverts)").arg(w)
            : qsTr("Page width %1").arg(w), hadInk ? 1 : 0)
    }

    Rectangle {   // baseline hairline against the page
        anchors.bottom: parent.bottom
        width: parent.width; height: 1
        color: Theme.colors.border
    }

    Repeater {   // detent ticks + on-demand numbers
        model: ruler.detents
        delegate: Item {
            required property int modelData
            readonly property bool current: !ruler.dragging
                                            && Math.round(ruler.curW) === modelData
            readonly property bool target: ruler.dragging && ruler.dragW === modelData
            x: ruler.xFor(modelData); y: 0
            width: 1; height: ruler.height
            Rectangle {   // the tick — taller when it's the resting/target width
                anchors.bottom: parent.bottom
                anchors.bottomMargin: ruler.pad - 1   // lifted just off the baseline
                width: 1; height: parent.target || parent.current ? 9 : 5
                color: parent.target ? Theme.colors.accent
                     : parent.current ? Theme.colors.textMuted : Theme.colors.divider
            }
            Text {   // number left of its tick — always up (it names the UI)
                anchors.right: parent.left; anchors.rightMargin: 3
                y: ruler.pad
                text: parent.modelData
                color: parent.target ? Theme.colors.accent
                     : parent.current ? Theme.colors.textMuted : Theme.colors.textSubtle
                font.family: Theme.font.mono; font.pixelSize: 10
            }
        }
    }

    Item {   // the width handle: a bracket hugging the page's right edge
        x: ruler.xFor(ruler.dragging ? ruler.dragW : ruler.curW)
        y: ruler.pad; width: 7; height: ruler.height - 2 * ruler.pad
        Rectangle {   // vertical bar ON the edge
            x: 0; width: 2; height: parent.height
            color: ruler.dragging ? Theme.colors.accent : Theme.colors.textMuted
        }
        Rectangle {   // foot pointing back over the page
            x: -5; anchors.bottom: parent.bottom; anchors.bottomMargin: 1
            width: 5; height: 2
            color: ruler.dragging ? Theme.colors.accent : Theme.colors.textMuted
        }
    }

    MouseArea {
        id: hoverMA
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.SizeHorCursor
        onPressed: (m) => {
            ruler.dragging = true
            ruler.dragW = ruler.nearestDetent(m.x)
            if (ruler.editor) {
                ruler.editor.previewWidth = ruler.dragW   // live reflow preview
                ruler.editor.widthDragging = true
            }
        }
        onPositionChanged: (m) => {
            if (!ruler.dragging) return
            var w = ruler.nearestDetent(m.x)
            if (w === ruler.dragW) return
            ruler.dragW = w
            if (ruler.editor) ruler.editor.previewWidth = w
        }
        onReleased: {
            if (!ruler.dragging) return
            ruler.dragging = false
            // Commit BEFORE clearing the preview: the binding lands on the
            // model's new width with no flicker back through the old one.
            ruler.commit(ruler.dragW)
            if (ruler.editor) {
                ruler.editor.previewWidth = 0
                ruler.editor.widthDragging = false
            }
        }
        onCanceled: {
            ruler.dragging = false
            if (ruler.editor) {
                ruler.editor.previewWidth = 0
                ruler.editor.widthDragging = false
            }
        }
    }
}
