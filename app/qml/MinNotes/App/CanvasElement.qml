// One pooled element on the canvas (2.0): the slot's element index → its id, kind and world rect;
// the kind picks the content — a page card, an ink item, a media item, a shape (connectors land
// with their tool; a frame meanwhile). VISIBILITY RULE (see BlockView.qml): an idle slot rides opacity.
import QtQuick

Item {
    id: el
    required property var editor
    required property var cam
    property int elementIndex: -1
    readonly property string elementId: elementIndex >= 0 ? (canvas.elementsRevision, canvas.elementIdAt(elementIndex)) : ""
    readonly property bool active: elementId !== ""
    readonly property string kind: active ? (canvas.elementsRevision, canvas.elementKind(elementId)) : ""
    // The content stays loaded while the slot idles (the pool rule: an idle slot rides opacity, never
    // child churn) — a page that scrolls back in keeps its LOD tier and its facade.
    property string shownKind: ""
    onKindChanged: if (kind !== "") shownKind = kind
    readonly property rect r: active ? (canvas.geometryRevision, canvas.elementRect(elementId)) : Qt.rect(0, 0, 0, 0)
    x: r.x; y: r.y; width: Math.max(1, r.width); height: Math.max(kind === "page" ? 49 : 1, r.height)   // == CanvasDocument::cullRect
    opacity: active ? 1 : 0
    enabled: active

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
    TapHandler {
        enabled: el.active && el.view && !el.view.drawing
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.DragThreshold
        onSingleTapped: { el.view.select(el.elementId); el.editor.forceActiveFocus() }
        onDoubleTapped: if (el.kind === "page") canvas.focusPage = el.elementId
    }
    DragHandler {
        enabled: el.active && el.view && !el.view.drawing && !el.view.spaceHeld
        acceptedButtons: Qt.LeftButton
        target: null
        property rect start
        onActiveChanged: if (active) { start = canvas.elementRect(el.elementId); el.view.select(el.elementId) }
        onCentroidChanged: {
            if (!active) return
            const z = el.view.cam.zoom
            const dx = (centroid.scenePosition.x - centroid.scenePressPosition.x) / z
            const dy = (centroid.scenePosition.y - centroid.scenePressPosition.y) / z
            canvas.setElementRect(el.elementId, Qt.rect(start.x + dx, start.y + dy, start.width, start.height), "move:" + el.elementId)
        }
    }
    Loader {
        id: content
        anchors.fill: parent
        sourceComponent: el.shownKind === "page" ? pageComponent
                       : el.shownKind === "ink" ? inkComponent
                       : el.shownKind === "media" ? mediaComponent
                       : el.shownKind === "shape" ? shapeComponent
                       : el.shownKind === "" ? null : placeholderComponent
    }
    Component {
        id: pageComponent
        CanvasPage { editor: el.editor; cam: el.cam; elementId: el.elementId }
    }
    Component { id: inkComponent;   CanvasInkItem   { editor: el.editor; cam: el.cam; elementId: el.elementId } }
    Component { id: mediaComponent; CanvasMediaItem { editor: el.editor; cam: el.cam; elementId: el.elementId } }
    Component { id: shapeComponent; CanvasShapeItem { editor: el.editor; cam: el.cam; elementId: el.elementId } }
    readonly property var inkItem: shownKind === "ink" ? content.item : null   // probe: strokeCount / vertexCount
    Component {   // connectors: a frame until their renderer lands
        id: placeholderComponent
        Rectangle {
            color: "transparent"; border.width: 1; border.color: Theme.colors.border; radius: 0
            Text { anchors.centerIn: parent; text: el.kind; color: Theme.colors.textSubtle
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome }
        }
    }
}
