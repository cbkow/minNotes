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
    width: 200
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.colors.surface; radius: 0; border.width: 1; border.color: Theme.colors.border }

    readonly property int n: view.selectionCount
    readonly property string kind1: view.selectedId !== "" ? canvas.elementKind(view.selectedId) : ""
    readonly property bool anyNonPage: view.selectedIds.some(id => canvas.elementKind(id) !== "page")
    // One LOCAL media element (a file on disk, or one that would be): its path actions (phase A4).
    readonly property bool localMedia: n === 1 && kind1 === "media" && canvas.mediaElementKind(view.selectedId) !== "bookmark"
                                       && !/^https?:/.test(canvas.elementLocalPath(view.selectedId) === "" ? "https:" : "")
    readonly property bool onMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
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
        Row { label: menu.n === 1 ? menu.view.openLabel(menu.view.selectedId) : ""
              show: menu.n === 1 && menu.view.canOpen(menu.view.selectedId); onTriggered: menu.view.openSelected() }
        Row { label: menu.onMac ? qsTr("Reveal in Finder") : qsTr("Show in Explorer"); show: menu.localMedia; onTriggered: canvas.revealElement(menu.view.selectedId) }
        Row { label: qsTr("Open in ufb");    show: menu.localMedia;        onTriggered: canvas.openElementInUfb(menu.view.selectedId) }
        Row { label: qsTr("Copy path");      show: menu.localMedia;        onTriggered: clipboard.writeText(canvas.elementLocalPath(menu.view.selectedId)) }
        Row { label: qsTr("Copy link");      show: menu.n === 1 && menu.kind1 === "media" && canvas.mediaElementKind(menu.view.selectedId) === "bookmark"
              onTriggered: clipboard.writeText(menu.view.mediaOf(menu.view.selectedId).src || "") }
        Row { label: qsTr("Spread files here"); show: menu.view.selectedIsFolder; onTriggered: menu.view.spreadSelected() }
        Row { label: qsTr("Refresh");        show: menu.view.selectedIsFolder; onTriggered: canvas.rescanFolder(menu.view.selectedId) }
        Row { label: qsTr("Open as stream"); show: menu.view.selectedIsFolder && canvas.folderIsStream(menu.view.selectedId)
              onTriggered: docs.openStreamFolder(canvas.elementLocalPath(menu.view.selectedId)) }   // a QCView stream folder → its notes document (step 9)
        Row { label: qsTr("Notes as a page below"); show: menu.view.selectedIsVideo; onTriggered: menu.view.notesPageBelow(menu.view.selectedId) }   // the clip's QCView notes as blocks on a new page (2026-10-18)
        Row { label: qsTr("Next page");      show: menu.view.selectedIsPdf; onTriggered: menu.view.pdfStepSelected(1) }
        Row { label: qsTr("Previous page");  show: menu.view.selectedIsPdf; onTriggered: menu.view.pdfStepSelected(-1) }
        Row { label: qsTr("Fit height to content"); show: menu.n === 1 && menu.kind1 === "page" && canvas.pageHeightFixed(menu.view.selectedId)
              onTriggered: canvas.fitPageHeight(menu.view.selectedId) }   // a hand-sized page back to auto height (walk 7c)
        Row { label: qsTr("Duplicate");      show: menu.anyNonPage;        onTriggered: menu.view.duplicateSelected() }
        Row { label: qsTr("Bring to front"); show: menu.n > 0;             onTriggered: menu.view.frontSelected() }
        Row { label: qsTr("Send to back");   show: menu.n > 0;             onTriggered: menu.view.backSelected() }
        Row { label: qsTr("Delete");         show: menu.n > 0;             onTriggered: menu.view.deleteSelected() }
        Row { label: qsTr("New page here");  show: menu.n === 0
              onTriggered: { const id = canvas.addPageAt(menu.worldX, menu.worldY); if (id !== "") menu.view.select(id) } }
        Row { label: qsTr("Add link…");      show: menu.n === 0;           onTriggered: menu.view.promptLink(menu.worldX, menu.worldY) }   // a bookmark element from a typed URL (2026-10-18)
    }
}
