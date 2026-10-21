// One row of a context menu (the shared plumbing, 2026-10-21 — the block, canvas and lane menus all build on it).
// `danger` = the error colour (a destructive row; always last, after a rule), `inert` = information (dimmed, not
// clickable), `bright` = a filled row's bold label (the spell well's Fix all). Hover ramps in `hoverColor` (the
// spell well hovers in its own tone). The row fills its column's width; long labels elide.
import QtQuick

Rectangle {
    id: row
    property string text: ""
    property bool danger: false
    property bool inert: false
    property bool bright: false
    property color hoverColor: Theme.colors.surfaceHover
    readonly property bool hovered: ma.containsMouse
    readonly property bool isMenuRow: true          // the probes count rows, not rules / headers
    signal clicked()
    width: parent ? parent.width : 200; height: 26
    radius: Theme.dim.radiusSmall
    color: !inert && ma.containsMouse ? hoverColor : Qt.rgba(hoverColor.r, hoverColor.g, hoverColor.b, 0)   // ramp alpha only (the spell well's tone too)
    Behavior on color { ColorAnimation { duration: Theme.dim.hoverMs } }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left; anchors.leftMargin: 12
        anchors.right: parent.right; anchors.rightMargin: 12
        text: row.text
        elide: Text.ElideRight
        color: row.inert ? Theme.colors.textMuted : row.danger ? Theme.colors.error
             : row.bright ? Theme.colors.textBright : Theme.colors.text
        font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome; font.bold: row.bright
    }
    MouseArea {
        id: ma
        enabled: !row.inert
        anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
        onClicked: row.clicked()
    }
}
