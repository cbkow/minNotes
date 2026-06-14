import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtCore
import Qt.labs.platform as Platform

ApplicationWindow {
    id: win
    visible: true
    width: 960
    height: 760
    title: blockModel.documentOpen
           ? (blockModel.documentName + (blockModel.untitled ? " (unsaved)" : "") + " — minNotes")
           : "minNotes"
    color: Theme.colors.bg

    // --- Document lifecycle (Phase 1: native dialogs + standard shortcuts; the
    // macOS menu bar lands in Phase 2). The file IS the SQLite DB; edits persist
    // live, so Save just checkpoints the WAL and Save As copies DB + media. ---
    FileDialog {
        id: openDialog
        title: "Open document"
        nameFilters: ["minNotes documents (*.mndb)"]
        onAccepted: blockModel.openDocument("" + selectedFile)
    }
    FileDialog {
        id: saveAsDialog
        title: "Save As"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mndb"
        nameFilters: ["minNotes documents (*.mndb)"]
        onAccepted: blockModel.saveAs("" + selectedFile)
    }
    function _saveOrSaveAs() { if (!blockModel.save()) saveAsDialog.open() }

    // --- Recent documents (persisted JSON list, most-recent first, cap 10) ---
    Settings { id: recentsStore; category: "recents"; property string paths: "[]" }
    function recentPaths() { try { var a = JSON.parse(recentsStore.paths); return Array.isArray(a) ? a : [] } catch (e) { return [] } }
    // Re-evaluates when the store changes (ternary, not a comma-tuple — qmlcachegen
    // elides the discarded left operand of a comma).
    readonly property var recents: recentsStore.paths.length >= 0 ? recentPaths() : []
    function addRecent(path) {
        if (!path) return
        var list = recentPaths().filter(function (p) { return p !== path })
        list.unshift(path)
        if (list.length > 10) list = list.slice(0, 10)
        recentsStore.paths = JSON.stringify(list)
    }
    function baseName(path) { var n = ("" + path).split("/").pop(); return n.replace(/\.mndb$/i, "") }
    Connections {
        target: blockModel
        function onDocumentChanged() {
            if (blockModel.documentOpen && !blockModel.untitled) win.addRecent(blockModel.documentPath)
        }
    }

    // --- Native macOS menu bar (Qt.labs.platform → system menu bar) ---
    Platform.MenuBar {
        Platform.Menu {
            title: qsTr("File")
            Platform.MenuItem { text: qsTr("New");   shortcut: StandardKey.New;  onTriggered: blockModel.newDocument() }
            Platform.MenuItem { text: qsTr("Open…"); shortcut: StandardKey.Open; onTriggered: openDialog.open() }
            Platform.Menu {
                id: recentMenu
                title: qsTr("Open Recent")
                Instantiator {
                    model: win.recents
                    delegate: Platform.MenuItem {
                        required property var modelData
                        text: win.baseName(modelData)
                        onTriggered: blockModel.openDocument(modelData)
                    }
                    onObjectAdded: (i, obj) => recentMenu.insertItem(i, obj)
                    onObjectRemoved: (i, obj) => recentMenu.removeItem(obj)
                }
                Platform.MenuSeparator {}
                Platform.MenuItem { text: qsTr("Clear Menu"); onTriggered: recentsStore.paths = "[]" }
            }
            Platform.MenuSeparator {}
            Platform.MenuItem { text: qsTr("Save");    shortcut: StandardKey.Save;   enabled: blockModel.documentOpen; onTriggered: win._saveOrSaveAs() }
            Platform.MenuItem { text: qsTr("Save As…"); shortcut: StandardKey.SaveAs; enabled: blockModel.documentOpen; onTriggered: saveAsDialog.open() }
            Platform.MenuSeparator {}
            Platform.MenuItem { text: qsTr("Close"); shortcut: StandardKey.Close; enabled: blockModel.documentOpen; onTriggered: blockModel.closeDocument() }
            Platform.MenuSeparator {}
            // Sparkle "Check for Updates…" (ApplicationSpecificRole keeps it in
            // place rather than being merged into a system role). No-op on builds
            // without Sparkle vendored.
            Platform.MenuItem {
                text: qsTr("Check for Updates…")
                role: Platform.MenuItem.ApplicationSpecificRole
                onTriggered: appUpdater.checkForUpdates()
            }
        }
    }

    // First app setting: window geometry persists across launches. Family
    // pattern (QtCore Settings) with UFB's hard-won rules: capture only
    // windowed-mode geometry (never the maximized rect, so un-maximize keeps
    // its target), debounce the capture, restore the size always but the
    // position only when it still overlaps a screen, and gate capture until
    // the restore has run (else the restore itself overwrites the store).
    Settings {
        id: winState
        category: "window"
        property int  w: 960
        property int  h: 760
        property int  wx: -1
        property int  wy: -1
        property bool maximized: false
    }
    property bool _geomRestored: false
    function _fitsAScreen(wx, wy, ww, wh) {
        var screens = Qt.application.screens
        for (var i = 0; i < screens.length; ++i) {
            var s = screens[i]
            if (wx + ww - 50 >= s.virtualX && wx + 50 <= s.virtualX + s.width
                && wy + wh - 50 >= s.virtualY && wy + 50 <= s.virtualY + s.height)
                return true
        }
        return false
    }
    Component.onCompleted: {
        width = Math.max(640, winState.w)
        height = Math.max(480, winState.h)
        if (winState.wx >= 0 && _fitsAScreen(winState.wx, winState.wy, width, height)) {
            x = winState.wx; y = winState.wy
        }
        if (winState.maximized) win.showMaximized()
        _geomRestored = true
    }
    function _captureGeometry() {
        if (!_geomRestored) return
        if (win.visibility === Window.Maximized || win.visibility === Window.FullScreen) {
            winState.maximized = (win.visibility === Window.Maximized)
            return
        }
        winState.maximized = false
        winState.w = win.width; winState.h = win.height
        winState.wx = win.x;    winState.wy = win.y
    }
    onWidthChanged: geomSave.restart()
    onHeightChanged: geomSave.restart()
    onXChanged: geomSave.restart()
    onYChanged: geomSave.restart()
    onVisibilityChanged: geomSave.restart()
    Timer { id: geomSave; interval: 500; onTriggered: win._captureGeometry() }

    // Thin left action rail + (editor page over a bottom status strip) + a
    // collapsible right inspector that slides in/out (default hidden → "min" by
    // default). The left rail's palette button toggles the inspector.
    Row {
        anchors.fill: parent
        // editor is null while no document is open — the rail/inspector guard it,
        // and the welcome overlay covers everything.
        readonly property var editor: docContent.item ? docContent.item.editorItem : null
        LeftRail { id: rail; height: parent.height; editor: parent.editor; inspector: inspector }
        // The editor surface only exists while a document is open, so no binding
        // ever queries the empty model (which would index empty vectors → crash).
        Loader {
            id: docContent
            active: blockModel.documentOpen
            width: parent.width - rail.width - inspector.width; height: parent.height
            sourceComponent: Column {
                anchors.fill: parent
                property alias editorItem: editorInner
                // The validated passive-surface editor (model owns the cursor; blocks
                // are passive; overlay-drawn caret/selection). Ported from spike Arm C.
                Editor { id: editorInner; width: parent.width; height: parent.height - innerTabs.height - innerBottom.height
                         inspector: inspector }
                TableTabs { id: innerTabs; width: parent.width; editor: editorInner }
                BottomRail { id: innerBottom; width: parent.width; editor: editorInner }
            }
        }
        Inspector { id: inspector; height: parent.height; editor: parent.editor }
    }

    // --- No-document welcome state: covers the (empty) editor until a document is
    // opened or created. New / Open + a recents list. ---
    Rectangle {
        anchors.fill: parent
        visible: !blockModel.documentOpen
        z: 100
        color: Theme.colors.bg
        // Swallow stray clicks so nothing reaches the rail/editor underneath.
        MouseArea { anchors.fill: parent }

        Column {
            anchors.centerIn: parent
            width: 440
            spacing: 22

            Column {
                width: parent.width; spacing: 4
                Text { text: "minNotes"; color: Theme.colors.textBright
                       font.family: Theme.font.family; font.pixelSize: 34; font.bold: true }
                Text { text: "Create a new document or open an existing one."
                       color: Theme.colors.textMuted
                       font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody }
            }

            Row {
                spacing: 10
                FlatButton {
                    text: "New document"; iconName: "file"; variant: "primary"
                    padding: 16; implicitHeight: 40
                    onClicked: blockModel.newDocument()
                }
                FlatButton {
                    text: "Open…"; iconName: "folder-open"
                    padding: 16; implicitHeight: 40
                    onClicked: openDialog.open()
                }
            }

            Column {
                width: parent.width; spacing: 6
                visible: win.recents.length > 0
                Text { text: "Recent"; color: Theme.colors.textMuted
                       font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall; font.bold: true }
                Repeater {
                    model: win.recents
                    delegate: Rectangle {
                        id: recentItem
                        required property var modelData
                        width: 440; height: 40; radius: 0
                        color: recentMA.containsMouse ? Theme.colors.surfaceHover : Theme.colors.surface
                        border.width: 1; border.color: Theme.colors.border
                        Row {
                            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                            spacing: 10
                            Icon { name: "file-text"; size: 18; color: Theme.colors.textMuted
                                   anchors.verticalCenter: parent.verticalCenter }
                            Column {
                                anchors.verticalCenter: parent.verticalCenter
                                width: recentItem.width - 64
                                Text { text: win.baseName(recentItem.modelData)
                                       color: Theme.colors.text; elide: Text.ElideRight; width: parent.width
                                       font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody }
                                Text { text: "" + recentItem.modelData
                                       color: Theme.colors.textSubtle; elide: Text.ElideMiddle; width: parent.width
                                       font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall }
                            }
                        }
                        MouseArea {
                            id: recentMA
                            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: blockModel.openDocument(recentItem.modelData)
                        }
                    }
                }
            }
        }
    }
}
