import QtQuick

// Right-side inspector rail. Collapsed: a column of tools (text-colour, highlight,
// palette). The two colour tools APPLY their current colour to the selection on
// click; the palette button expands the rail to reveal the colour picker (with a
// Text/Highlight target toggle) for CHANGING those colours. Apply and change are
// separate affordances — picking a colour never applies on its own.
Rectangle {
    id: rail
    property var editor: null
    property color fgColor: Theme.colors.textBright   // default text colour (white)
    property color bgColor: "#7a6a36"                 // default highlight colour (muted gold)
    property bool pickerOpen: false
    property string target: "fg"                       // which colour the picker edits

    readonly property int collapsedW: 46
    readonly property int expandedW: 237
    width: pickerOpen ? expandedW : collapsedW
    Behavior on width { NumberAnimation { duration: 140; easing.type: Easing.OutQuad } }
    color: Theme.colors.surface
    Rectangle { width: 1; height: parent.height; color: Theme.colors.border }   // left divider

    onTargetChanged: picker.value = (target === "fg" ? fgColor : bgColor)

    // A tool button matching the left rail's flat-button chassis (full width,
    // squared), with an optional colour underbar showing the tool's current colour.
    component RailTool: FlatButton {
        property color underColor: "transparent"
        property bool showUnder: false
        width: rail.collapsedW
        implicitHeight: Theme.dim.toolStripHeight
        radius: 0                              // squared, family flat style
        iconSize: Theme.icon.sizeToolbar
        tooltipSide: "left"
        Rectangle {
            visible: parent.showUnder
            anchors.bottom: parent.bottom; anchors.bottomMargin: 3
            anchors.horizontalCenter: parent.horizontalCenter
            width: 18; height: 3; radius: 1.5; color: parent.underColor
        }
    }

    function revertTarget() {                  // reset the active colour to its default
        if (target === "fg") fgColor = Theme.colors.textBright
        else                 bgColor = Theme.colors.accent
        picker.value = (target === "fg" ? fgColor : bgColor)
    }

    // Target toggle tab (Text / Highlight) with a colour chip.
    component Tab: Rectangle {
        property string label: ""
        property string t: ""
        width: 87; height: 26
        color: rail.target === t ? Theme.colors.bg : (tma.containsMouse ? Theme.colors.surfaceHover : "transparent")
        border.width: 1
        border.color: rail.target === t ? Theme.colors.textMuted : Theme.colors.border
        Row {
            anchors.centerIn: parent; spacing: 6
            Rectangle { width: 12; height: 12; radius: 2; anchors.verticalCenter: parent.verticalCenter
                        color: parent.parent.t === "fg" ? rail.fgColor : rail.bgColor
                        border.width: 1; border.color: Theme.colors.border }
            Text { text: parent.parent.label; anchors.verticalCenter: parent.verticalCenter
                   color: rail.target === parent.parent.t ? Theme.colors.textBright : Theme.colors.textMuted
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
        }
        MouseArea { id: tma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: rail.target = parent.t }
    }

    Column {
        id: tools
        width: rail.collapsedW; spacing: 0
        RailTool { text: "A"; boldLabel: true; showUnder: true; underColor: rail.fgColor; tooltip: "Text color"
                   onClicked: if (rail.editor) rail.editor.applyTextColor(rail.fgColor) }
        RailTool { iconName: "highlighter"; showUnder: true; underColor: rail.bgColor; tooltip: "Highlight"
                   onClicked: if (rail.editor) rail.editor.applyHighlight(rail.bgColor) }
        RailTool { iconName: "palette"; checked: rail.pickerOpen; tooltip: "Colors"
                   onClicked: rail.pickerOpen = !rail.pickerOpen }
    }

    Rectangle {   // divider between the tool column and the open picker
        visible: rail.pickerOpen
        x: rail.collapsedW; width: 1; height: parent.height
        color: Theme.colors.border
    }

    Column {
        visible: rail.pickerOpen
        x: rail.collapsedW + 8; y: 10; spacing: 8
        Row {
            spacing: 0
            Tab { label: "Text"; t: "fg" }
            Tab { label: "Highlight"; t: "bg" }
        }
        ColorPickerInline {
            id: picker
            onValueChanged: { if (rail.target === "fg") rail.fgColor = value; else rail.bgColor = value }
            Component.onCompleted: value = rail.fgColor
        }
        Rectangle {   // revert: soft grey (brighter than the page, not loud)
            width: 174; height: 28
            color: revertMA.containsMouse ? "#3a3a3a" : "#2e2e2e"
            Row {
                anchors.centerIn: parent; spacing: 6
                Icon { name: "arrow-counter-clockwise"; size: 14; color: Theme.colors.text
                       anchors.verticalCenter: parent.verticalCenter }
                Text { text: "Revert to default"; color: Theme.colors.text
                       font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                       anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea { id: revertMA; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor; onClicked: rail.revertTarget() }
        }
    }
}
