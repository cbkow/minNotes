import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: win
    visible: true
    width: 960
    height: 760
    title: "minNotes"
    color: Theme.colors.bg

    // Thin left action rail + (editor page over a bottom status strip) + a
    // collapsible right inspector that slides in/out (default hidden → "min" by
    // default). The left rail's palette button toggles the inspector.
    Row {
        anchors.fill: parent
        LeftRail { id: rail; height: parent.height; editor: editor; inspector: inspector }
        Column {
            width: parent.width - rail.width - inspector.width; height: parent.height
            // The validated passive-surface editor (model owns the cursor; blocks
            // are passive; overlay-drawn caret/selection). Ported from spike Arm C.
            Editor { id: editor; width: parent.width; height: parent.height - tabs.height - bottom.height }
            TableTabs { id: tabs; width: parent.width; editor: editor }
            BottomRail { id: bottom; width: parent.width; editor: editor }
        }
        Inspector { id: inspector; height: parent.height; editor: editor }
    }
}
