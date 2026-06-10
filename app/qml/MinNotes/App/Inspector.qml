import QtQuick
import QtQuick.Controls
import QtCore

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
    color: Theme.colors.surfaceRaised                  // a step lighter than the page, so the panel reads as its own layer
    clip: true                                         // so content clips cleanly while sliding

    // Switching target retargets the picker — deselect first, or the reassign
    // would write the OTHER target's colour into the selected swatch.
    onTargetChanged: { selPreset = -1; selUser = -1
                       picker.value = (target === "fg" ? fgColor : bgColor) }
    function revertTarget() {                           // reset the active colour to its default
        selPreset = -1; selUser = -1                    // (don't drag a swatch back to default)
        if (target === "fg") fgColor = Theme.colors.textBright
        else                 bgColor = "#7a6a36"
        picker.value = (target === "fg" ? fgColor : bgColor)
    }

    // --- Swatches: a pastel preset grid + a row of user slots. Clicking one
    // selects it and loads its colour; while selected, picker edits WRITE BACK
    // into the swatch (so a tweaked preset stays tweaked). Persisted app-wide
    // via Settings (JSON arrays of hex). Empty user slots capture the current
    // colour on first click.
    readonly property var defaultPresets: [
        "#e8aaaa", "#e8c5a0", "#e9dfa7", "#cfe3a6", "#aedfb2", "#a8dfc9", "#a9d4e8", "#a9bce8",
        "#bcaae8", "#d6aae8", "#e8aad7", "#e8aabb", "#d8c9b4", "#c6cdd6", "#b7c8bb", "#e3d6c2"]
    property var presets: []
    property var userSlots: ["", "", "", "", "", "", "", ""]
    property int selPreset: -1
    property int selUser: -1
    Settings {
        id: swatchStore
        category: "swatches"
        property string presets: ""
        property string user: ""
    }
    Component.onCompleted: {
        try { var p = JSON.parse(swatchStore.presets); if (p && p.length === defaultPresets.length) presets = p } catch (e) {}
        if (presets.length !== defaultPresets.length) presets = defaultPresets.slice()
        try { var u = JSON.parse(swatchStore.user); if (u && u.length === userSlots.length) userSlots = u } catch (e) {}
    }
    function saveSwatches() {
        swatchStore.presets = JSON.stringify(presets)
        swatchStore.user = JSON.stringify(userSlots)
    }
    function noteEdit(c) {                              // picker moved → selected swatch follows
        var hex = "" + c
        if (selPreset >= 0)   { var p = presets.slice();   p[selPreset] = hex; presets = p;   saveSwatches() }
        else if (selUser >= 0) { var u = userSlots.slice(); u[selUser] = hex;   userSlots = u; saveSwatches() }
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
        border.color: panel.target === t ? Theme.colors.textBright : Theme.colors.border
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
                onValueChanged: {
                    if (panel.target === "fg") panel.fgColor = value; else panel.bgColor = value
                    panel.noteEdit(value)               // selected swatch tracks the edit
                }
                Component.onCompleted: value = panel.fgColor
            }

            // (Apply lives on the left rail's bottom swatches — pick here, apply there.)

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

            // --- Preset + user swatches (see the property block up top) ---
            Text { text: "Presets"; color: Theme.colors.textMuted; topPadding: 6
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
            Grid {
                columns: 8; spacing: 4
                Repeater {
                    model: panel.presets.length
                    delegate: Swatch { required property int index; idx: index }
                }
            }
            Text { text: "Yours"; color: Theme.colors.textMuted; topPadding: 2
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
            Row {
                spacing: 4
                Repeater {
                    model: panel.userSlots.length
                    delegate: Swatch { required property int index; idx: index; userSlot: true }
                }
            }
        }
    }

    // One swatch. Selected = white border (the family selection language);
    // empty user slots show a faint + and capture the current colour on click.
    component Swatch: Rectangle {
        property int idx: -1
        property bool userSlot: false
        readonly property string hex: userSlot ? panel.userSlots[idx] : panel.presets[idx]
        readonly property bool selected: userSlot ? panel.selUser === idx : panel.selPreset === idx
        width: 18; height: 18
        color: hex !== "" ? hex : "transparent"
        border.width: 1
        border.color: selected ? Theme.colors.textBright : Theme.colors.border
        Text {
            visible: parent.hex === ""
            anchors.centerIn: parent
            text: "+"; color: Theme.colors.textSubtle
            font.family: Theme.font.family; font.pixelSize: 11
        }
        MouseArea {
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
            onClicked: {
                if (parent.userSlot) {
                    panel.selPreset = -1; panel.selUser = parent.idx
                    if (parent.hex === "") {            // empty slot captures the current colour
                        var cur = "" + (panel.target === "fg" ? panel.fgColor : panel.bgColor)
                        var u = panel.userSlots.slice(); u[parent.idx] = cur
                        panel.userSlots = u; panel.saveSwatches()
                    } else picker.value = parent.hex
                } else {
                    panel.selUser = -1; panel.selPreset = parent.idx
                    picker.value = parent.hex
                }
            }
        }
    }
}
