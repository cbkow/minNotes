// SmoothWheel — wheel-MOUSE notches become MOMENTUM instead of jumps (2026-09-16). A trackpad
// sends pixel deltas with the OS's own momentum and stays native (acceptedDevices); a mouse
// wheel — and anything arriving through a remote desktop — sends discrete notches, which a
// bare Flickable applies as fixed jumps. Here each notch adds a kick to a velocity that
// friction bleeds off every frame: one notch alone travels pxPerNotch and stops softly; a
// burst of notches COMPOUNDS — the kick grows with the burst rate (up to maxGain) and the
// kicks overlap — so a long scroll on a long document amps up and keeps travelling for a
// beat after the wheel stops, the way a flick does. ⌘-wheel (Ctrl on Windows) is the zoom
// chord: the owner handles it via zoomRequested.
//
//   Flickable { SmoothWheel { id: sw; flick: parent; onZoomRequested: … }
//               WheelHandler { onWheel: (e) => sw.handle(e) } }
//
// A zero-size Item holding the state + frame animation. The WheelHandler is declared by the
// OWNER, inside the Flickable: a handler's scope is the item it is declared in — setting its
// `parent` from elsewhere does NOT re-scope it (2026-09-16: events silently went to the bare
// Flickable).
import QtQuick

Item {
    id: root
    required property Flickable flick
    property real pxPerNotch: 96          // one lone notch's total travel (three lines of body and a bit)
    property real friction: 0.90          // velocity kept per 60 Hz frame — the tail's length
    property real maxGain: 6              // a sustained burst's kick multiplier, at most
    property real gainPerNotch: 0.45      // how fast a burst ramps toward maxGain
    property int  burstGapMs: 250         // notches closer than this are one burst
    readonly property bool log: Qt.application.arguments.indexOf("--perf-log") >= 0   // [wheel] lines
    property bool horizontalToo: true
    signal zoomRequested(int dir, real x, real y)

    width: 0; height: 0

    property real vx: 0                   // px per 60 Hz frame
    property real vy: 0
    property real lastMs: 0
    property int  burst: 0
    property bool gliding: false

    function maxX() { return Math.max(0, flick.contentWidth - flick.width) }
    function maxY() { return Math.max(0, flick.contentHeight - flick.height) }

    // Every device: a trackpad's PIXEL deltas apply directly (the OS already supplies its
    // momentum as a stream of them); NOTCH-only events — a mouse wheel, or whatever a remote
    // desktop relays — take the momentum model below.
    function handle(event) {
            const now = Date.now()
            if (root.log) console.log("[wheel] angle", event.angleDelta.x, event.angleDelta.y, "pixel", event.pixelDelta.x, event.pixelDelta.y,
                                      "gap", root.lastMs ? now - root.lastMs : -1, "ms", "mods", event.modifiers, "inverted", event.inverted)
            if (event.modifiers & Qt.ControlModifier) {          // the zoom chord
                root.zoomRequested(event.angleDelta.y > 0 ? 1 : -1, event.x, event.y)
                return
            }
            if (event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0) {   // trackpad: native
                const f = root.flick
                root.vx = 0; root.vy = 0; root.gliding = false
                f.contentY = Math.max(0, Math.min(root.maxY(), f.contentY - event.pixelDelta.y))
                if (root.horizontalToo) f.contentX = Math.max(0, Math.min(root.maxX(), f.contentX - event.pixelDelta.x))
                root.lastMs = now
                return
            }
            root.burst = (now - root.lastMs < root.burstGapMs) ? root.burst + 1 : 0
            root.lastMs = now
            const gain = 1 + Math.min(root.maxGain - 1, root.burst * root.gainPerNotch)
            const kick = root.pxPerNotch * (1 - root.friction) * gain   // Σ kick·friction^n = pxPerNotch·gain
            let dx = event.angleDelta.x, dy = event.angleDelta.y
            if (event.modifiers & Qt.ShiftModifier) { dx = dy; dy = 0 }   // shift-wheel pans sideways
            // A reversal cancels the tail: the wheel means "the other way now", not "slow down".
            const ky = -dy / 120 * kick, kx = -dx / 120 * kick
            if (ky !== 0 && Math.sign(ky) !== Math.sign(root.vy)) root.vy = 0
            if (kx !== 0 && Math.sign(kx) !== Math.sign(root.vx)) root.vx = 0
            root.vy += ky
            if (root.horizontalToo) root.vx += kx
            root.gliding = true
    }

    FrameAnimation {
        running: root.gliding
        onTriggered: {
            const f = root.flick
            const k = Math.max(0.25, Math.min(3, frameTime * 60))   // frames elapsed (dropped frames travel further)
            const my = root.maxY(), mx = root.maxX()
            let ny = f.contentY + root.vy * k, nx = f.contentX + root.vx * k
            if (ny <= 0) { ny = 0; root.vy = 0 } else if (ny >= my) { ny = my; root.vy = 0 }
            if (nx <= 0) { nx = 0; root.vx = 0 } else if (nx >= mx) { nx = mx; root.vx = 0 }
            f.contentY = ny; f.contentX = nx
            const decay = Math.pow(root.friction, k)
            root.vy *= decay; root.vx *= decay
            if (Math.abs(root.vy) < 0.05 && Math.abs(root.vx) < 0.05) { root.vy = 0; root.vx = 0; root.gliding = false }
        }
    }
}
