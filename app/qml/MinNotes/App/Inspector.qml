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
    property color bgColor: "#FFEC59"                 // highlight colour (classic bright yellow)
    property string target: "fg"                       // which colour the picker edits

    // Drawing state (the video studio's annotator reads these; QSettings-
    // backed like QCView's annotation/colorHex). drawTool "" = disarmed.
    property color drawColor: "#FF0000"                // QCView's default stroke red
    property string drawTool: ""                       // freehand|rect|oval|arrow|line|eraser
    property real drawWidth: 6                         // source-pixel stroke width (1..24)
    Settings {
        id: drawStore
        category: "drawing"
        property string colorHex: ""
        property real width: 0
    }
    onDrawColorChanged: drawStore.colorHex = "" + drawColor
    onDrawWidthChanged: drawStore.width = drawWidth

    readonly property int panelW: 198
    width: open ? panelW : 0
    Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    color: Theme.colors.surfaceRaised                  // a step lighter than the page, so the panel reads as its own layer
    clip: true                                         // so content clips cleanly while sliding

    function targetColor() {
        return target === "fg" ? fgColor : target === "bg" ? bgColor : drawColor
    }
    // Switching target retargets the picker — deselect first, or the reassign
    // would write the OTHER target's colour into the selected swatch.
    onTargetChanged: { selPreset = -1; selUser = -1
                       picker.value = targetColor() }
    function revertTarget() {                           // reset the active colour to its default
        selPreset = -1; selUser = -1                    // (don't drag a swatch back to default)
        if (target === "fg")      fgColor = Theme.colors.textBright
        else if (target === "bg") bgColor = "#FFEC59"
        else                      drawColor = "#FF0000"
        picker.value = targetColor()
    }

    // --- Swatches: a FIXED bright preset grid + a row of user slots. Clicking
    // one selects it and loads its colour. Only user slots are editable: while
    // a user slot is selected, picker edits write back into it (persisted via
    // Settings); presets are immutable — editing while one is selected just
    // deselects it (the swatch no longer matches the picker). Empty user slots
    // capture the current colour on first click.
    readonly property var presets: [
        "#FF5768", "#FF6F68", "#FC6238", "#FFA23A", "#FFBF65", "#FFD872", "#FFEC59", "#CFF800",
        "#4DD091", "#00CDAC", "#8DD7BF", "#00B0BA", "#00A5E3", "#6C88C4", "#C05780", "#FF96C5"]
    property var userSlots: ["", "", "", "", "", "", "", ""]
    property int selPreset: -1
    property int selUser: -1
    Settings {
        id: swatchStore
        category: "swatches"
        property string user: ""
    }
    Component.onCompleted: {
        try { var u = JSON.parse(swatchStore.user); if (u && u.length === userSlots.length) userSlots = u } catch (e) {}
        if (drawStore.colorHex !== "") drawColor = drawStore.colorHex
        if (drawStore.width > 0) drawWidth = drawStore.width
    }
    function saveSwatches() { swatchStore.user = JSON.stringify(userSlots) }
    function noteEdit(c) {                              // picker moved
        var hex = "" + c
        if (selUser >= 0) { var u = userSlots.slice(); u[selUser] = hex; userSlots = u; saveSwatches() }
        else if (selPreset >= 0 && hex.toLowerCase() !== presets[selPreset].toLowerCase())
            selPreset = -1                              // presets are fixed; a tweak detaches
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
                        color: parent.parent.t === "fg" ? panel.fgColor
                             : parent.parent.t === "bg" ? panel.bgColor : panel.drawColor
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
                Tab { label: "Text"; t: "fg"; width: 50 }
                Tab { label: "Highlight"; t: "bg"; width: 76 }
                Tab { label: "Draw"; t: "draw"; width: 48 }
            }

            ColorPickerInline {
                id: picker
                onValueChanged: {
                    if (panel.target === "fg")      panel.fgColor = value
                    else if (panel.target === "bg") panel.bgColor = value
                    else                            panel.drawColor = value
                    panel.noteEdit(value)               // selected swatch tracks the edit
                }
                Component.onCompleted: value = panel.fgColor
            }

            // --- Drawing tools (video studio) — visible on the Draw target.
            // Arming a tool turns the studio stage into a drawing surface;
            // clicking the armed tool again (or Esc) disarms.
            Grid {
                visible: panel.target === "draw"
                columns: 6; spacing: 2
                Repeater {
                    model: [
                        { tool: "freehand", icon: "scribble",      tip: qsTr("Freehand") },
                        { tool: "rect",     icon: "rectangle",     tip: qsTr("Rectangle") },
                        { tool: "oval",     icon: "circle",        tip: qsTr("Oval") },
                        { tool: "arrow",    icon: "arrow-up-right",tip: qsTr("Arrow") },
                        { tool: "line",     icon: "line-segment",  tip: qsTr("Line") },
                        { tool: "eraser",   icon: "eraser",        tip: qsTr("Eraser") }
                    ]
                    delegate: FlatButton {
                        required property var modelData
                        width: 27; height: 27
                        iconName: modelData.icon
                        checked: panel.drawTool === modelData.tool
                        checkedColor: Theme.colors.divider   // grey — family selection language
                        iconColor: checked ? Theme.colors.textBright : Theme.colors.textMuted
                        tooltip: modelData.tip; tooltipSide: "top"
                        onClicked: panel.drawTool =
                            (panel.drawTool === modelData.tool) ? "" : modelData.tool
                    }
                }
            }
            Row {
                visible: panel.target === "draw"
                spacing: 8
                Text {
                    text: "Width"
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.colors.textMuted
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                }
                FlatSlider {
                    width: 110
                    anchors.verticalCenter: parent.verticalCenter
                    from: 1; to: 24
                    value: panel.drawWidth
                    fillColor: Theme.colors.textMuted
                    onMoved: panel.drawWidth = value
                }
                Text {
                    text: Math.round(panel.drawWidth)
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.colors.textSubtle
                    font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
                }
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
            acceptedButtons: parent.userSlot ? (Qt.LeftButton | Qt.RightButton) : Qt.LeftButton
            onClicked: (m) => {
                if (parent.userSlot && m.button === Qt.RightButton) {
                    if (parent.hex === "") return       // nothing to clear
                    var p = mapToItem(panel, m.x, m.y)
                    slotMenu.idx = parent.idx
                    slotMenu.x = Math.max(8, Math.min(p.x, panel.width - slotMenu.width - 8))
                    slotMenu.y = p.y + 4
                    slotMenu.open()
                    return
                }
                if (parent.userSlot) {
                    panel.selPreset = -1; panel.selUser = parent.idx
                    if (parent.hex === "") {            // empty slot captures the current colour
                        var cur = "" + panel.targetColor()
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

    // Right-click menu for a user slot: clear it back to empty (+).
    Popup {
        id: slotMenu
        property int idx: -1
        padding: 4
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Rectangle {
            implicitWidth: 104; implicitHeight: 26
            color: clearMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
            Text {
                anchors.verticalCenter: parent.verticalCenter; x: 8
                text: "Clear slot"; color: Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            MouseArea {
                id: clearMA
                anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                onClicked: {
                    var u = panel.userSlots.slice(); u[slotMenu.idx] = ""
                    panel.userSlots = u; panel.saveSwatches()
                    if (panel.selUser === slotMenu.idx) panel.selUser = -1
                    slotMenu.close()
                }
            }
        }
    }
}
