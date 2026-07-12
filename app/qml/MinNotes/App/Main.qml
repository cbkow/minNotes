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
           ? (blockModel.documentName
              + (blockModel.untitled ? " (unsaved)" : (blockModel.dirty ? " — Edited" : ""))
              + " — minNotes")
           : "minNotes"
    color: Theme.colors.bg

    // --- App-close guard: prompt before discarding unsaved work in any tab.
    // Two entry points share the dialog: the window close button arrives via
    // onClosing; ⌘Q / logout / last-window-closed arrive as QEvent::Quit, which
    // MinNotesApplication vetoes and re-emits as quitRequested (the raw quit
    // path skips onClosing, and aboutToQuit deletes the scratch working
    // copies — so it must never run before this guard clears). ---
    property bool _forceClose: false
    onClosing: (close) => {
        if (win._forceClose || docs.dirtyCount === 0) return   // nothing unsaved → allow
        close.accepted = false
        quitConfirmDialog.open()
    }
    Connections {
        target: minApp
        function onQuitRequested() {
            if (docs.dirtyCount === 0) { win._forceClose = true; minApp.forceQuit() }
            else quitConfirmDialog.open()
        }
    }
    Dialog {
        id: quitConfirmDialog
        modal: true; anchors.centerIn: Overlay.overlay; width: 460; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text { width: 420; wrapMode: Text.Wrap
                   text: docs.dirtyCount === 1 ? "You have unsaved changes in 1 document."
                                               : "You have unsaved changes in " + docs.dirtyCount + " documents."
                   color: Theme.colors.textBright; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody; font.bold: true }
            Text { width: 420; wrapMode: Text.Wrap
                   text: "Quitting will lose changes you haven’t saved."
                   color: Theme.colors.textMuted; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton { text: "Cancel"; padding: 12; onClicked: quitConfirmDialog.close() }
                // Both quit buttons go through minApp.forceQuit() — NOT
                // win.close(): a bare close with dirty docs would trip the
                // QEvent::Quit veto again after the window is gone (zombie
                // process, no window to host this dialog).
                FlatButton { text: "Discard & Quit"; padding: 12
                             onClicked: { quitConfirmDialog.close(); win._forceClose = true; minApp.forceQuit() } }
                FlatButton { text: "Save All & Quit"; variant: "primary"; padding: 12
                             onClicked: {
                                 quitConfirmDialog.close()
                                 if (docs.saveAllTitled()) { win._forceClose = true; minApp.forceQuit() }
                                 else { var i = docs.firstDirtyIndex()   // untitled/conflict left — focus it
                                        if (i >= 0) win.switchToTab(i) }
                             } }
            }
        }
    }

    // --- Document lifecycle (Phase 1: native dialogs + standard shortcuts; the
    // macOS menu bar lands in Phase 2). The file IS the SQLite DB; edits persist
    // live, so Save just checkpoints the WAL and Save As copies DB + media. ---
    FileDialog {
        id: openDialog
        title: "Open document"
        nameFilters: ["minNotes documents (*.mndb)"]
        onAccepted: win.openDoc(selectedFile)
    }
    FileDialog {
        id: saveAsDialog
        title: "Save As"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mndb"
        nameFilters: ["minNotes documents (*.mndb)"]
        onAccepted: {
            var f = "" + selectedFile
            if (blockModel.saveAs(f)) Toasts.show(qsTr("Saved as ") + win.baseName(f))
            else saveFailedDialog.open()   // was silently ignored before the toast pass
        }
    }
    // --- Export (File ▸ Export as Markdown…). A pre-scan decides whether the
    // options dialog appears at all: options only exist when the document
    // carries the thing they govern (video notes, page ink) — the common case
    // goes straight to the save dialog. ---
    property var _exportScan: ({})
    property bool _exportNotes: true
    property string _exportFormat: "md"   // "md" | "html"
    function startExport(format) {
        _exportFormat = format || "md"
        _exportScan = exporter.scan()
        _exportNotes = true
        // Ink only affects markdown (HTML doesn't carry it either yet, but
        // the note is markdown-worded); notes matter to both.
        if ((_exportScan.videoNotes || 0) > 0 || (_exportScan.inkBlocks || 0) > 0)
            exportOptionsDialog.open()
        else
            exportSaveDialog.open()
    }
    FileDialog {
        id: exportSaveDialog
        title: win._exportFormat === "html" ? "Export as HTML" : "Export as Markdown"
        fileMode: FileDialog.SaveFile
        defaultSuffix: win._exportFormat
        nameFilters: win._exportFormat === "html" ? ["HTML (*.html)"] : ["Markdown (*.md)"]
        onAccepted: {
            var f = "" + selectedFile
            var ok = win._exportFormat === "html"
                ? exporter.exportHtml(f, win._exportNotes)
                : exporter.exportMarkdown(f, win._exportNotes)
            if (ok) Toasts.show(qsTr("Exported ") + win.baseName(f))
            else    Toasts.show(qsTr("Export failed"), 2)
        }
    }
    Dialog {
        id: exportOptionsDialog
        modal: true; anchors.centerIn: Overlay.overlay; width: 440; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text { width: 400; wrapMode: Text.Wrap
                   text: win._exportFormat === "html" ? qsTr("Export as HTML")
                                                      : qsTr("Export as Markdown")
                   color: Theme.colors.textBright; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody; font.bold: true }
            Row {   // video-notes option — present only when notes were detected
                visible: (win._exportScan.videoNotes || 0) > 0
                spacing: 8
                Rectangle {   // squared family checkbox (PathMappings pattern)
                    width: 16; height: 16
                    anchors.verticalCenter: parent.verticalCenter
                    color: win._exportNotes ? Theme.colors.divider : "transparent"
                    border.width: 1
                    border.color: win._exportNotes ? Theme.colors.textBright : Theme.colors.border
                    Text { anchors.centerIn: parent; visible: win._exportNotes
                           text: "✓"; color: Theme.colors.textBright; font.pixelSize: 11 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: win._exportNotes = !win._exportNotes }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        var n = win._exportScan.videoNotes || 0
                        var v = win._exportScan.videosWithNotes || 0
                        return qsTr("Include video notes (%1 %2 on %3 %4)")
                            .arg(n).arg(n === 1 ? qsTr("note") : qsTr("notes"))
                            .arg(v).arg(v === 1 ? qsTr("video") : qsTr("videos"))
                    }
                    color: Theme.colors.text
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: win._exportNotes = !win._exportNotes }
                }
            }
            Text {
                visible: (win._exportScan.inkBlocks || 0) > 0
                width: 400; wrapMode: Text.Wrap
                text: win._exportFormat === "html"
                      ? qsTr("Page ink exports as toggleable overlays (ink over text is position-approximate).")
                      : qsTr("Page ink isn't included in Markdown export.")
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton { text: qsTr("Cancel"); padding: 12
                             onClicked: exportOptionsDialog.close() }
                FlatButton { text: qsTr("Export…"); variant: "primary"; padding: 12
                             onClicked: { exportOptionsDialog.close(); exportSaveDialog.open() } }
            }
        }
    }

    // Mirrors BlockModel::SaveState (the enum isn't registered as a QML type — the
    // model is a context-property instance — so compare the int saveState here).
    readonly property int _saveClean: 0
    readonly property int _saveSaving: 1
    readonly property int _saveFailed: 2
    readonly property int _saveConflict: 3

    // Explicit save. Untitled → Save As; a save that's blocked by an external
    // change opens the conflict dialog; a write failure opens the failed dialog.
    function _saveOrSaveAs() {
        if (!blockModel.documentOpen) return
        if (blockModel.untitled) { saveAsDialog.open(); return }
        if (blockModel.save()) { Toasts.show(qsTr("Saved")); return }
        if (blockModel.saveState === win._saveConflict) conflictDialog.open()
        else if (blockModel.saveState === win._saveFailed) saveFailedDialog.open()
    }

    // The original on disk changed since we opened it — don't clobber silently.
    Dialog {
        id: conflictDialog
        modal: true; anchors.centerIn: Overlay.overlay; width: 440; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text { width: 400; wrapMode: Text.Wrap
                   text: "“" + blockModel.documentName + "” changed on disk"
                   color: Theme.colors.textBright; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody; font.bold: true }
            Text { width: 400; wrapMode: Text.Wrap
                   text: "Another program changed this document since you opened it. "
                       + "Overwriting replaces those changes with your version."
                   color: Theme.colors.textMuted; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton { text: "Cancel"; padding: 12; onClicked: conflictDialog.close() }
                FlatButton { text: "Save As copy…"; padding: 12
                             onClicked: { conflictDialog.close(); saveAsDialog.open() } }
                FlatButton { text: "Overwrite"; variant: "primary"; padding: 12
                             onClicked: { conflictDialog.close()
                                          if (blockModel.overwriteSave()) Toasts.show(qsTr("Saved"))
                                          else saveFailedDialog.open() } }
            }
        }
    }

    // The write-back to the original failed (network dropped, permission, disk).
    Dialog {
        id: saveFailedDialog
        modal: true; anchors.centerIn: Overlay.overlay; width: 440; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text { width: 400; wrapMode: Text.Wrap
                   text: "Couldn’t save “" + blockModel.documentName + "”"
                   color: Theme.colors.textBright; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody; font.bold: true }
            Text { width: 400; wrapMode: Text.Wrap
                   text: "The document couldn’t be written to its location. Your edits are "
                       + "safe in this window — retry, or save a copy somewhere else."
                   color: Theme.colors.textMuted; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton { text: "Cancel"; padding: 12; onClicked: saveFailedDialog.close() }
                FlatButton { text: "Save As…"; padding: 12
                             onClicked: { saveFailedDialog.close(); saveAsDialog.open() } }
                FlatButton { text: "Retry"; variant: "primary"; padding: 12
                             onClicked: { saveFailedDialog.close()
                                          if (!blockModel.overwriteSave()) saveFailedDialog.open() } }
            }
        }
    }

    // --- Close / quit guards: unsaved edits prompt before discard. ---
    property int _closePendingIndex: -1
    function requestCloseTab(i) {
        var m = docs.models[i]
        if (m && m.dirty) { win._closePendingIndex = i; closeConfirmDialog.open() }
        else win.closeTabAt(i)
    }
    Dialog {
        id: closeConfirmDialog
        modal: true; anchors.centerIn: Overlay.overlay; width: 440; padding: 20
        readonly property var pendingModel: win._closePendingIndex >= 0
                                          ? docs.models[win._closePendingIndex] : null
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text { width: 400; wrapMode: Text.Wrap
                   text: "Save changes to “"
                       + (closeConfirmDialog.pendingModel ? closeConfirmDialog.pendingModel.documentName : "")
                       + "”?"
                   color: Theme.colors.textBright; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody; font.bold: true }
            Text { width: 400; wrapMode: Text.Wrap
                   text: "Your changes will be lost if you don’t save them."
                   color: Theme.colors.textMuted; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton { text: "Cancel"; padding: 12
                             onClicked: { win._closePendingIndex = -1; closeConfirmDialog.close() } }
                FlatButton { text: "Don’t Save"; padding: 12
                             onClicked: { var i = win._closePendingIndex
                                          win._closePendingIndex = -1; closeConfirmDialog.close()
                                          win.closeTabAt(i) } }
                FlatButton { text: "Save"; variant: "primary"; padding: 12
                             onClicked: win._saveThenClosePending() }
            }
        }
    }
    // "Save" from the close prompt: titled → save then close; untitled → switch to
    // it and open Save As (closing is left to the user after they pick a path).
    function _saveThenClosePending() {
        var i = win._closePendingIndex
        var m = docs.models[i]
        closeConfirmDialog.close()
        if (!m) { win._closePendingIndex = -1; return }
        if (m.untitled) { win.switchToTab(i); saveAsDialog.open(); return }
        if (m.save()) { win._closePendingIndex = -1; win.closeTabAt(i); return }
        // Conflict / failure → focus the tab and surface the right dialog.
        win.switchToTab(i)
        if (m.saveState === win._saveConflict) conflictDialog.open()
        else saveFailedDialog.open()
    }

    // Cross-OS path mappings editor (opened from the File menu). Edits a draft +
    // commits through the `pathMap` bridge, which re-resolves open media.
    PathMappingsDialog { id: pathMappingsDialog; anchors.centerIn: Overlay.overlay }

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
    function removeRecent(path) {
        recentsStore.paths = JSON.stringify(
            recentPaths().filter(function (p) { return p !== path }))
    }
    // Open with failure feedback. A dead recents entry (the file was moved,
    // renamed, or deleted) used to fail SILENTLY — indistinguishable from the
    // app just not opening anything. Now it says so and prunes the entry.
    property string openFailedPath: ""
    function openDoc(path) {
        path = "" + path
        if (docs.openTab(path)) return
        openFailedPath = path
        removeRecent(path)
        openFailedDialog.open()
    }
    Dialog {
        id: openFailedDialog
        modal: true; anchors.centerIn: Overlay.overlay; width: 440; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text { width: 400; wrapMode: Text.Wrap
                   text: "Couldn’t open “" + win.baseName(win.openFailedPath) + "”"
                   color: Theme.colors.textBright; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody; font.bold: true }
            Text { width: 400; wrapMode: Text.Wrap
                   text: "The file may have been moved, renamed, or deleted:\n"
                       + win.openFailedPath + "\n\nIt has been removed from Open Recent."
                   color: Theme.colors.textMuted; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton { text: "OK"; variant: "primary"; padding: 12
                             onClicked: openFailedDialog.close() }
            }
        }
    }
    Connections {
        target: blockModel
        function onDocumentChanged() {
            // Save As on the active model (untitled → named) lands here.
            if (blockModel.documentOpen && !blockModel.untitled) win.addRecent(blockModel.documentPath)
        }
    }
    // A tab opened or was switched to → record it (a newly-opened doc emits
    // documentChanged on its own model BEFORE it becomes active, so the
    // blockModel-targeted Connections above can't catch it — this does).
    Connections {
        target: docs
        function onActiveChanged() {
            if (blockModel.documentOpen && !blockModel.untitled) win.addRecent(blockModel.documentPath)
        }
    }

    // --- Multi-document tabs: one shared Editor, `blockModel` re-points to the
    // active tab's model (main.cpp, on docs.activeChanged). Switching saves the
    // outgoing tab's QML view state (scroll/caret/active sub-tab) and restores
    // the incoming one's; the blob is held per-tab by the DocumentManager. ---
    function _editor() { return docContent.item ? docContent.item.editorItem : null }
    function _restoreActiveView() {
        var ed = win._editor()
        if (ed && docs.activeIndex >= 0)
            ed.restoreViewState(docs.viewState(docs.tabIdAt(docs.activeIndex)))
    }
    function switchToTab(i) {
        if (i === docs.activeIndex) return
        var ed = win._editor()
        var curId = docs.tabIdAt(docs.activeIndex)
        if (ed && curId >= 0) docs.setViewState(curId, ed.captureViewState())
        docs.setActive(i)
        Qt.callLater(win._restoreActiveView)   // after blockModel re-points + bindings settle
    }
    function closeTabAt(i) {
        var wasActive = (i === docs.activeIndex)
        docs.closeTab(i)
        if (wasActive) Qt.callLater(win._restoreActiveView)
    }

    // --- Menu bar. macOS uses the native Qt.labs.platform system menu bar
    // (a Component created on macOS only). Windows/Linux use an in-window Qt
    // Quick Controls MenuBar themed via Fusion `palette` + the ThemedMenu
    // wrapper — the sister-app pattern (Fusion honors palette, so plain
    // Action/MenuItem need no custom delegates). Both are created in
    // Component.onCompleted so the Windows menu's shortcut Actions exist only on
    // that platform (no double-binding with the macOS native menu). ---
    Component {
        id: macMenuBarComp
        Platform.MenuBar {
            Platform.Menu {
                title: qsTr("File")
                Platform.MenuItem { text: qsTr("New");   shortcut: StandardKey.New;  onTriggered: docs.newTab() }
                Platform.MenuItem { text: qsTr("Open…"); shortcut: StandardKey.Open; onTriggered: openDialog.open() }
                Platform.Menu {
                    id: recentMenu
                    title: qsTr("Open Recent")
                    Instantiator {
                        model: win.recents
                        delegate: Platform.MenuItem {
                            required property var modelData
                            text: win.baseName(modelData)
                            onTriggered: win.openDoc(modelData)
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
                Platform.MenuItem { text: qsTr("Export as Markdown…"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen; onTriggered: win.startExport("md") }
                Platform.MenuItem { text: qsTr("Export as HTML…"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen; onTriggered: win.startExport("html") }
                Platform.MenuSeparator {}
                Platform.MenuItem { text: qsTr("Close"); shortcut: StandardKey.Close; enabled: blockModel.documentOpen; onTriggered: win.requestCloseTab(docs.activeIndex) }
                Platform.MenuSeparator {}
                // NoRole keeps it in File (reliably visible); see the updater note below.
                Platform.MenuItem { text: qsTr("Path Mappings…"); role: Platform.MenuItem.NoRole; onTriggered: pathMappingsDialog.open2() }
                Platform.MenuSeparator {}
                // Keep it in the File menu (NoRole), matching the Windows menu.
                // NB: the default TextHeuristicRole — and ApplicationSpecificRole —
                // RELOCATE immediate-menubar items into the macOS application menu
                // (Qt docs); when that relocation doesn't land it vanishes from
                // both menus, which is why this previously didn't appear. NoRole
                // pins it here reliably. No-op on builds without Sparkle vendored.
                Platform.MenuItem {
                    text: qsTr("Check for Updates…")
                    role: Platform.MenuItem.NoRole
                    onTriggered: appUpdater.checkForUpdates()
                }
            }
        }
    }

    Component {
        id: winMenuBarComp
        MenuBar {
            // Match the palette/inspector panel: surfaceRaised fill + bottom
            // hairline border; Fusion reads palette for the items + hover.
            palette.window:          Theme.colors.surfaceRaised
            palette.windowText:      Theme.colors.text
            palette.button:          Theme.colors.surfaceRaised
            palette.buttonText:      Theme.colors.text
            palette.highlight:       Theme.colors.surfaceHover
            palette.highlightedText: Theme.colors.textBright
            background: Rectangle {
                color: Theme.colors.surfaceRaised
                Rectangle {
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: 1
                    color: Theme.colors.border
                }
            }
            ThemedMenu {
                title: qsTr("&File")
                Action { text: qsTr("&New");   shortcut: StandardKey.New;  onTriggered: docs.newTab() }
                Action { text: qsTr("&Open…"); shortcut: StandardKey.Open; onTriggered: openDialog.open() }
                ThemedMenu {
                    id: winRecentMenu
                    title: qsTr("Open &Recent")
                    width: 360
                    // Repeater (vs Instantiator + insertItem): rebuilds cleanly
                    // when the recents list bumps-to-front on open.
                    Repeater {
                        model: win.recents
                        MenuItem {
                            required property var modelData
                            text: win.baseName(modelData)
                            onTriggered: win.openDoc(modelData)
                        }
                    }
                    Repeater { model: win.recents.length > 0 ? 1 : 0; ThemedMenuSeparator {} }
                    Repeater {
                        model: win.recents.length === 0 ? 1 : 0
                        MenuItem { text: qsTr("(none)"); enabled: false }
                    }
                    Repeater {
                        model: win.recents.length > 0 ? 1 : 0
                        MenuItem { text: qsTr("Clear Menu"); onTriggered: recentsStore.paths = "[]" }
                    }
                }
                ThemedMenuSeparator {}
                Action { text: qsTr("&Save");    shortcut: StandardKey.Save;   enabled: blockModel.documentOpen; onTriggered: win._saveOrSaveAs() }
                Action { text: qsTr("Save &As…"); shortcut: StandardKey.SaveAs; enabled: blockModel.documentOpen; onTriggered: saveAsDialog.open() }
                ThemedMenuSeparator {}
                Action { text: qsTr("&Export as Markdown…"); enabled: blockModel.documentOpen; onTriggered: win.startExport("md") }
                Action { text: qsTr("Export as &HTML…"); enabled: blockModel.documentOpen; onTriggered: win.startExport("html") }
                ThemedMenuSeparator {}
                Action { text: qsTr("&Close"); shortcut: StandardKey.Close; enabled: blockModel.documentOpen; onTriggered: win.requestCloseTab(docs.activeIndex) }
                ThemedMenuSeparator {}
                Action { text: qsTr("Path &Mappings…"); onTriggered: pathMappingsDialog.open2() }
                ThemedMenuSeparator {}
                Action { text: qsTr("Check for &Updates…"); onTriggered: appUpdater.checkForUpdates() }
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
        // macOS: native system menu bar. Windows/Linux: in-window themed bar
        // assigned as the window menuBar.
        if (Qt.platform.os === "osx") macMenuBarComp.createObject(win)
        else win.menuBar = winMenuBarComp.createObject(win)
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

    // Document tab strip on top + (thin left action rail + editor page over a
    // bottom status strip + a collapsible right inspector).
    Column {
        anchors.fill: parent

        // --- Multi-document tab strip. Browser-style top tabs, one per open
        // document; the active tab = divider fill + a white underline (style
        // rules: squared, accent reserved for real highlights). A trailing "+"
        // opens a new tab; each tab has a hover/active close affordance, and a
        // middle-click also closes. Hidden in the welcome state (no docs). ---
        Rectangle {
            id: docTabBar
            width: parent.width
            visible: docs.count > 0
            height: visible ? 36 : 0
            color: Theme.colors.surfaceRaised
            // (bottom hairline removed — the raised strip's tonal step against
            // the page separates on its own; border diet, product pass 2)

            Row {
                anchors.left: parent.left; anchors.top: parent.top
                anchors.right: navBtn.left
                height: parent.height
                clip: true
                Repeater {
                    model: docs.models
                    delegate: Rectangle {
                        id: tab
                        required property int index
                        required property var modelData     // this tab's BlockModel
                        readonly property bool active: index === docs.activeIndex
                        width: Math.min(220, tabLabel.implicitWidth + 56)
                        height: docTabBar.height
                        color: active ? Theme.colors.divider
                             : (tabMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
                        Rectangle {   // active underline (white)
                            visible: tab.active
                            anchors.bottom: parent.bottom; width: parent.width; height: 2
                            color: Theme.colors.textBright
                        }
                        Rectangle { anchors.right: parent.right; width: 1; height: parent.height
                                    color: Theme.colors.border }
                        Text {
                            id: tabLabel
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 12
                            anchors.right: tabClose.left; anchors.rightMargin: 4
                            elide: Text.ElideMiddle
                            // "•" marks an untitled (never-saved) OR a modified (unsaved-edits) doc.
                            text: ((tab.modelData.untitled || tab.modelData.dirty) ? "• " : "")
                                  + tab.modelData.documentName
                            color: tab.active ? Theme.colors.textBright : Theme.colors.textMuted
                            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
                        }
                        MouseArea {
                            id: tabMA
                            anchors.fill: parent; hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                            cursorShape: Qt.PointingHandCursor
                            onClicked: (m) => { if (m.button === Qt.MiddleButton) win.requestCloseTab(tab.index)
                                                else win.switchToTab(tab.index) }
                        }
                        Rectangle {   // close affordance (declared after tabMA so its MouseArea wins)
                            id: tabClose
                            anchors.right: parent.right; anchors.rightMargin: 7
                            anchors.verticalCenter: parent.verticalCenter
                            width: 18; height: 18
                            visible: tabMA.containsMouse || closeMA.containsMouse || tab.active
                            color: closeMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
                            Icon { anchors.centerIn: parent; name: "x"; size: 12
                                   color: closeMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted }
                            MouseArea { id: closeMA; anchors.fill: parent; hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: win.requestCloseTab(tab.index) }
                        }
                    }
                }
            }

            // Navigator: a list of every open document by FULL name — so the
            // strip stays usable when many docs are open (the tabs clip; this
            // is always one click away). Mirrors TableTabs' overflow menu, but
            // opens DOWNWARD (the strip is at the window's top edge).
            Rectangle {
                id: navBtn
                anchors.right: newTabBtn.left; anchors.top: parent.top
                width: 36; height: docTabBar.height   // one width for every strip button
                color: navMenu.visible ? Theme.colors.divider
                     : (navMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
                Rectangle { anchors.left: parent.left; width: 1; height: parent.height
                            color: Theme.colors.border }
                Icon { anchors.centerIn: parent; name: "list"; size: 16
                       color: navMenu.visible ? Theme.colors.textBright : Theme.colors.textMuted }
                MouseArea { id: navMA; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: navMenu.visible ? navMenu.close() : navMenu.open() }

                Popup {
                    id: navMenu
                    padding: 1
                    width: 320
                    height: Math.min(360, navCol.implicitHeight + 2)
                    x: navBtn.width - width        // right-aligned to the button
                    y: navBtn.height + 1            // drop down below the strip
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    background: Rectangle { color: Theme.colors.surface; radius: 0
                                            border.width: 1; border.color: Theme.colors.border }
                    contentItem: Flickable {
                        id: navFlick
                        clip: true
                        contentWidth: width
                        contentHeight: navCol.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: MnScrollBar {}
                        Column {
                            id: navCol
                            width: navFlick.width
                            Repeater {
                                model: docs.models
                                delegate: Rectangle {
                                    required property int index
                                    required property var modelData
                                    readonly property bool active: index === docs.activeIndex
                                    width: navCol.width; height: 28
                                    color: navItemMA.containsMouse ? Theme.colors.surfaceHover
                                         : (active ? Theme.colors.divider : "transparent")
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left; anchors.leftMargin: 12
                                        anchors.right: parent.right; anchors.rightMargin: 10
                                        // Same dirty-dot rule as the tab strip: unsaved OR edited.
                                        text: ((parent.modelData.untitled || parent.modelData.dirty) ? "• " : "")
                                              + parent.modelData.documentName
                                        elide: Text.ElideMiddle   // keep the distinguishing tail
                                        color: parent.active ? Theme.colors.textBright : Theme.colors.text
                                        font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
                                    }
                                    MouseArea {
                                        id: navItemMA
                                        anchors.fill: parent; hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: { win.switchToTab(parent.index); navMenu.close() }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {   // new-tab button, pinned right
                id: newTabBtn
                anchors.right: annotCluster.left; anchors.top: parent.top
                width: 36; height: docTabBar.height
                color: newMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
                Rectangle { anchors.left: parent.left; width: 1; height: parent.height
                            color: Theme.colors.border }
                Icon { anchors.centerIn: parent; name: "plus"; size: 16
                       color: newMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted }
                MouseArea { id: newMA; anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor; onClicked: docs.newTab() }
            }

            // Annotation controls at the strip's right edge (right of "+"):
            // document-level chrome lives on the tab row — no z-order fights
            // with the editor's overlays. Draw toggles annotation mode (and
            // opens the Inspector on its Palette view); the eye shows/hides
            // the ink layer and only appears when the document has ink.
            Row {
                id: annotCluster
                anchors.right: parent.right; anchors.top: parent.top
                readonly property var ed: contentRow.editor
                Rectangle {
                    visible: !!annotCluster.ed
                    width: 36; height: docTabBar.height
                    readonly property bool on: !!annotCluster.ed && annotCluster.ed.inkMode
                    // Accent-on-toggle is a DELIBERATE exception here (user
                    // ruling 2026-07-05): mode state earned the accent.
                    color: on ? Theme.colors.accent
                              : (annMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
                    Rectangle { anchors.left: parent.left; width: 1; height: parent.height
                                color: Theme.colors.border }
                    Icon { anchors.centerIn: parent; name: "pen-nib"; size: 15
                           color: parent.on || annMA.containsMouse ? Theme.colors.textBright
                                                                   : Theme.colors.textMuted }
                    MouseArea { id: annMA; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: annotCluster.ed.setInkMode(!annotCluster.ed.inkMode) }
                }
                Rectangle {
                    visible: !!annotCluster.ed && annotCluster.ed.inkStrokeCount > 0
                    width: 36; height: docTabBar.height
                    readonly property bool hidden: !!annotCluster.ed && !annotCluster.ed.inkLayerVisible
                    color: hidden ? Theme.colors.accent
                                  : (eyeMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
                    Rectangle { anchors.left: parent.left; width: 1; height: parent.height
                                color: Theme.colors.border }
                    Icon { anchors.centerIn: parent
                           name: parent.hidden ? "eye-slash" : "eye"; size: 15
                           color: parent.hidden || eyeMA.containsMouse ? Theme.colors.textBright
                                                                       : Theme.colors.textMuted }
                    MouseArea { id: eyeMA; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                enabled: !!annotCluster.ed && !annotCluster.ed.inkMode
                                onClicked: annotCluster.ed.inkLayerVisible = !annotCluster.ed.inkLayerVisible }
                }
                // Inspector (right sidebar) toggle — moved off the LeftRail.
                // The arrow points where the panel will GO: left (slide out
                // over the document) when closed, right (dismiss) when open.
                Rectangle {
                    visible: !!annotCluster.ed
                    width: 36; height: docTabBar.height
                    color: inspectorPanel.open ? Theme.colors.accent
                                               : (sideMA.containsMouse ? Theme.colors.surfaceHover : "transparent")
                    Rectangle { anchors.left: parent.left; width: 1; height: parent.height
                                color: Theme.colors.border }
                    Icon { anchors.centerIn: parent
                           name: inspectorPanel.open ? "arrow-line-right" : "arrow-line-left"; size: 15
                           color: inspectorPanel.open || sideMA.containsMouse ? Theme.colors.textBright
                                                                              : Theme.colors.textMuted }
                    MouseArea { id: sideMA; anchors.fill: parent; hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: inspectorPanel.open = !inspectorPanel.open }
                }
            }
        }

        Row {
            id: contentRow
            width: parent.width
            height: parent.height - docTabBar.height
            // editor is null while no document is open — the rail/inspector guard it,
            // and the welcome overlay covers everything.
            readonly property var editor: docContent.item ? docContent.item.editorItem : null
            LeftRail { id: rail; height: parent.height; editor: parent.editor; inspector: inspectorPanel }
            // The editor surface only exists while a document is open, so no binding
            // ever queries the empty model (which would index empty vectors → crash).
            // The Inspector is anchored OUTSIDE this Row (below) so it can float
            // in annotation mode; when it isn't floating, subtracting its width
            // here reproduces the classic push layout exactly.
            Loader {
                id: docContent
                active: blockModel.documentOpen
                width: parent.width - rail.width - (inspectorPanel.floating ? 0 : inspectorPanel.width)
                height: parent.height
                sourceComponent: Column {
                    anchors.fill: parent
                    property alias editorItem: editorInner
                    // The validated passive-surface editor (model owns the cursor; blocks
                    // are passive; overlay-drawn caret/selection). Ported from spike Arm C.
                    Editor { id: editorInner; width: parent.width; height: parent.height - innerTabs.height - innerBottom.height
                             inspector: inspectorPanel }
                    TableTabs { id: innerTabs; width: parent.width; editor: editorInner }
                    BottomRail { id: innerBottom; width: parent.width; editor: editorInner }
                }
            }
        }
    }

    // The right Inspector. Lives OUTSIDE the layout Row so annotation mode can
    // FLOAT it over the editor (opening the palette then can't push the locked
    // 760 column); when not floating, the Row's width subtraction above makes
    // it behave exactly like the old push layout.
    Inspector {
        id: inspectorPanel
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: parent.height - (docTabBar.visible ? docTabBar.height : 0)
        z: 5   // above the editor content; the welcome overlay (z:100) still covers
        editor: contentRow.editor
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
                    onClicked: docs.newTab()
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
                        color: recentMA.containsMouse ? Theme.colors.surfaceHover : Theme.colors.card
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
                            onClicked: win.openDoc(recentItem.modelData)
                        }
                    }
                }
            }
        }
    }

    // Toast — the app's outcome voice (Toasts singleton holds the state; a
    // new show() replaces the message and restarts the clock). Top-right
    // under the tab strip, above every overlay; click dismisses early.
    // Non-modal and non-focusable — it never interrupts typing.
    Rectangle {
        id: toastBox
        z: 300
        visible: opacity > 0
        opacity: Toasts.active ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
        anchors { top: parent.top; right: parent.right
                  topMargin: (docTabBar.visible ? docTabBar.height : 0) + 8; rightMargin: 12 }
        width: toastRow.implicitWidth + 24
        height: 32
        radius: 0
        color: Theme.colors.surfaceRaised
        border.width: 1; border.color: Theme.colors.divider
        Row {
            id: toastRow
            anchors.centerIn: parent
            spacing: 8
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: Toasts.kind === 2 ? "x-circle"
                    : Toasts.kind === 1 ? "warning" : "check-circle"
                size: 15
                color: Toasts.kind === 2 ? Theme.colors.error
                     : Toasts.kind === 1 ? Theme.colors.warn : Theme.colors.success
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: Toasts.message
                color: Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
            }
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                    onClicked: Toasts.dismiss() }
    }
}
