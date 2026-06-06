import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: win
    visible: true
    width: 960
    height: 760
    title: "minNotes"
    color: Theme.colors.bg

    // Left action rail + the editor page. The rail is a fixed-width vertical
    // strip (grows collapsible sections later); the editor fills the rest.
    Row {
        anchors.fill: parent
        LeftRail { id: rail; height: parent.height; editor: editor }
        // The validated passive-surface editor (model owns the cursor; blocks are
        // passive; overlay-drawn caret/selection). Ported from the spike's Arm C.
        Editor { id: editor; width: parent.width - rail.width; height: parent.height }
    }
}
