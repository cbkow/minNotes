// A context menu's section header (a table menu's Block / Column / Row, a run's "N blocks").
import QtQuick

Text {
    width: parent ? parent.width : 200
    elide: Text.ElideRight
    color: Theme.colors.textMuted
    font.family: Theme.font.family; font.pixelSize: 11; font.bold: true
    leftPadding: 12; topPadding: 6; bottomPadding: 4
}
