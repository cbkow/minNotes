import QtQuick

// Slim HSV colour picker: a saturation×value square + a vertical hue strip + a hex
// field. `value` (color) is the I/O: set it imperatively to reposition the handles,
// and read changes via onValueChanged. No alpha. Ported/slimmed from QCView's
// ColorPickerInline (we drop the R/G/B fields — hex covers manual entry).
Item {
    id: root
    property color value: "#ffffff"
    implicitWidth: body.implicitWidth
    implicitHeight: body.implicitHeight

    // HSV ground truth (0..1). Kept so hue survives a saturation→0 (grayscale) pass.
    property real _h: 0
    property real _s: 0
    property real _v: 1
    property bool _suppress: false   // guard: internal sets shouldn't re-derive HSV

    function _setHsv(h, s, v) {
        _h = h; _s = s; _v = v
        _suppress = true
        value = Qt.hsva(h, s, v, 1)
        _suppress = false
        _updateFields()
    }
    onValueChanged: if (!_suppress) _applyValue()
    function _applyValue() {            // external set → re-derive HSV + fields
        var hh = value.hsvHue
        if (hh >= 0) _h = hh            // -1 for grayscale → keep the prior hue
        _s = value.hsvSaturation
        _v = value.hsvValue
        _updateFields()
    }
    function _updateFields() {          // mirror the colour into the hex + RGB fields
        hexField.text = root._hex()
        rCh.field.text = Math.round(value.r * 255)
        gCh.field.text = Math.round(value.g * 255)
        bCh.field.text = Math.round(value.b * 255)
    }
    function _applyRgb() {              // RGB fields → colour
        function c(x) { return Math.max(0, Math.min(255, parseInt(x) || 0)) }
        value = Qt.rgba(c(rCh.field.text) / 255, c(gCh.field.text) / 255, c(bCh.field.text) / 255, 1)
    }
    function _hex() {
        function h2(x) { var s = Math.round(x * 255).toString(16); return s.length < 2 ? "0" + s : s }
        return (h2(value.r) + h2(value.g) + h2(value.b)).toUpperCase()
    }
    Component.onCompleted: _applyValue()

    Column {
        id: body
        spacing: 8
        Row {
            spacing: 8
            Rectangle {   // saturation × value square
                id: sv
                width: 188; height: 168
                border.width: 1; border.color: Theme.colors.border
                Rectangle {   // white → hue (horizontal)
                    anchors.fill: parent; anchors.margins: 1
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: "#ffffff" }
                        GradientStop { position: 1; color: Qt.hsva(root._h, 1, 1, 1) }
                    }
                }
                Rectangle {   // transparent → black (vertical value)
                    anchors.fill: parent; anchors.margins: 1
                    gradient: Gradient {
                        GradientStop { position: 0; color: "transparent" }
                        GradientStop { position: 1; color: "#000000" }
                    }
                }
                Rectangle {   // handle
                    width: 12; height: 12; radius: 6; color: "transparent"
                    border.width: 2; border.color: "white"
                    x: root._s * (sv.width - 2) - 5
                    y: (1 - root._v) * (sv.height - 2) - 5
                }
                MouseArea {
                    anchors.fill: parent
                    function upd(m) {
                        root._setHsv(root._h,
                            Math.max(0, Math.min(1, m.x / (sv.width - 2))),
                            1 - Math.max(0, Math.min(1, m.y / (sv.height - 2))))
                    }
                    onPressed: (m) => upd(m)
                    onPositionChanged: (m) => { if (pressed) upd(m) }
                }
            }
            Rectangle {   // hue strip
                id: hue
                width: 18; height: sv.height
                border.width: 1; border.color: Theme.colors.border
                Rectangle {
                    anchors.fill: parent; anchors.margins: 1
                    gradient: Gradient {
                        GradientStop { position: 0.000; color: "#ff0000" }
                        GradientStop { position: 0.167; color: "#ffff00" }
                        GradientStop { position: 0.333; color: "#00ff00" }
                        GradientStop { position: 0.500; color: "#00ffff" }
                        GradientStop { position: 0.667; color: "#0000ff" }
                        GradientStop { position: 0.833; color: "#ff00ff" }
                        GradientStop { position: 1.000; color: "#ff0000" }
                    }
                }
                Rectangle {   // hue indicator bar
                    width: parent.width + 4; height: 3; x: -2
                    y: root._h * (hue.height - 1) - 1
                    color: "white"; border.width: 1; border.color: "#000000"
                }
                MouseArea {
                    anchors.fill: parent
                    function upd(m) { root._setHsv(Math.max(0, Math.min(1, m.y / (hue.height - 1))), root._s, root._v) }
                    onPressed: (m) => upd(m)
                    onPositionChanged: (m) => { if (pressed) upd(m) }
                }
            }
        }
        Rectangle {   // hex field
            width: 214; height: 28; radius: 0
            color: Theme.colors.codeBg; border.width: 1; border.color: Theme.colors.border
            Text {
                anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; anchors.leftMargin: 8
                text: "#"; color: Theme.colors.textMuted; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeBody
            }
            TextInput {
                id: hexField
                anchors.fill: parent; anchors.leftMargin: 20; anchors.rightMargin: 8
                verticalAlignment: TextInput.AlignVCenter; clip: true; selectByMouse: true
                color: Theme.colors.text; selectionColor: Theme.colors.selectionBg
                font.family: Theme.font.mono; font.pixelSize: 12
                maximumLength: 6
                onAccepted: {
                    var t = text.replace(/[^0-9a-fA-F]/g, "")
                    if (t.length === 3) t = t[0]+t[0]+t[1]+t[1]+t[2]+t[2]
                    if (t.length === 6) { root.value = "#" + t; hexField.text = root._hex() }
                }
            }
        }
        Row {   // R / G / B channels (0–255)
            spacing: 8
            component Channel: Rectangle {
                property string lab: ""
                property alias field: chInput
                width: 66; height: 26; radius: 0
                color: Theme.colors.codeBg; border.width: 1; border.color: Theme.colors.border
                Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; anchors.leftMargin: 6
                       text: parent.lab; color: Theme.colors.textMuted
                       font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall }
                TextInput {
                    id: chInput
                    anchors.fill: parent; anchors.leftMargin: 18; anchors.rightMargin: 6
                    verticalAlignment: TextInput.AlignVCenter; clip: true; selectByMouse: true
                    color: Theme.colors.text; selectionColor: Theme.colors.selectionBg
                    font.family: Theme.font.mono; font.pixelSize: 12
                    maximumLength: 3; inputMethodHints: Qt.ImhDigitsOnly
                    onAccepted: root._applyRgb()
                }
            }
            Channel { id: rCh; lab: "R" }
            Channel { id: gCh; lab: "G" }
            Channel { id: bCh; lab: "B" }
        }
    }
}
