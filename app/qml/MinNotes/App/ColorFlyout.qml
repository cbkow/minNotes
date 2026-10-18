// The colour FLYOUT (2.0 chrome re-organisation, 2026-10-18 — §2.14): the Inspector's palette, moved into a Popup
// under the toolbar's 3-stack swatch. The STATE stays on the Inspector object (fgColor / bgColor / drawColor /
// drawWidth / target + their Settings — six files read `editor.inspector.*`); this is the picker UI only: the target
// tabs, the HSV picker, Apply (pick-then-select) + live apply (select-then-pick — the gesture-order rule), Revert, the
// presets + user slots, the pen width. No focus of its own (the keys stay with the editor); the hex field takes focus
// when clicked and `onClosed` hands it back.
import QtQuick
import QtQuick.Controls
import QtCore

Popup {
    id: flyout
    required property var inspector              // the Inspector panel object (the colour state)
    readonly property var editor: inspector ? inspector.editor : null
    readonly property int contentW: 214
    readonly property var pickerItem: picker     // probe
    padding: 12
    x: parent ? parent.width - width : 0         // right-aligned to the swatch (it sits near the window's right edge)
    y: {   // under the swatch — but never past the window's bottom edge (the tallest flyout; the window may be 480 tall)
        const top = parent ? parent.mapToItem(null, 0, 0).y : 0
        const ov = flyout.Overlay.overlay ? flyout.Overlay.overlay.height : 0
        return Math.min(parent ? parent.height + 4 : 0, Math.max(4, ov - top - height - 8))
    }
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    modal: true; dim: false   // a press outside CLOSES the flyout and goes NO further — never through to the page beneath (user, 2026-10-18)
    background: Rectangle { color: Theme.colors.surfaceRaised; radius: 0; border.width: 1; border.color: Theme.colors.border }
    onClosed: if (editor) editor.forceActiveFocus()
    onAboutToShow: if (inspector) setPickerValue(inspector.targetColor())

    // Picking a colour auto-applies it to the document selection (live). But a PROGRAMMATIC picker.value set (a
    // target switch, init, revert) must NOT — so those go through setPickerValue, which suppresses the apply.
    property bool _suppressApply: false
    function setPickerValue(v) { _suppressApply = true; picker.value = v; _suppressApply = false }
    // Switching target retargets the picker — deselect first, or the reassign would write the OTHER target's colour
    // into the selected swatch.
    Connections {
        target: flyout.inspector
        function onTargetChanged() { flyout.selPreset = -1; flyout.selUser = -1; flyout.setPickerValue(flyout.inspector.targetColor()) }
    }
    // Reset the picker to its default AND strip THIS target's colour from the current selection (table cells or
    // text) — "revert to default" means uncoloured, for the text colour OR the highlight. Pen only resets the pen.
    function revertTarget() {
        if (!inspector) return
        selPreset = -1; selUser = -1                    // (don't drag a swatch back to default)
        if (inspector.target === "fg")      inspector.fgColor = Theme.colors.text
        else if (inspector.target === "bg") inspector.bgColor = "#FFEC59"
        else                                inspector.drawColor = "#FF0000"
        setPickerValue(inspector.targetColor())
        if (editor && inspector.target !== "draw") editor.revertColors(inspector.target === "fg")
    }

    // --- Swatches: a FIXED bright preset grid + a row of user slots. Clicking one selects it and loads its colour.
    // Only user slots are editable: while a user slot is selected, picker edits write back into it (persisted);
    // presets are immutable — editing while one is selected just deselects it. Empty user slots capture the current
    // colour on first click.
    readonly property var presets: [
        "#FF5768", "#FF6F68", "#FC6238", "#FFA23A", "#FFBF65", "#FFD872", "#FFEC59", "#CFF800", "#86E36B", "#43E8D8",
        "#4DD091", "#00CDAC", "#8DD7BF", "#00B0BA", "#00A5E3", "#6C88C4", "#C05780", "#FF96C5", "#9D8DF1", "#FF60A8"]
    property var userSlots: ["", "", "", "", "", "", "", "", "", ""]
    property int selPreset: -1
    property int selUser: -1
    Settings {
        id: swatchStore
        category: "swatches"
        property string user: ""
    }
    Component.onCompleted: {
        try { var u = JSON.parse(swatchStore.user)   // pad/truncate the saved slots to the current count
              if (u && u.length) { var n = userSlots.slice()
                                   for (var i = 0; i < n.length && i < u.length; i++) n[i] = u[i]
                                   userSlots = n } } catch (e) {}
    }
    function saveSwatches() { swatchStore.user = JSON.stringify(userSlots) }
    function noteEdit(c) {                              // picker moved
        var hex = "" + c
        if (selUser >= 0) { var u = userSlots.slice(); u[selUser] = hex; userSlots = u; saveSwatches() }
        else if (selPreset >= 0 && hex.toLowerCase() !== presets[selPreset].toLowerCase())
            selPreset = -1                              // presets are fixed; a tweak detaches
    }

    // Target toggle (Text / Highlight / Pen) — flat inline buttons carrying a colour chip; the active one uses the
    // family selection language: divider-grey fill + bright text.
    component Tab: Rectangle {
        property string label: ""
        property string t: ""
        readonly property bool sel: !!flyout.inspector && flyout.inspector.target === t
        width: tabRow.implicitWidth + 20; height: 28; radius: 0
        color: sel ? Theme.colors.divider : (tma.containsMouse ? Theme.colors.surfaceHover : "transparent")
        Row {
            id: tabRow
            anchors.centerIn: parent; spacing: 6
            Rectangle { width: 12; height: 12; radius: 0; anchors.verticalCenter: parent.verticalCenter
                        color: !flyout.inspector ? "transparent" : parent.parent.t === "fg" ? flyout.inspector.fgColor
                             : parent.parent.t === "bg" ? flyout.inspector.bgColor : flyout.inspector.drawColor
                        border.width: 1; border.color: Theme.colors.border }
            Text { text: parent.parent.label; anchors.verticalCenter: parent.verticalCenter
                   color: parent.parent.sel ? Theme.colors.textBright : Theme.colors.textMuted
                   font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
        }
        MouseArea { id: tma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: if (flyout.inspector) flyout.inspector.target = parent.t }
    }
    // One swatch. Selected = white border; empty user slots show a faint + and capture the current colour on click.
    component Swatch: Rectangle {
        property int idx: -1
        property bool userSlot: false
        readonly property string hex: userSlot ? flyout.userSlots[idx] : flyout.presets[idx]
        readonly property bool selected: userSlot ? flyout.selUser === idx : flyout.selPreset === idx
        width: Math.floor((flyout.contentW - 4 * 4) / 5); height: 18; radius: 0   // WIDE clips, five across (user, 2026-10-18)
        color: hex !== "" ? hex : "transparent"
        border.width: 1
        border.color: selected ? Theme.colors.textBright : Theme.colors.border
        Text { visible: parent.hex === ""; anchors.centerIn: parent; text: "+"; color: Theme.colors.textSubtle
               font.family: Theme.font.family; font.pixelSize: 11 }
        MouseArea {
            anchors.fill: parent; cursorShape: Qt.PointingHandCursor
            acceptedButtons: parent.userSlot ? (Qt.LeftButton | Qt.RightButton) : Qt.LeftButton
            onClicked: (m) => {
                if (parent.userSlot && m.button === Qt.RightButton) {
                    if (parent.hex === "") return       // nothing to clear
                    var p = mapToItem(pal, m.x, m.y)
                    slotMenu.idx = parent.idx
                    slotMenu.x = Math.max(0, Math.min(p.x, pal.width - slotMenu.width))
                    slotMenu.y = p.y + 4
                    slotMenu.open()
                    return
                }
                if (parent.userSlot) {
                    flyout.selPreset = -1; flyout.selUser = parent.idx
                    if (parent.hex === "") {            // empty slot captures the current colour
                        var cur = "" + (flyout.inspector ? flyout.inspector.targetColor() : "#ffffff")
                        var u = flyout.userSlots.slice(); u[parent.idx] = cur
                        flyout.userSlots = u; flyout.saveSwatches()
                    } else picker.value = parent.hex
                } else {
                    flyout.selUser = -1; flyout.selPreset = parent.idx
                    picker.value = parent.hex
                }
            }
        }
    }

    contentItem: Column {
        id: pal
        spacing: 8
        Row {   // Text leads now that the swatch stack chooses the target; the stack's order is text · highlight · pen
            spacing: 4
            Tab { label: qsTr("Text"); t: "fg" }
            Tab { label: qsTr("Highlight"); t: "bg" }
            Tab { label: qsTr("Pen"); t: "draw" }
        }
        ColorPickerInline {
            id: picker
            onValueChanged: {
                if (!flyout.inspector) return
                if (flyout.inspector.target === "fg")      flyout.inspector.fgColor = value
                else if (flyout.inspector.target === "bg") flyout.inspector.bgColor = value
                else                                       flyout.inspector.drawColor = value
                flyout.noteEdit(value)               // the selected swatch tracks the edit
                // Live auto-apply to the document selection (text / highlight), unless this is a programmatic set.
                // Text colour also arms the typing "pen" (pickTextColor).
                if (!flyout._suppressApply && flyout.editor) {
                    if (flyout.inspector.target === "fg")      flyout.editor.pickTextColor("" + value)
                    else if (flyout.inspector.target === "bg") flyout.editor.pickHighlight("" + value)
                }
            }
            Component.onCompleted: if (flyout.inspector) flyout.setPickerValue(flyout.inspector.targetColor())   // init: no apply/arm
        }
        // Apply: push the picked colour onto the current selection explicitly (user ruling 2026-08-21): a colour
        // picked BEFORE the selection existed needs this. Hidden for the pen (that colour feeds the armed tool).
        Rectangle {
            visible: !!flyout.inspector && flyout.inspector.target !== "draw"
            width: flyout.contentW; height: 28; radius: 0
            readonly property bool canApply: !!flyout.editor && (flyout.editor.hasSelection || flyout.editor.caretInCell)
            opacity: canApply ? 1.0 : 0.4          // disabled chrome rule
            color: applyMA.containsMouse && canApply ? Theme.colors.divider : Theme.colors.surfaceHover
            Row {
                anchors.centerIn: parent; spacing: 6
                Rectangle { width: 12; height: 12; radius: 0; anchors.verticalCenter: parent.verticalCenter
                            color: !flyout.inspector ? "transparent" : flyout.inspector.target === "fg" ? flyout.inspector.fgColor : flyout.inspector.bgColor
                            border.width: 1; border.color: Theme.colors.border }
                Text { text: qsTr("Apply to selection"); color: Theme.colors.text
                       font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                       anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea { id: applyMA; anchors.fill: parent; hoverEnabled: true
                        cursorShape: parent.canApply ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: {
                            if (!parent.canApply) return
                            if (flyout.inspector.target === "fg") flyout.editor.applyTextColor(flyout.inspector.fgColor)
                            else                                  flyout.editor.applyHighlight(flyout.inspector.bgColor)
                        } }
        }
        Rectangle {   // Revert: soft grey (brighter than the page, not loud)
            width: flyout.contentW; height: 28; radius: 0
            color: revertMA.containsMouse ? Theme.colors.divider : Theme.colors.surfaceHover
            Row {
                anchors.centerIn: parent; spacing: 6
                Icon { name: "arrow-counter-clockwise"; size: 14; color: Theme.colors.text; anchors.verticalCenter: parent.verticalCenter }
                Text { text: qsTr("Revert to default"); color: Theme.colors.text
                       font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
                       anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea { id: revertMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: flyout.revertTarget() }
        }
        Row {   // the pen width (source px, 1..64)
            spacing: 8
            Text { text: qsTr("Pen width"); anchors.verticalCenter: parent.verticalCenter
                   color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
            FlatSlider {
                width: flyout.contentW - 100
                anchors.verticalCenter: parent.verticalCenter
                from: 1; to: 64
                value: flyout.inspector ? flyout.inspector.drawWidth : 6
                fillColor: Theme.colors.textMuted
                onMoved: if (flyout.inspector) flyout.inspector.drawWidth = value
            }
            Text { text: flyout.inspector ? Math.round(flyout.inspector.drawWidth) : ""; anchors.verticalCenter: parent.verticalCenter
                   color: Theme.colors.textSubtle; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall }
        }
        Text { text: qsTr("Presets"); color: Theme.colors.textMuted; topPadding: 4
               font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
        Grid {
            columns: 5; spacing: 4
            Repeater { model: flyout.presets.length; delegate: Swatch { required property int index; idx: index } }
        }
        Text { text: qsTr("Yours"); color: Theme.colors.textMuted; topPadding: 2
               font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
        Grid {
            columns: 5; spacing: 4
            Repeater { model: flyout.userSlots.length; delegate: Swatch { required property int index; idx: index; userSlot: true } }
        }
        // Right-click menu for a user slot: clear it back to empty (+).
        Popup {
            id: slotMenu
            parent: pal
            property int idx: -1
            padding: 4
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            background: Rectangle { color: Theme.colors.surface; radius: 0; border.width: 1; border.color: Theme.colors.border }
            contentItem: Rectangle {
                implicitWidth: 104; implicitHeight: 26; radius: 0
                color: clearMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
                Text { anchors.verticalCenter: parent.verticalCenter; x: 8; text: qsTr("Clear slot"); color: Theme.colors.text
                       font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
                MouseArea {
                    id: clearMA
                    anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        var u = flyout.userSlots.slice(); u[slotMenu.idx] = ""
                        flyout.userSlots = u; flyout.saveSwatches()
                        if (flyout.selUser === slotMenu.idx) flyout.selUser = -1
                        slotMenu.close()
                    }
                }
            }
        }
    }
}
