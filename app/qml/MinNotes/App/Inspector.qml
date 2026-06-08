import QtQuick
import QtQuick.Controls

// The right-side inspector — a collapsible panel that SLIDES in/out (default
// hidden, so the resting view is just the thin left rail + document). The left
// rail's palette button toggles `open`; the editor column reflows to make room.
// It stays open while you select text and apply, never click-away-dismissed.
//
// Today it holds the colour tools (text colour + highlight, an HSV picker with a
// Text/Highlight target toggle, revert). It's built as a real panel — not a
// floating popout — so it has room to grow into a full interface (annotation
// tools, etc.) without obscuring the document.
Rectangle {
    id: panel
    property var editor: null
    property bool open: false

    // Colour state (the I/O the editor's apply functions read).
    property color fgColor: Theme.colors.textBright   // text colour (white)
    property color bgColor: "#7a6a36"                 // highlight colour (muted gold)
    property string target: "fg"                       // which colour the picker edits

    readonly property int panelW: 198
    width: open ? panelW : 0
    Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    color: Theme.colors.surface
    clip: true                                         // so content clips cleanly while sliding

    onTargetChanged: picker.value = (target === "fg" ? fgColor : bgColor)
    function revertTarget() {                           // reset the active colour to its default
        if (target === "fg") fgColor = Theme.colors.textBright
        else                 bgColor = "#7a6a36"
        picker.value = (target === "fg" ? fgColor : bgColor)
    }
    // Apply the active target's colour to the selection (text colour or highlight).
    function applyActive() {
        if (!editor) return
        if (target === "fg") editor.applyTextColor(fgColor)
        else                 editor.applyHighlight(bgColor)
    }

    // Left hairline against the document (only meaningful while open).
    Rectangle { width: 1; height: parent.height; color: Theme.colors.border }

    // Target toggle tab (Text / Highlight) with a colour chip; selecting one points
    // the picker AND the Apply button at that colour.
    component Tab: Rectangle {
        property string label: ""
        property string t: ""
        width: 87; height: 26
        color: panel.target === t ? Theme.colors.bg : (tma.containsMouse ? Theme.colors.surfaceHover : "transparent")
        border.width: 1
        border.color: panel.target === t ? Theme.colors.accent : Theme.colors.border
        Row {
            anchors.centerIn: parent; spacing: 6
            Rectangle { width: 12; height: 12; radius: 2; anchors.verticalCenter: parent.verticalCenter
                        color: parent.parent.t === "fg" ? panel.fgColor : panel.bgColor
                        border.width: 1; border.color: Theme.colors.border }
            Text { text: parent.parent.label; anchors.verticalCenter: parent.verticalCenter
                   color: panel.target === parent.parent.t ? Theme.colors.textBright : Theme.colors.textMuted
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
        }
        MouseArea { id: tma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: panel.target = parent.t }
    }

    // Fixed-width content anchored to the right edge so it doesn't squish during the
    // slide; the panel's animating width just reveals/hides it.
    Item {
        width: panel.panelW
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }

        // Header: title + close (so you can dismiss without reaching the left rail).
        Item {
            id: header
            x: 12; width: parent.width - 24; height: 30; y: 6
            Text { anchors.verticalCenter: parent.verticalCenter
                   text: "Colors"; color: Theme.colors.textBright
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody; font.bold: true }
            FlatButton {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: 24; height: 24; radius: 0; iconName: "x"; iconSize: 13
                tooltip: "Close"; onClicked: panel.open = false
            }
        }

        Column {
            anchors.top: header.bottom; anchors.topMargin: 6
            x: 12; spacing: 8

            Row {
                spacing: 0
                Tab { label: "Text"; t: "fg" }
                Tab { label: "Highlight"; t: "bg" }
            }

            ColorPickerInline {
                id: picker
                onValueChanged: { if (panel.target === "fg") panel.fgColor = value; else panel.bgColor = value }
                Component.onCompleted: value = panel.fgColor
            }

            // Apply the active colour to the selection.
            Rectangle {
                width: 174; height: 30; radius: 0
                color: applyMA.containsMouse ? Qt.lighter(Theme.colors.accent, 1.1) : Theme.colors.accent
                Row {
                    anchors.centerIn: parent; spacing: 6
                    Rectangle { width: 12; height: 12; radius: 2; anchors.verticalCenter: parent.verticalCenter
                                color: panel.target === "fg" ? panel.fgColor : panel.bgColor
                                border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.4) }
                    Text { text: panel.target === "fg" ? "Apply text color" : "Apply highlight"
                           color: Theme.colors.textBright; anchors.verticalCenter: parent.verticalCenter
                           font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
                }
                MouseArea { id: applyMA; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor; onClicked: panel.applyActive() }
            }

            // Revert: soft grey (brighter than the page, not loud).
            Rectangle {
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
                            cursorShape: Qt.PointingHandCursor; onClicked: panel.revertTarget() }
            }
        }
    }
}
