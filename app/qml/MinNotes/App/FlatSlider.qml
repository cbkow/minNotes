// FlatSlider — slim flat slider, ported from ufb/QCView: a thin track in
// border, a bright fill from the origin to the handle, and a small round
// handle. Instant color swaps, no animation. The fill color is exposed so a
// scrub bar can be accent-tinted while a volume bar stays muted.
//
// Usage:
//   FlatSlider { from: 0; to: 1; value: 0.5; onMoved: ... }
//   FlatSlider { fillColor: Theme.colors.accent; ... }   // scrub
import QtQuick
import QtQuick.Controls.Basic

Slider {
    id: root

    // Fill (and handle) color when enabled. Default = the app accent.
    property color fillColor: Theme.colors.accent

    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width:  root.availableWidth
        height: 4
        radius: 2
        color:  Theme.colors.border
        Rectangle {
            width:  root.visualPosition * parent.width
            height: parent.height
            radius: 2
            // Bright fill while enabled; dims to subtle when disabled
            // (e.g. the volume slider with no audio track).
            color:  root.enabled ? root.fillColor : Theme.colors.textSubtle
        }
    }
    handle: Rectangle {
        x: root.leftPadding + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: 13; height: 13; radius: 6.5
        color: !root.enabled ? Theme.colors.textSubtle
                             : (root.pressed ? Theme.colors.textBright : root.fillColor)
    }
    opacity: root.enabled ? 1.0 : 0.55
}
