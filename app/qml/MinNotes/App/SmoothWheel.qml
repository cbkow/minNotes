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
//               WheelHandler { acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
//                              onWheel: (e) => sw.handle(e) } }
//
// acceptedDevices MUST include TouchPad: WheelHandler takes only a real mouse wheel by default,
// and macOS reports a remote desktop's relayed wheel (and any precise-delta scroll) as a
// TouchPad device — the handler saw nothing and the bare Flickable scrolled (2026-09-16).
//
// A zero-size Item holding the state + frame animation. The WheelHandler is declared by the
// OWNER, inside the Flickable: a handler's scope is the item it is declared in — setting its
// `parent` from elsewhere does NOT re-scope it (2026-09-16: events silently went to the bare
// Flickable).
import QtQuick

Item {
    id: root
    required property Flickable flick
    property real pxPerNotch: 80          // one lone notch's total travel (about three lines of body)
    property real friction: 0.90          // velocity kept per 60 Hz frame — the tail's length
    property real maxGain: 6              // a sustained burst's kick multiplier, at most
    property real gainPerNotch: 0.5       // how fast the gain ramps per extra notch in the window
    property int  windowMs: 300           // the rate window: notches decay out of it exponentially
    readonly property bool log: Qt.application.arguments.indexOf("--perf-log") >= 0   // [wheel] lines
    property bool horizontalToo: true
    signal zoomRequested(int dir, real x, real y)

    width: 0; height: 0

    property real vx: 0                   // px per 60 Hz frame
    property real vy: 0
    property real lastMs: 0
    property real recent: 0               // notch-equivalents in the rate window (decays with time)
    property bool gliding: false

    // The Flickable's own range — its MARGINS included: a grid frame (a table's tab) clamps the
    // document to the table by setting them, and a range that ignored them scrolled the tab
    // through the whole document (2026-09-16).
    function minX() { return -flick.leftMargin }
    function minY() { return -flick.topMargin }
    function maxX() { return Math.max(minX(), flick.contentWidth - flick.width + flick.rightMargin) }
    function maxY() { return Math.max(minY(), flick.contentHeight - flick.height + flick.bottomMargin) }

    // A trackpad GESTURE (it carries a scroll phase: begin / update / end / momentum) applies its
    // pixel deltas directly — the OS already supplies the momentum as a stream of them. Everything
    // else — a mouse wheel's notches, or whatever a remote desktop relays (pixel deltas WITHOUT a
    // phase) — takes the momentum model below, counting notches by angleDelta.
    function handle(event) {
            const now = Date.now()
            if (root.log) console.log("[wheel] angle", event.angleDelta.x, event.angleDelta.y, "pixel", event.pixelDelta.x, event.pixelDelta.y,
                                      "phase", event.phase, "gap", root.lastMs ? now - root.lastMs : -1, "ms", "mods", event.modifiers, "inverted", event.inverted)
            if (event.modifiers & Qt.ControlModifier) {          // the zoom chord
                root.zoomRequested(event.angleDelta.y > 0 ? 1 : -1, event.x, event.y)
                return
            }
            const gesture = event.phase !== Qt.NoScrollPhase
            if (gesture && (event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0)) {   // trackpad: native
                const f = root.flick
                root.vx = 0; root.vy = 0; root.gliding = false
                f.contentY = Math.max(root.minY(), Math.min(root.maxY(), f.contentY - event.pixelDelta.y))
                if (root.horizontalToo) f.contentX = Math.max(root.minX(), Math.min(root.maxX(), f.contentX - event.pixelDelta.x))
                root.lastMs = now
                return
            }
            // The gain keys on the notch RATE, not the event count: a remote desktop relays one
            // physical notch as several fractional events 0 ms apart (angleDelta ±40 ×3), which an
            // event counter read as a burst — a lone slow notch amplified. `recent` accumulates
            // notch-equivalents and decays over windowMs; the first notch's worth is free.
            const dt = root.lastMs ? now - root.lastMs : root.windowMs
            root.recent = root.recent * Math.exp(-dt / root.windowMs) + Math.abs(event.angleDelta.y) / 120
            root.lastMs = now
            const gain = 1 + Math.min(root.maxGain - 1, Math.max(0, root.recent - 1) * root.gainPerNotch)
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
            const my = root.maxY(), mx = root.maxX(), ly = root.minY(), lx = root.minX()
            let ny = f.contentY + root.vy * k, nx = f.contentX + root.vx * k
            if (ny <= ly) { ny = ly; root.vy = 0 } else if (ny >= my) { ny = my; root.vy = 0 }
            if (nx <= lx) { nx = lx; root.vx = 0 } else if (nx >= mx) { nx = mx; root.vx = 0 }
            f.contentY = ny; f.contentX = nx
            const decay = Math.pow(root.friction, k)
            root.vy *= decay; root.vx *= decay
            if (Math.abs(root.vy) < 0.05 && Math.abs(root.vx) < 0.05) { root.vy = 0; root.vx = 0; root.gliding = false }
        }
    }
}
