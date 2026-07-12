// FlatSlider — slim flat slider, ported from ufb/QCView: a thin track in
// border, a bright fill from the origin to the handle, and a small SQUARED
// "box" playhead handle (ufb's look). Instant color swaps, no animation. The
// fill color is exposed so a scrub bar can be accent-tinted while a volume bar
// stays muted; the box stays bright so the playhead reads on any fill.
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
        radius: 0
        color:  Theme.colors.border
        Rectangle {
            width:  root.visualPosition * parent.width
            height: parent.height
            radius: 0
            // Bright fill while enabled; dims to subtle when disabled
            // (e.g. the volume slider with no audio track).
            color:  root.enabled ? root.fillColor : Theme.colors.textSubtle
        }
    }
    handle: Rectangle {
        x: root.leftPadding + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: 10; height: 10
        radius: 0                  // hard-square playhead (no rounding)
        color: !root.enabled ? Theme.colors.textSubtle
                             : (root.pressed ? Theme.colors.text : Theme.colors.textBright)
    }
    opacity: root.enabled ? 1.0 : 0.55
}
