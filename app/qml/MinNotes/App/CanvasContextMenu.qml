// The canvas's context menu (2.0): a house-style popup whose rows are FILTERED by what is selected — a row that
// can't apply is not greyed, it is absent. The menu grammar (2026-10-21, shared with the block menu): the primary
// open · the thing's own actions · insert · arrange · delete, a rule between sections, the destructive row red
// and last.
import QtQuick
import QtQuick.Controls

Popup {
    id: menu
    required property var view
    property real worldX: 0
    property real worldY: 0
    padding: 4
    width: 208
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.dim.popupMs } }
    background: Rectangle { color: Theme.colors.surface; radius: Theme.dim.radius; border.width: 1; border.color: Theme.colors.border }

    readonly property int n: view.selectionCount
    readonly property string kind1: view.selectedId !== "" ? canvas.elementKind(view.selectedId) : ""
    readonly property bool anyNonPage: view.selectedIds.some(id => canvas.elementKind(id) !== "page")
    // One LOCAL media element (a file on disk, or one that would be): its path actions (phase A4).
    readonly property bool localMedia: n === 1 && kind1 === "media" && canvas.mediaElementKind(view.selectedId) !== "bookmark"
                                       && !/^https?:/.test(canvas.elementLocalPath(view.selectedId) === "" ? "https:" : "")
    readonly property bool isBookmark: n === 1 && kind1 === "media" && canvas.mediaElementKind(view.selectedId) === "bookmark"
    readonly property bool isPreview: (canvas.elementsRevision, n === 1 && kind1 === "media" && canvas.elementPreviewFormat(view.selectedId) !== "")   // the preview pass (2026-10-21)
    readonly property bool onMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property int visibleRows: { let c = 0; for (let i = 0; i < rows.children.length; ++i) if (rows.children[i].visible && rows.children[i].isMenuRow === true) ++c; return c }   // probe: rows, not rules
    function popupAt(sx, sy) { x = sx; y = sy; open() }

    component Row: ContextMenuRow {
        property string label: ""
        property bool show: true
        signal triggered()
        text: label
        visible: show
        onClicked: { menu.close(); row.triggered() }
        id: row
    }
    component Sep: ContextMenuSep {}

    // The sections' visibility, so a rule never stands alone.
    readonly property bool primary: n === 1 && view.canOpen(view.selectedId)
    readonly property bool own: localMedia || isBookmark || view.selectedIsFolder || view.selectedIsVideo || view.selectedIsPdf
                                || (n === 1 && kind1 === "page" && canvas.pageHeightFixed(view.selectedId))
                                || (canvas.elementsRevision, n === 1 && kind1 === "shape" && view.textFixed(view.selectedId))
    readonly property bool arrange: n > 0

    contentItem: Column {
        id: rows
        width: menu.width - 8
        spacing: 1
        // 1. Primary
        Row { label: menu.n === 1 ? menu.view.openLabel(menu.view.selectedId) : ""; show: menu.primary; onTriggered: menu.view.openSelected() }
        Sep { visible: menu.primary && (menu.own || menu.arrange) }
        // 2. The thing's own actions
        Row { label: menu.onMac ? qsTr("Reveal in Finder") : qsTr("Show in Explorer"); show: menu.localMedia; onTriggered: canvas.revealElement(menu.view.selectedId) }
        Row { label: qsTr("Open in ufb");    show: menu.localMedia;        onTriggered: canvas.openElementInUfb(menu.view.selectedId) }
        Row { label: qsTr("Copy path");      show: menu.localMedia;        onTriggered: clipboard.writeText(canvas.elementSourcePath(menu.view.selectedId)) }   // a preview's ORIGINAL
        Row { label: qsTr("Refresh preview"); show: menu.isPreview;        onTriggered: canvas.refreshElementPreview(menu.view.selectedId) }   // re-render from the PSD / HEIC / RAW / HDR
        Row { label: qsTr("Copy link");      show: menu.isBookmark;        onTriggered: clipboard.writeText(menu.view.mediaOf(menu.view.selectedId).src || "") }
        Row { label: qsTr("Spread files here"); show: menu.view.selectedIsFolder; onTriggered: menu.view.spreadSelected() }
        Row { label: qsTr("Refresh");        show: menu.view.selectedIsFolder; onTriggered: canvas.rescanFolder(menu.view.selectedId) }
        Row { label: qsTr("Open as stream"); show: menu.view.selectedIsFolder && canvas.folderIsStream(menu.view.selectedId)
              onTriggered: docs.openStreamFolder(canvas.elementLocalPath(menu.view.selectedId)) }   // a QCView stream folder → its notes document (step 9)
        Row { label: qsTr("Notes page below"); show: menu.view.selectedIsVideo; onTriggered: menu.view.notesPageBelow(menu.view.selectedId) }   // the clip's QCView notes as a page under it
        Row { label: qsTr("Next page");      show: menu.view.selectedIsPdf; onTriggered: menu.view.pdfStepSelected(1) }
        Row { label: qsTr("Previous page");  show: menu.view.selectedIsPdf; onTriggered: menu.view.pdfStepSelected(-1) }
        Row { label: qsTr("Fit height to content"); show: menu.n === 1 && menu.kind1 === "page" && canvas.pageHeightFixed(menu.view.selectedId)
              onTriggered: canvas.fitPageHeight(menu.view.selectedId) }   // a hand-sized page back to auto height (walk 7c)
        Row { label: qsTr("Fit width to text"); show: (canvas.elementsRevision, menu.n === 1 && menu.kind1 === "shape" && menu.view.textFixed(menu.view.selectedId))
              onTriggered: menu.view.unpinText(menu.view.selectedId) }   // a pinned-width text chip back to auto width (2026-10-19)
        Sep { visible: menu.own && menu.arrange }
        // 3. Insert (empty canvas)
        Row { label: qsTr("New page here");  show: menu.n === 0
              onTriggered: { const id = canvas.addPageAt(menu.worldX, menu.worldY); if (id !== "") menu.view.select(id) } }
        Row { label: qsTr("Add link…");      show: menu.n === 0;           onTriggered: menu.view.promptLink(menu.worldX, menu.worldY) }   // a bookmark element from a typed URL
        // 4. Arrange
        Row { label: qsTr("Duplicate");      show: menu.anyNonPage;        onTriggered: menu.view.duplicateSelected() }
        Row { label: qsTr("Bring to front"); show: menu.arrange;           onTriggered: menu.view.frontSelected() }
        Row { label: qsTr("Send to back");   show: menu.arrange;           onTriggered: menu.view.backSelected() }
        Sep { visible: menu.arrange }
        // 5. Delete
        Row { label: qsTr("Delete");         show: menu.arrange; danger: true; onTriggered: menu.view.deleteSelected() }
    }
}
