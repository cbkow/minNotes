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
    // The box = the element's picture + (a video) its 22-px bar: ONE object with one hit rect (walk 7c). The stored
    // rect stays the picture (geometry, exports, ink untouched); `pictureH` is where the picture ends.
    readonly property real pictureH: Math.max(1, world.height * view.cam.zoom)
    readonly property real barH: transport.visible ? transport.height + 1 : 0   // + the box's bottom border under the bar (the accent stays visible on all four sides)
    width: Math.max(1, world.width * view.cam.zoom); height: pictureH + barH
    opacity: shown ? 1 : 0
    enabled: shown

    Rectangle { x: 0; y: 0; width: Math.max(frame.width, transport.visible ? transport.width + 2 : 0); height: frame.height
                color: "transparent"; border.width: 1; border.color: Theme.colors.accent; radius: 0 }

    // The OPEN glyph (2.0 phase A4): a 20-px square off the frame's top-right corner, screen space, no
    // accent (the frame carries it) — the "full window" rung of the gesture ladder for a single element.
    readonly property bool canOpen: single && view.canOpen(id_)
    readonly property var openGlyph: glyph   // probe
    function glyphHit(p) {   // p in the view's coordinates: over the open glyph, the PDF pager or the video transport (the frame's chrome)
        const q = mapFromItem(view, p.x, p.y)
        const inside = (it) => it.visible && q.x >= it.x && q.x <= it.x + it.width && q.y >= it.y && q.y <= it.y + it.height
        return inside(glyph) || inside(pager) || inside(transport)
    }
    // A selected VIDEO element's transport (walk 2026-10-18): the family bar under the frame, screen space, on the
    // shared player through the editor's canvasVideoHost; the surface paints inside the element meanwhile.
    readonly property bool isVideo: single && canvas.mediaElementKind(id_) === "video"
    readonly property var videoBar: transport   // probe
    CompactTransport {
        id: transport
        visible: frame.isVideo && !!frame.view.editor && !!frame.view.editor.canvasVideoHost
        x: 1; y: frame.pictureH   // inside the box, under the picture, one pixel in from the box's sides and bottom
        width: Math.max(200, frame.width - 2)
        editor: frame.view.editor
        dec: frame.view.editor ? frame.view.editor.videoDecoder : null
        audio: frame.view.editor ? frame.view.editor.audioPlayer : null
        host: frame.view.editor ? frame.view.editor.canvasVideoHost : null
        row: -1
        live: !!frame.view.editor && frame.view.editor.canvasHostPlaying
    }
    // The PDF pager (phase A5): ‹ n / N › under a selected PDF element — screen space, chrome tone.
    readonly property bool isPdf: single && canvas.mediaElementKind(id_) === "pdf"
    readonly property var pagerPrev: prevBtn   // probe
    readonly property var pagerNext: nextBtn
    Rectangle {
        id: pager
        visible: frame.isPdf
        x: Math.round(frame.width / 2 - width / 2); y: frame.pictureH + 6
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
        visible: frame.single   // handles resize ONE element; a set only moves (a page's height too — walk 7c: fixed-height pages)
        width: 8; height: 8; radius: 0
        color: Theme.colors.accent
        z: 3   // above the bar (the bottom handles sit on the picture/bar seam, never on the bar's buttons)
        x: (dx < 0 ? 0 : dx > 0 ? frame.width : frame.width / 2) - 4
        y: (dy < 0 ? 0 : dy > 0 ? frame.pictureH : frame.pictureH / 2) - 4
        TapHandler {   // a double-click on a top/bottom handle of a page = Fit height to content (back to auto)
            enabled: frame.isPage && parent.dy !== 0
            acceptedButtons: Qt.LeftButton
            onDoubleTapped: canvas.fitPageHeight(frame.id_)
        }
        DragHandler {
            target: null
            property rect start
            onActiveChanged: if (active) start = frame.world
            onCentroidChanged: {
                if (!active) return
                const ddx = (centroid.scenePosition.x - centroid.scenePressPosition.x) / frame.view.cam.zoom
                const ddy = (centroid.scenePosition.y - centroid.scenePressPosition.y) / frame.view.cam.zoom
                frame.view.resizeSelected(frame.view.edgeResize(start, parent.dx, parent.dy, ddx, ddy, frame.isVideo))   // a video keeps its aspect: nothing to letterbox or crop
            }
        }
    }
    Handle { dx: -1; dy: -1 } Handle { dx: 0; dy: -1 } Handle { dx: 1; dy: -1 }
    Handle { dx: -1; dy: 0 }                         Handle { dx: 1; dy: 0 }
    Handle { dx: -1; dy: 1 }  Handle { dx: 0; dy: 1 }  Handle { dx: 1; dy: 1 }
}
