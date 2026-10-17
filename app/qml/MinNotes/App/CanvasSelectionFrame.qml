// The selected element's frame and handles (2.0), drawn in SCREEN space over the canvas so the
// chrome stays one pixel wide at every zoom. Edge/corner drags resize through
// canvas.setElementRect(…, "resize:<id>") — a page keeps only its side handles (its width is its
// measure; its height follows its content) and the mutator clamps the measure.
import QtQuick

Item {
    id: frame
    required property var view        // the CanvasView (cam, selectedId)
    readonly property string id_: view.selectedId
    readonly property bool shown: id_ !== "" && canvas.elementKind(id_) !== ""
    readonly property bool isPage: shown && canvas.elementKind(id_) === "page"
    readonly property rect world: shown ? (canvas.geometryRevision, canvas.elementRect(id_)) : Qt.rect(0, 0, 0, 0)
    readonly property point tl: (view.cam.rev, view.cam.worldToScreen(world.x, world.y))
    x: tl.x; y: tl.y
    width: Math.max(1, world.width * view.cam.zoom); height: Math.max(1, world.height * view.cam.zoom)
    opacity: shown ? 1 : 0
    enabled: shown

    Rectangle { anchors.fill: parent; color: "transparent"; border.width: 1; border.color: Theme.colors.accent; radius: 0 }

    // Resize handles: dx/dy say which edges move. Drags read the handler's SCENE delta (zoom-free),
    // divide by the zoom and rebase the world rect.
    component Handle: Rectangle {
        required property int dx     // -1 left edge, 1 right edge, 0 neither
        required property int dy
        visible: frame.shown && (!frame.isPage || dy === 0)
        width: 8; height: 8; radius: 0
        color: Theme.colors.accent
        x: (dx < 0 ? 0 : dx > 0 ? frame.width : frame.width / 2) - 4
        y: (dy < 0 ? 0 : dy > 0 ? frame.height : frame.height / 2) - 4
        DragHandler {
            target: null
            property rect start
            onActiveChanged: if (active) start = frame.world
            onCentroidChanged: {
                if (!active) return
                const ddx = (centroid.scenePosition.x - centroid.scenePressPosition.x) / frame.view.cam.zoom
                const ddy = (centroid.scenePosition.y - centroid.scenePressPosition.y) / frame.view.cam.zoom
                frame.view.resizeSelected(frame.view.edgeResize(start, parent.dx, parent.dy, ddx, ddy))
            }
        }
    }
    Handle { dx: -1; dy: -1 } Handle { dx: 0; dy: -1 } Handle { dx: 1; dy: -1 }
    Handle { dx: -1; dy: 0 }                         Handle { dx: 1; dy: 0 }
    Handle { dx: -1; dy: 1 }  Handle { dx: 0; dy: 1 }  Handle { dx: 1; dy: 1 }
}
