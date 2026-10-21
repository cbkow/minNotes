// The PEN WIDTH flyout (walk 7d, 2026-10-18): its own button in the toolbar — a width is not a colour. The slider
// spends most of its travel on 1–20 px (the range that matters) and the last stretch on 20–64. Dragging it re-styles
// any SELECTED ink elements live (one history row per drag, through the view's batch). Modal like the other flyouts:
// a press outside closes it and goes no further; no focus of its own.
import QtQuick
import QtQuick.Controls

Popup {
    id: flyout
    required property var inspector
    required property var editor
    readonly property var canvasItem: editor ? editor.canvasItem : null
    readonly property var sliderItem: slider   // probe
    padding: 12
    x: parent ? parent.width - width : 0
    y: parent ? parent.height + 4 : 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    modal: true; dim: false
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.dim.popupMs } }
    background: Rectangle { color: Theme.colors.surfaceRaised; radius: Theme.dim.radius; border.width: 1; border.color: Theme.colors.border }
    onClosed: if (editor) editor.forceActiveFocus()

    // The travel: 70 % of the slider covers 1–20, the rest 20–64.
    readonly property real knee: 0.7
    function widthAt(p) { return Math.round(p <= knee ? 1 + (p / knee) * 19 : 20 + ((p - knee) / (1 - knee)) * 44) }
    function posOf(w) { return w <= 20 ? ((w - 1) / 19) * knee : knee + ((w - 20) / 44) * (1 - knee) }
    readonly property real width_: inspector ? inspector.drawWidth : 6

    contentItem: Column {
        spacing: 10
        Row {
            spacing: 8
            Text { text: qsTr("Pen width"); anchors.verticalCenter: parent.verticalCenter
                   color: Theme.colors.textMuted; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
            Item { width: 150 - 8 - 54; height: 1 }
            Text { text: Math.round(flyout.width_) + " px"; anchors.verticalCenter: parent.verticalCenter; width: 54; horizontalAlignment: Text.AlignRight
                   color: Theme.colors.textSubtle; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall }
        }
        FlatSlider {
            id: slider
            width: 214; height: 24
            from: 0; to: 1
            value: flyout.posOf(flyout.width_)
            fillColor: Theme.colors.textMuted
            onPressedChanged: {   // one history row per drag for the selected ink
                if (!flyout.canvasItem) return
                if (pressed) flyout.canvasItem.beginPenStyle(qsTr("Pen width")); else flyout.canvasItem.endPenStyle()
            }
            onMoved: {
                const w = flyout.widthAt(value)
                if (flyout.inspector && flyout.inspector.drawWidth !== w) {
                    flyout.inspector.drawWidth = w
                    if (flyout.canvasItem) flyout.canvasItem.applyPenWidth(w)   // selected strokes follow
                }
            }
            Rectangle {   // the knee: 20 px
                x: parent.leftPadding + parent.availableWidth * flyout.knee - 0.5; y: parent.height / 2 - 5
                width: 1; height: 10; color: Theme.colors.textSubtle
            }
        }
        Item {   // a preview of the stroke
            width: 214; height: 28
            Rectangle {
                anchors.centerIn: parent
                width: 160; height: Math.max(1, Math.min(28, Math.round(flyout.width_))); radius: 0
                color: flyout.inspector ? flyout.inspector.drawColor : Theme.colors.text
            }
        }
    }
}
