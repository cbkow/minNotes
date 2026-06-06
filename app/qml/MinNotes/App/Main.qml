import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: win
    visible: true
    width: 960
    height: 760
    title: "minNotes"
    color: Theme.colors.bg

    // Left action rail + (editor page over a bottom status strip).
    Row {
        anchors.fill: parent
        LeftRail { id: rail; height: parent.height; editor: editor }
        Column {
            width: parent.width - rail.width; height: parent.height
            // The validated passive-surface editor (model owns the cursor; blocks
            // are passive; overlay-drawn caret/selection). Ported from spike Arm C.
            Editor { id: editor; width: parent.width; height: parent.height - bottom.height }
            BottomRail { id: bottom; width: parent.width; editor: editor }
        }
    }
}
