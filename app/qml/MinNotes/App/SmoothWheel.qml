// SmoothWheel — wheel-MOUSE notches glide instead of jumping (2026-09-16). A trackpad sends
// pixel deltas with the OS's own momentum and stays native (acceptedDevices); a mouse wheel —
// and anything arriving through a remote desktop — sends discrete notches, which a bare
// Flickable applies as jumps. Each notch moves a TARGET; every frame the position eases a
// fraction of the remaining distance toward it, so a run of notches reads as one motion.
// ⌘-wheel (Ctrl on Windows) is the zoom chord: the owner handles it via zoomRequested.
//
//   SmoothWheel { flick: someFlickable; onZoomRequested: (dir, x, y) => … }
//
// A zero-size Item (a handler can't hold the frame animation itself); the handler's scope
// is the Flickable. heightSettled-style nudges that move contentY under a glide must call
// shift(dy) so the target moves with them.
import QtQuick

Item {
    id: root
    required property Flickable flick
    property real pxPerNotch: 96          // one notch (120 units): three lines of body and a bit
    property real ease: 0.25              // fraction of the remaining distance per frame
    property bool horizontalToo: true
    signal zoomRequested(int dir, real x, real y)

    width: 0; height: 0

    property real targetX: 0
    property real targetY: 0
    property bool gliding: false

    function maxX() { return Math.max(0, flick.contentWidth - flick.width) }
    function maxY() { return Math.max(0, flick.contentHeight - flick.height) }
    function shift(dy) { if (gliding) targetY += dy }

    WheelHandler {
        parent: root.flick
        acceptedDevices: PointerDevice.Mouse  // trackpads keep their native, momentum scroll
        onWheel: (event) => {
            if (event.modifiers & Qt.ControlModifier) {          // the zoom chord
                root.zoomRequested(event.angleDelta.y > 0 ? 1 : -1, event.x, event.y)
                return
            }
            if (!root.gliding) { root.targetX = root.flick.contentX; root.targetY = root.flick.contentY }
            let dx = event.angleDelta.x, dy = event.angleDelta.y
            if (event.modifiers & Qt.ShiftModifier) { dx = dy; dy = 0 }   // shift-wheel pans sideways
            root.targetY = Math.max(0, Math.min(root.maxY(), root.targetY - dy / 120 * root.pxPerNotch))
            if (root.horizontalToo) root.targetX = Math.max(0, Math.min(root.maxX(), root.targetX - dx / 120 * root.pxPerNotch))
            root.gliding = true
        }
    }

    FrameAnimation {
        running: root.gliding
        onTriggered: {
            const f = root.flick
            const ty = Math.max(0, Math.min(root.maxY(), root.targetY)), tx = Math.max(0, Math.min(root.maxX(), root.targetX))
            const ry = ty - f.contentY, rx = tx - f.contentX
            if (Math.abs(ry) < 0.5 && Math.abs(rx) < 0.5) {
                f.contentY = ty; f.contentX = tx
                root.gliding = false
                return
            }
            f.contentY += ry * root.ease
            f.contentX += rx * root.ease
        }
    }
}
