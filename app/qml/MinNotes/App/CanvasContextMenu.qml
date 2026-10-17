// The canvas's context menu (2.0): a house-style popup (squared, one-pixel border, no outer margin)
// whose rows are FILTERED by what is selected — a page offers Open page, a text chip Edit text,
// anything but a page Duplicate, any selection the z order and Delete, empty canvas New page here.
// A row that can't apply is not greyed, it is absent.
import QtQuick
import QtQuick.Controls

Popup {
    id: menu
    required property var view
    property real worldX: 0
    property real worldY: 0
    padding: 1
    width: 180
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.colors.surface; radius: 0; border.width: 1; border.color: Theme.colors.border }

    readonly property int n: view.selectionCount
    readonly property string kind1: view.selectedId !== "" ? canvas.elementKind(view.selectedId) : ""
    readonly property bool anyNonPage: view.selectedIds.some(id => canvas.elementKind(id) !== "page")
    readonly property int visibleRows: { let c = 0; for (let i = 0; i < rows.children.length; ++i) if (rows.children[i].visible) ++c; return c }   // probe
    function popupAt(sx, sy) { x = sx; y = sy; open() }

    component Row: Rectangle {
        id: row
        property string label: ""
        property bool show: true
        signal triggered()
        visible: show
        width: rows.width; height: 28
        color: ma.containsMouse ? Theme.colors.surfaceHover : "transparent"
        Text {
            anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; anchors.leftMargin: 12
            text: row.label; color: Theme.colors.text
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
        }
        MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: { menu.close(); row.triggered() } }
    }
    contentItem: Column {
        id: rows
        width: menu.width - 2
        Row { label: qsTr("Open page");      show: menu.kind1 === "page";  onTriggered: menu.view.drillSelected() }
        Row { label: qsTr("Edit text");      show: menu.kind1 === "shape"; onTriggered: menu.view.editText(menu.view.selectedId) }
        Row { label: qsTr("Duplicate");      show: menu.anyNonPage;        onTriggered: menu.view.duplicateSelected() }
        Row { label: qsTr("Bring to front"); show: menu.n > 0;             onTriggered: menu.view.frontSelected() }
        Row { label: qsTr("Send to back");   show: menu.n > 0;             onTriggered: menu.view.backSelected() }
        Row { label: qsTr("Delete");         show: menu.n > 0;             onTriggered: menu.view.deleteSelected() }
        Row { label: qsTr("New page here");  show: menu.n === 0
              onTriggered: { const id = canvas.addPageAt(menu.worldX, menu.worldY); if (id !== "") menu.view.select(id) } }
    }
}
