import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: win
    visible: true
    width: 900
    height: 760
    title: "minNotes"

    // The validated passive-surface editor (model owns the cursor; blocks are
    // passive; overlay-drawn caret/selection). Ported from the spike's Arm C.
    Editor { anchors.fill: parent }
}
