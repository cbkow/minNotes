import QtQuick

// Frame-time + virtualization telemetry overlay. Frame timing comes from
// FrameAnimation (fires once per rendered frame); p50/p99/worst are computed
// over a rolling window. This is how P1/P3/P6 become measurements, not opinions.
Item {
    id: hud
    property string armName: ""
    property int firstVisible: 0
    property int lastVisible: 0
    property int delegateCount: 0
    property real barFraction: 0
    property real trueFraction: 0
    property string selSummary: ""

    property var samples: []
    property real p50: 0
    property real p99: 0
    property real worst: 0
    property real best: 0
    property real fps: 0
    property string benchResult: "—"

    function reset() { samples = []; p50 = 0; p99 = 0; worst = 0; best = 0; fps = 0 }

    // Snapshot stats from the samples collected DURING a bench sweep, before any
    // idle (throttled-to-60Hz) frames creep in. Logged to stdout so it can be
    // read from /tmp/spike.log without defocusing the window.
    function snapshot(label) {
        if (samples.length === 0) { console.log("BENCH " + label + ": no samples"); return }
        var s = samples.slice().sort(function(a, b) { return a - b })
        var bp50 = s[Math.floor(s.length * 0.50)]
        var bp99 = s[Math.min(s.length - 1, Math.floor(s.length * 0.99))]
        benchResult = label + " — p50 " + bp50.toFixed(1) + " / p99 " + bp99.toFixed(1)
                    + " / best " + s[0].toFixed(1) + " / worst " + s[s.length - 1].toFixed(1)
                    + "ms (n=" + s.length + ")"
        console.log("BENCH " + benchResult)
    }

    width: panel.width
    height: panel.height

    FrameAnimation {
        running: true
        onTriggered: {
            var s = hud.samples
            s.push(frameTime * 1000)
            if (s.length > 720) s.shift()
            hud.samples = s
        }
    }

    Timer {
        interval: 250; running: true; repeat: true
        onTriggered: {
            if (hud.samples.length === 0) return
            var s = hud.samples.slice().sort(function(a, b) { return a - b })
            hud.best = s[0]
            hud.p50 = s[Math.floor(s.length * 0.50)]
            hud.p99 = s[Math.min(s.length - 1, Math.floor(s.length * 0.99))]
            hud.worst = s[s.length - 1]
            hud.fps = hud.p50 > 0 ? 1000 / hud.p50 : 0
        }
    }

    function band(ms) {                       // 120Hz green / 60Hz amber / red
        return ms <= 8.3 ? "#5ad15a" : (ms <= 16.6 ? "#e0c040" : "#e85050")
    }
    readonly property real driftPct: Math.abs(barFraction - trueFraction) * 100

    Rectangle {
        id: panel
        width: 330
        height: col.implicitHeight + 18
        color: "#d0101018"
        border.color: "#40ffffff"
        radius: 6

        Column {
            id: col
            x: 11; y: 9; spacing: 2

            Text { text: "ARM:  " + hud.armName; color: "white"; font { pixelSize: 13; bold: true } }
            Row {
                spacing: 6
                Text { text: "frame p50 " + hud.p50.toFixed(1) + "ms"; color: hud.band(hud.p50); font.pixelSize: 12 }
                Text { text: "p99 " + hud.p99.toFixed(1) + "ms"; color: hud.band(hud.p99); font { pixelSize: 12; bold: true } }
            }
            Text { text: "worst " + hud.worst.toFixed(1) + "ms   (" + hud.fps.toFixed(0) + " fps eq.)"
                   color: hud.band(hud.worst); font.pixelSize: 12 }
            Text { text: "best  " + hud.best.toFixed(1) + "ms   "
                          + (hud.best <= 9.5 ? "→ hit 120Hz" : "→ capped ~60Hz")
                   color: hud.band(hud.best); font { pixelSize: 12; bold: true } }
            Rectangle { width: 308; height: 1; color: "#30ffffff" }
            Text { text: "blocks: " + blockModel.count + "   total: " + (blockModel.totalHeight / 1000).toFixed(0) + "k px"
                   color: "#c8c8d0"; font.pixelSize: 12 }
            Text { text: "visible rows: " + hud.firstVisible + "–" + hud.lastVisible
                          + "   live delegates: " + hud.delegateCount
                   color: "#c8c8d0"; font.pixelSize: 12 }
            Text { text: "scrollbar drift: " + hud.driftPct.toFixed(2) + "%  (≤0.50 PASS)"
                   color: hud.driftPct <= 0.5 ? "#5ad15a" : "#e85050"; font.pixelSize: 12 }
            Rectangle { width: 308; height: 1; color: "#30ffffff" }
            Text { text: "bench: " + hud.benchResult; color: "#9cff9c"; font.pixelSize: 12; width: 308; wrapMode: Text.Wrap }
            Text { text: hud.selSummary; color: "#8ad0ff"; font { pixelSize: 12; bold: true } }
            Text { text: "revision: layout " + blockModel.layoutRevision + " / content " + blockModel.contentRevision
                   color: "#8088a0"; font.pixelSize: 11 }
        }
    }
}
