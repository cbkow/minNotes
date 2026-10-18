// The selected element's frame and handles (2.0), drawn in SCREEN space over the canvas so the
// chrome stays one pixel wide at every zoom. Edge/corner drags resize through
// canvas.setElementRect(…, "resize:<id>") — a page keeps only its side handles (its width is its
// measure; its height follows its content) and the mutator clamps the measure.
import QtQuick

Item {
    id: frame
    required property var view        // the CanvasView (cam, selectedId)
    readonly property string id_: view.selectedId          // the one id when exactly one is selected
    readonly property bool shown: view.selectionCount > 0
    readonly property bool single: view.selectionCount === 1 && canvas.elementKind(id_) !== ""
    readonly property bool isPage: single && canvas.elementKind(id_) === "page"
    readonly property rect world: shown ? (canvas.geometryRevision, view.selectedIds, view.selectionRect()) : Qt.rect(0, 0, 0, 0)
    readonly property point tl: (view.cam.rev, view.cam.worldToScreen(world.x, world.y))
    x: tl.x; y: tl.y
    width: Math.max(1, world.width * view.cam.zoom); height: Math.max(1, world.height * view.cam.zoom)
    opacity: shown ? 1 : 0
    enabled: shown

    Rectangle { anchors.fill: parent; color: "transparent"; border.width: 1; border.color: Theme.colors.accent; radius: 0 }

    // The OPEN glyph (2.0 phase A4): a 20-px square off the frame's top-right corner, screen space, no
    // accent (the frame carries it) — the "full window" rung of the gesture ladder for a single element.
    readonly property bool canOpen: single && view.canOpen(id_)
    readonly property var openGlyph: glyph   // probe
    function glyphHit(p) {   // p in the view's coordinates: over the open glyph or the PDF pager (the frame's chrome)
        const q = mapFromItem(view, p.x, p.y)
        const inside = (it) => it.visible && q.x >= it.x && q.x <= it.x + it.width && q.y >= it.y && q.y <= it.y + it.height
        return inside(glyph) || inside(pager)
    }
    // The PDF pager (phase A5): ‹ n / N › under a selected PDF element — screen space, chrome tone.
    readonly property bool isPdf: single && canvas.mediaElementKind(id_) === "pdf"
    readonly property var pagerPrev: prevBtn   // probe
    readonly property var pagerNext: nextBtn
    Rectangle {
        id: pager
        visible: frame.isPdf
        x: Math.round(frame.width / 2 - width / 2); y: frame.height + 6
        width: 118; height: 22; radius: 0
        color: Theme.colors.surfaceRaised
        border.width: 1; border.color: Theme.colors.border
        Rectangle {
            id: prevBtn
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: 24; color: prevHover.hovered ? Theme.colors.surfaceHover : "transparent"
            Icon { anchors.centerIn: parent; name: "caret-left"; size: 12; color: Theme.colors.textMuted }
            HoverHandler { id: prevHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { acceptedButtons: Qt.LeftButton; gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: frame.view.pdfStepSelected(-1) }
        }
        Text {
            anchors.centerIn: parent
            text: (frame.view.pdfPageOfSelected + 1) + " / " + Math.max(1, frame.view.pdfPagesOf(frame.id_))
            color: Theme.colors.textMuted
            font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeMono
        }
        Rectangle {
            id: nextBtn
            anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: 24; color: nextHover.hovered ? Theme.colors.surfaceHover : "transparent"
            Icon { anchors.centerIn: parent; name: "caret-right"; size: 12; color: Theme.colors.textMuted }
            HoverHandler { id: nextHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { acceptedButtons: Qt.LeftButton; gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: frame.view.pdfStepSelected(1) }
        }
    }
    Rectangle {
        id: glyph
        visible: frame.canOpen
        x: frame.width + 4; y: -24; width: 20; height: 20; radius: 0
        color: glyphHover.hovered ? Theme.colors.surfaceHover : Theme.colors.surface
        border.width: 1; border.color: Theme.colors.border
        Icon { anchors.centerIn: parent; name: "arrow-square-out"; size: 14
               color: glyphHover.hovered ? Theme.colors.textBright : Theme.colors.textMuted }
        HoverHandler { id: glyphHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { acceptedButtons: Qt.LeftButton; gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: frame.view.openSelected() }
        FlatToolTip { parent: glyph; x: -implicitWidth + glyph.width; y: glyph.height + 6; visible: glyphHover.hovered; text: frame.view.openLabel(frame.id_) }
    }

    // Resize handles: dx/dy say which edges move. Drags read the handler's SCENE delta (zoom-free),
    // divide by the zoom and rebase the world rect.
    component Handle: Rectangle {
        required property int dx     // -1 left edge, 1 right edge, 0 neither
        required property int dy
        visible: frame.single && (!frame.isPage || dy === 0)   // handles resize ONE element; a set only moves
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
