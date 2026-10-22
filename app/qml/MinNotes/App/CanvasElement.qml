// One pooled element on the canvas (2.0): the slot's element index → its id, kind and world rect;
// the kind picks the content — a page card, an ink item, a media item, a shape (connectors land
// with their tool; a frame meanwhile). VISIBILITY RULE (see BlockView.qml): an idle slot rides opacity.
import QtQuick

Item {
    id: el
    required property var editor
    required property var cam
    property int elementIndex: -1
    // Paint order = the FILE's z order (the index into the z-ordered mirror), not the slot order: the pool hands a
    // new element whatever slot is free, so without this a fresh stroke over a PDF painted UNDER it until a reload
    // re-assigned the slots in order (walk 7d, 2026-10-18). Changes only on structure, never per frame.
    z: elementIndex
    readonly property string elementId: elementIndex >= 0 ? (canvas.elementsRevision, canvas.elementIdAt(elementIndex)) : ""
    readonly property bool active: elementId !== ""
    readonly property string kind: active ? (canvas.elementsRevision, canvas.elementKind(elementId)) : ""
    // The content stays loaded while the slot idles (the pool rule: an idle slot rides opacity, never
    // child churn) — a page that scrolls back in keeps its LOD tier and its facade.
    property string shownKind: ""
    onKindChanged: if (kind !== "") shownKind = kind
    readonly property rect r: active ? (canvas.geometryRevision, canvas.elementRect(elementId)) : Qt.rect(0, 0, 0, 0)
    // A child of a FIXED frame (a board) is CLIPPED to the board (2026-10-22, user: "boards aren't cropping"): the item is
    // the visible part (hit-testing follows), the content sits inside it at the element's full rect.
    readonly property string parentId: active ? (canvas.elementsRevision, canvas.parentOf(elementId)) : ""
    readonly property bool clipped: parentId !== "" && (canvas.elementsRevision, !canvas.frameElastic(parentId))
    readonly property rect clipR: {
        if (!clipped) return r
        const f = (canvas.geometryRevision, canvas.elementRect(parentId))
        const x0 = Math.max(r.x, f.x), y0 = Math.max(r.y, f.y), x1 = Math.min(r.x + r.width, f.x + f.width), y1 = Math.min(r.y + r.height, f.y + f.height)
        return Qt.rect(x0, y0, Math.max(0, x1 - x0), Math.max(0, y1 - y0))
    }
    readonly property real fullH: Math.max(kind === "page" ? 49 : 1, r.height)   // == CanvasDocument::cullRect
    x: clipR.x; y: clipR.y; width: Math.max(1, clipR.width); height: clipped ? Math.max(1, clipR.height) : fullH
    clip: clipped
    opacity: active && (!clipped || (clipR.width > 0 && clipR.height > 0)) ? 1 : 0
    enabled: active
    // The element handles a tap only when nothing of its own stands under it: a tap over a frame's CHILD belongs to the
    // child (both items' TapHandlers see the press; the frame's used to fire last and win the selection).
    function overOwnChild(p) {
        if (kind !== "group") return false
        const wx = x + p.x, wy = y + p.y
        for (const cid of canvas.childrenOf(elementId)) { const c = canvas.elementRect(cid); if (wx >= c.x && wx <= c.x + c.width && wy >= c.y && wy <= c.y + c.height) return true }
        return false
    }

    // Forwards for the view's per-frame row budget and the probe (pages only).
    readonly property var page: shownKind === "page" ? content.item : null
    readonly property bool needsRows: page ? page.needsRows : false
    function feedRows(n) { if (page) page.feedRows(n) }
    readonly property int tier: page ? page.tier : -1
    readonly property int liveRows: page ? page.liveRows : 0
    readonly property int wantRows: page ? page.wantRows : 0
    readonly property var facade: page ? page.facade : null
    readonly property var pageModel: page ? page.pageModel : null
    readonly property real originY: page ? page.originY : 0
    function delegateAt(i) { return page ? page.delegateAt(i) : null }

    // Pointer (type / select tools): tap selects, double-tap drills into a page, drag moves. The
    // drag reads the SCENE delta (zoom-free) and rebases the world rect; a run coalesces into one
    // undo entry ("move:<id>"). Space held = hand: the view's pan handler takes over.
    readonly property var view: parent && parent.parent ? parent.parent : null   // canvasLayer → CanvasView
    readonly property bool isLive: view && view.liveKind === "page" && view.liveId === elementId   // the Editor owns the pointer inside a live card
    TapHandler {
        id: tap
        enabled: el.active && el.view && !el.view.drawing && !el.view.handTool && !el.view.pageTool && !el.view.frameTool && !el.isLive
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.DragThreshold
        // (the signals' first argument is an eventPoint, which carries no modifiers — the handler's point does)
        readonly property bool shift: (point.modifiers & Qt.ShiftModifier) !== 0
        // THE LADDER (ruled 2026-10-22, simplified the same day — no in-place form any more): click = select ·
        // double-click (or Return) = OPEN — the page view, the studio, the PDF view, the lightbox; a text chip's
        // open form is its editor.
        onSingleTapped: (ev) => {
            if (el.overOwnChild(ev.position)) return
            ++el.view.taps
            if (tap.shift) el.view.toggleSelect(el.elementId)
            else if (!el.view.isSelected(el.elementId)) el.view.select(el.elementId)
            el.editor.forceActiveFocus()
        }
        onDoubleTapped: (ev) => {
            if (el.overOwnChild(ev.position)) return
            ++el.view.doubleTaps
            if (el.kind === "shape") { el.view.editText(el.elementId); return }
            el.view.select(el.elementId); el.view.openSelected()   // (Shift-double-click is the same rung — it stays for muscle memory)
        }
    }
    DragHandler {   // moves the whole SELECTION (this element joins it if it wasn't in it) as one history row
        enabled: el.active && el.view && !el.view.drawing && !el.view.handTool && !el.view.pageTool && !el.view.frameTool && !el.isLive
        acceptedButtons: Qt.LeftButton
        target: null
        onActiveChanged: {
            if (active) {
                if (!el.view.isSelected(el.elementId)) el.view.select(el.elementId)
                el.editor.forceActiveFocus()   // a jittery click past the drag threshold is a SELECTION too: the keys must land in the editor (walk 7c)
                el.view.beginSelectionDrag()
                ++el.view.elementDrags
            } else el.view.endSelectionDrag()
        }
        onCentroidChanged: {
            if (!active) return
            const z = el.view.cam.zoom
            el.view.dragSelectionTo((centroid.scenePosition.x - centroid.scenePressPosition.x) / z, (centroid.scenePosition.y - centroid.scenePressPosition.y) / z)
        }
    }
    Loader {
        id: content
        x: el.r.x - el.clipR.x; y: el.r.y - el.clipR.y; width: Math.max(1, el.r.width); height: el.fullH   // the full element inside the (possibly clipped) item
        sourceComponent: el.shownKind === "page" ? pageComponent
                       : el.shownKind === "ink" ? inkComponent
                       : el.shownKind === "media" ? mediaComponent
                       : el.shownKind === "shape" ? shapeComponent
                       : el.shownKind === "group" ? frameComponent
                       : el.shownKind === "" ? null : placeholderComponent
    }
    Component {
        id: pageComponent
        CanvasPage { editor: el.editor; cam: el.cam; elementId: el.elementId }
    }
    Component { id: inkComponent;   CanvasInkItem   { editor: el.editor; cam: el.cam; elementId: el.elementId } }
    Component { id: mediaComponent; CanvasMediaItem { editor: el.editor; cam: el.cam; elementId: el.elementId } }
    Component { id: shapeComponent; CanvasShapeItem { editor: el.editor; cam: el.cam; elementId: el.elementId } }
    Component { id: frameComponent; CanvasFrameItem { editor: el.editor; cam: el.cam; elementId: el.elementId } }   // a group / a board (2026-10-22)
    readonly property var frameItem: shownKind === "group" ? content.item : null   // probe + the title edit
    readonly property var inkItem: shownKind === "ink" ? content.item : null   // probe: strokeCount / vertexCount
    readonly property var mediaItem: shownKind === "media" ? content.item : null   // probe: pdfPage / posterFrame
    Component {   // connectors: a frame until their renderer lands
        id: placeholderComponent
        Rectangle {
            color: "transparent"; border.width: 1; border.color: Theme.colors.border; radius: Theme.dim.radius
            Text { anchors.centerIn: parent; text: el.kind; color: Theme.colors.textSubtle
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome }
        }
    }
}
