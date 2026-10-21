import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtCore
import Qt.labs.platform as Platform

ApplicationWindow {
    id: win
    minimumWidth: 640; minimumHeight: 480   // the toolbar row's budget (§2.14: 589 / 621 px with a stream chip) and the tallest flyout
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
    // Modal dialogs (the quit / close-tab prompts, conflicts, export options…) DARKEN the app behind them (user,
    // 2026-10-18): the window overlay's modal backdrop, one component for every modal popup. The flyouts opt out
    // with `dim: false`, so the canvas keeps its light while a menu is up.
    Component {
        id: modalDim
        Rectangle { color: Qt.rgba(0, 0, 0, 0.55) }
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
        nameFilters: ["minNotes documents (*.mnd *.mnpkg)"]
        onAccepted: win.openDoc(selectedFile)
    }
    FileDialog {
        id: saveAsDialog
        title: "Save As"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mnd"
        nameFilters: ["minNotes documents (*.mnd)"]
        onAccepted: {
            var f = "" + selectedFile
            if (blockModel.saveAs(f)) {
                Toasts.show(qsTr("Saved as ") + win.baseName(f))
                // Save As is the "make this doc self-contained" moment (user
                // request 2026-08-21): if externally referenced media remains
                // (NAS/absolute refs — the sidecar itself already copied),
                // offer the collect dialog. Silent when there's nothing.
                win.startCollectMedia(true)
            }
            else saveFailedDialog.open()   // was silently ignored before the toast pass
        }
    }
    // --- Import (File ▸ Import…, drop/paste intercept, welcome screen). A
    // single-document import lands in a FRESH untitled tab (preview before
    // commit — nothing touches disk until the user saves). The intercept:
    // dropping/pasting an importable file into the editor funnels through
    // blockModel.insertMediaFromUrl → importFileRequested (one C++ seam for
    // both paths) → this flow. ---
    FileDialog {
        id: importDialog
        title: "Import"
        // RTF rides the OS-native converter — ask the CLASSIFIER whether this
        // build supports it (macOS today; Windows lights up automatically
        // when RtfConvert_win lands) rather than hard-coding platforms.
        nameFilters: {
            var rtfOk = importer.formatFor("probe.rtf") !== ""
            var code = "*.py *.js *.ts *.jsx *.tsx *.c *.cc *.cpp *.cxx *.h *.hpp *.m *.mm"
                     + " *.cs *.java *.kt *.swift *.go *.rs *.rb *.php *.sh *.bash *.zsh"
                     + " *.sql *.json *.yaml *.yml *.toml *.xml *.ini *.css *.scss *.lua *.pl *.r *.cmake *.ps1"
            var all = "*.md *.markdown *.mdown *.txt *.text *.log *.csv *.tsv *.tab *.html *.htm"
                    + " *.docx *.xlsx *.odt *.ods *.enex *.zip " + code
            var f = []
            if (rtfOk) all += " *.rtf"
            f.push("Importable documents (" + all + ")")
            f = f.concat(["Markdown (*.md *.markdown *.mdown)",
                          "Plain text (*.txt *.text *.log)",
                          "CSV / TSV (*.csv *.tsv *.tab)",
                          "HTML (*.html *.htm)",
                          "Word document (*.docx)",
                          "Excel workbook (*.xlsx)",
                          "OpenDocument (*.odt *.ods)",
                          "Source code (" + code + ")",
                          "Evernote export (*.enex)",
                          "Notion export (*.zip)"])
            if (rtfOk) f.push("RTF (*.rtf)")
            return f
        }
        onAccepted: win.startImport("" + selectedFile)
    }
    // Multi-document imports (ENEX, Notion zips) write N .mnd files into a
    // chosen folder — the OS organizes (no vault) — then open the first.
    property string _pendingMultiImport: ""
    property string _pendingImportName: ""
    FolderDialog {
        id: importFolderDialog
        title: "Choose a folder for the imported documents"
        onAccepted: importer.startImportToFolder(win._pendingMultiImport, "" + selectedFolder)
    }
    // Imports run on a worker (parsing + media copies); the dialog returns
    // immediately and this popup owns the wait. Modal — which also pins the
    // fresh target tab open until the insert lands.
    Connections {
        target: importer
        function onImportFinished(ok, count, firstPath, error) {
            if (!ok) {
                if (error !== "cancelled")
                    Toasts.show(qsTr("Import failed") + (error !== "" ? " — " + error : ""), 2)
                return
            }
            if (firstPath !== "") {   // multi-doc: open the first, say how many
                Toasts.show(count === 1 ? qsTr("Imported 1 document")
                                        : qsTr("Imported %1 documents").arg(count))
                win.openDoc(firstPath)
            } else {
                Toasts.show(qsTr("Imported ") + win._pendingImportName)
            }
        }
    }
    // The cell cap (SR-4 S8f, R-I4 4a): a file whose tables hold over 100,000 cells asks — the
    // first rows that fit, the file attached as a chip, or nothing.
    Connections {
        target: importer
        function onImportNeedsDecision(cells, rows, keptRows, fileName) {
            importCapDialog.cells = cells; importCapDialog.rows = rows
            importCapDialog.keptRows = keptRows; importCapDialog.fileName = fileName
            importCapDialog.open()
        }
    }
    Popup {
        id: importCapDialog
        property int cells: 0
        property int rows: 0
        property int keptRows: 0
        property string fileName: ""
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
        width: 460; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text {
                text: qsTr("This table is very large")
                color: Theme.colors.textBright; font.family: Theme.font.family
                font.pixelSize: Theme.font.sizeBody; font.bold: true
            }
            Text {
                width: 420; wrapMode: Text.Wrap
                text: qsTr("%1 has %2 rows and %3 cells. Documents are for tables you read and edit; big data stays in spreadsheets. Import the first %4 rows, or attach the file to open it in its own app.")
                          .arg(importCapDialog.fileName).arg(importCapDialog.rows.toLocaleString(Qt.locale(), 'f', 0))
                          .arg(importCapDialog.cells.toLocaleString(Qt.locale(), 'f', 0)).arg(importCapDialog.keptRows.toLocaleString(Qt.locale(), 'f', 0))
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton { text: qsTr("Cancel"); padding: 12; onClicked: { importCapDialog.close(); importer.resolvePendingImport("cancel") } }
                FlatButton { text: qsTr("Attach file"); padding: 12; onClicked: { importCapDialog.close(); importer.resolvePendingImport("attach") } }
                FlatButton { text: qsTr("Import first %1 rows").arg(importCapDialog.keptRows.toLocaleString(Qt.locale(), 'f', 0)); padding: 12
                             onClicked: { importCapDialog.close(); importer.resolvePendingImport("rows") } }
            }
        }
    }
    Popup {
        id: importProgressDialog
        visible: importer.running
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
        width: 440; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text {
                text: qsTr("Importing…")
                color: Theme.colors.textBright; font.family: Theme.font.family
                font.pixelSize: Theme.font.sizeBody; font.bold: true
            }
            Text {
                width: 400; elide: Text.ElideMiddle
                text: importer.currentItem
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton {
                    text: qsTr("Cancel"); padding: 12
                    onClicked: importer.cancel()
                }
            }
        }
    }
    function startImport(fileUrl) {
        if (importer.isMultiDocument(fileUrl)) {
            win._pendingMultiImport = "" + fileUrl
            importFolderDialog.open()
            return
        }
        win._pendingImportName = win.importBaseName(fileUrl)
        docs.newTab()   // activeChanged re-points the importer synchronously
        importer.startImportFile(fileUrl)
    }
    function importBaseName(path) { return ("" + path).split("/").pop() }
    Connections {
        target: blockModel
        // Deferred: the signal fires synchronously from inside the editor's
        // drop/paste LOOP — importing immediately would newTab() and re-point
        // `blockModel` mid-loop (a mixed drop of an image + a .md would chain
        // rows into the wrong document). Fresh closures per file: Qt.callLater
        // only coalesces IDENTICAL functions.
        function onImportFileRequested(fileUrl) {
            Qt.callLater(function () {
                if (/\.mnpkg$/i.test("" + fileUrl)) win.openDoc(fileUrl)
                else win.startImport(fileUrl)
            })
        }
    }

    // --- Export (File ▸ Export as Markdown…). A pre-scan decides whether the
    // options dialog appears at all: options only exist when the document
    // carries the thing they govern (video notes, page ink) — the common case
    // goes straight to the save dialog. ---
    property var _exportScan: ({})
    property bool _exportNotes: true
    property bool _exportVideos: true     // mnpkg: videos default IN (copy, never
                                          // transcode — user ruling 2026-08-18;
                                          // the checkbox opts OUT for size)
    property string _exportFormat: "md"   // "md" | "html" | "docx" | "pdf" | "mnpkg" | "canvas" (HTML) | "poster" (PNG)
    function fmtBytes(b) {
        b = b || 0
        if (b >= 1e9) return (b / 1e9).toFixed(1) + " GB"
        if (b >= 1e6) return (b / 1e6).toFixed(0) + " MB"
        return Math.max(1, Math.round(b / 1e3)) + " KB"
    }
    function startExport(format) {
        _exportFormat = format || "md"
        _exportNotes = true
        _exportVideos = true
        if (_exportFormat === "canvas" || _exportFormat === "poster") { _openExportSave(); return }   // the canvas: straight to the file (ufb links follow the preference)
        if (_exportFormat === "mnpkg") {
            // Package pre-scan: the only option is detection-driven videos.
            _exportScan = packageExporter.scan()
            if ((_exportScan.videos || 0) > 0) exportOptionsDialog.open()
            else _openExportSave()
            return
        }
        _exportScan = exporter.scan()
        if ((_exportScan.videoNotes || 0) > 0 || (_exportScan.refPaths || 0) > 0)
            exportOptionsDialog.open()
        else
            _openExportSave()
    }
    // Seed the save dialog's filename on EVERY open: the dialog remembers
    // the last saved file, so "test.pdf" followed by a markdown export
    // offered "test.pdf.md" (bug 2026-08-20). Name = the document, suffix =
    // the current format; the remembered FOLDER is kept.
    function _openExportSave() {
        var name = (blockModel.documentName || "Untitled") + (_exportFormat === "canvas" ? "-canvas.html" : _exportFormat === "poster" ? "-poster.png" : "." + _exportFormat)
        exportSaveDialog.selectedFile = exportSaveDialog.currentFolder + "/" + name
        exportSaveDialog.open()
    }
    FileDialog {
        id: exportSaveDialog
        title: win._exportFormat === "html" ? "Export as HTML"
             : win._exportFormat === "docx" ? "Export as Word document"
             : win._exportFormat === "pdf" ? "Export as PDF"
             : win._exportFormat === "mnpkg" ? "Export as Package"
             : win._exportFormat === "canvas" ? "Export the canvas as HTML"
             : win._exportFormat === "poster" ? "Export the canvas poster"
             : "Export as Markdown"
        fileMode: FileDialog.SaveFile
        defaultSuffix: win._exportFormat === "canvas" ? "html" : win._exportFormat === "poster" ? "png" : win._exportFormat
        nameFilters: win._exportFormat === "html" || win._exportFormat === "canvas" ? ["HTML (*.html)"]
                   : win._exportFormat === "docx" ? ["Word document (*.docx)"]
                   : win._exportFormat === "pdf" ? ["PDF (*.pdf)"]
                   : win._exportFormat === "mnpkg" ? ["minNotes package (*.mnpkg)"]
                   : win._exportFormat === "poster" ? ["PNG image (*.png)"]
                   : ["Markdown (*.md)"]
        onAccepted: {
            var f = "" + selectedFile
            if (win._exportFormat === "canvas" || win._exportFormat === "poster") {
                var cok = win._exportFormat === "canvas" ? canvasExporter.exportHtml(f, true, exportPrefs.ufbLinks) : canvasExporter.exportPoster(f, 1600)
                if (cok) Toasts.show(qsTr("Exported ") + win.baseName(f) + qsTr(" in ") + win._folderName(f))
                else Toasts.show(qsTr("Export failed") + (canvasExporter.lastError ? " — " + canvasExporter.lastError : ""), 2)
                return
            }
            if (win._exportFormat === "mnpkg") {
                // Async: the zip assembles on a worker; the progress dialog
                // tracks it and the finished signal owns the toast.
                win._exportPkgName = win.baseName(f)
                packageExporter.startExport(f, win._exportVideos)
                return
            }
            var ok = win._exportFormat === "html"
                ? exporter.exportHtml(f, win._exportNotes, exportPrefs.ufbLinks)
                : win._exportFormat === "docx"
                ? exporter.exportDocx(f, win._exportNotes, exportPrefs.ufbLinks)
                : win._exportFormat === "pdf"
                ? exporter.exportPdf(f, win._exportNotes, exportPrefs.ufbLinks)
                : exporter.exportMarkdown(f, win._exportNotes, exportPrefs.ufbLinks)
            // The folder too (2026-09-15): the panel remembers its last folder, and an export that
            // landed elsewhere read as "the file wasn't overwritten".
            if (ok) Toasts.show(qsTr("Exported ") + win.baseName(f) + qsTr(" in ") + win._folderName(f))
            else    Toasts.show(qsTr("Export failed — ") + exporter.lastError, 2)
        }
    }
    // --- Package export progress (async worker; family dialog chrome). ---
    property string _exportPkgName: ""
    Connections {
        target: packageExporter
        function onExportFinished(ok, error) {
            if (ok) Toasts.show(qsTr("Exported ") + win._exportPkgName)
            else if (error === "Cancelled") Toasts.show(qsTr("Export cancelled"))
            else Toasts.show(qsTr("Export failed — ") + error, 2)
        }
    }
    Popup {
        id: exportProgressDialog
        visible: packageExporter.running
        modal: true
        closePolicy: Popup.NoAutoClose   // cancel is the only way out
        anchors.centerIn: Overlay.overlay
        width: 440; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text {
                text: qsTr("Exporting package…")
                color: Theme.colors.textBright; font.family: Theme.font.family
                font.pixelSize: Theme.font.sizeBody; font.bold: true
            }
            Text {
                width: 400; elide: Text.ElideMiddle
                text: packageExporter.currentItem
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            Rectangle {   // FlatSlider-background recipe: recessed track + fill
                width: 400; height: 6
                color: Theme.colors.surfaceRecess
                border.width: 1; border.color: Theme.colors.border
                Rectangle {
                    width: Math.round((parent.width - 2) * Math.min(1, packageExporter.progress))
                    height: parent.height - 2
                    x: 1; y: 1
                    color: Theme.colors.divider
                }
            }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton {
                    text: qsTr("Cancel"); padding: 12
                    onClicked: packageExporter.cancel()
                }
            }
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
                       : win._exportFormat === "docx" ? qsTr("Export as Word document")
                       : win._exportFormat === "pdf" ? qsTr("Export as PDF")
                       : win._exportFormat === "mnpkg" ? qsTr("Export as Package")
                       : qsTr("Export as Markdown")
                   color: Theme.colors.textBright; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody; font.bold: true }
            Row {   // mnpkg: detection-driven "Include videos" (default off — size)
                visible: win._exportFormat === "mnpkg" && (win._exportScan.videos || 0) > 0
                spacing: 8
                Rectangle {
                    width: 16; height: 16
                    anchors.verticalCenter: parent.verticalCenter
                    color: win._exportVideos ? Theme.colors.divider : "transparent"
                    border.width: 1
                    border.color: win._exportVideos ? Theme.colors.textBright : Theme.colors.border
                    Text { anchors.centerIn: parent; visible: win._exportVideos
                           text: "✓"; color: Theme.colors.textBright; font.pixelSize: 11 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: win._exportVideos = !win._exportVideos }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        var n = win._exportScan.videos || 0
                        return qsTr("Include videos (%1, %2)")
                            .arg(n).arg(win.fmtBytes(win._exportScan.videoBytes))
                    }
                    color: Theme.colors.text
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: win._exportVideos = !win._exportVideos }
                }
            }
            Text {
                visible: win._exportFormat === "mnpkg" && (win._exportScan.videos || 0) > 0
                width: 400; wrapMode: Text.Wrap
                text: qsTr("Videos left out stay as references to their original locations.")
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            Row {   // video-notes option — present only when notes were detected
                visible: win._exportFormat !== "mnpkg" && (win._exportScan.videoNotes || 0) > 0
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
            Row {   // ufb deep links — persisted preference, default OFF
                visible: win._exportFormat !== "mnpkg" && (win._exportScan.refPaths || 0) > 0
                spacing: 8
                Rectangle {   // squared family checkbox (PathMappings pattern)
                    width: 16; height: 16
                    anchors.verticalCenter: parent.verticalCenter
                    color: exportPrefs.ufbLinks ? Theme.colors.divider : "transparent"
                    border.width: 1
                    border.color: exportPrefs.ufbLinks ? Theme.colors.textBright : Theme.colors.border
                    Text { anchors.centerIn: parent; visible: exportPrefs.ufbLinks
                           text: "\u2713"; color: Theme.colors.textBright; font.pixelSize: 11 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: exportPrefs.ufbLinks = !exportPrefs.ufbLinks }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Include ufb links under file paths (%1 %2)")
                          .arg(win._exportScan.refPaths || 0)
                          .arg((win._exportScan.refPaths || 0) === 1 ? qsTr("path") : qsTr("paths"))
                    color: Theme.colors.text
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: exportPrefs.ufbLinks = !exportPrefs.ufbLinks }
                }
            }
            Text {
                visible: win._exportFormat !== "mnpkg" && (win._exportScan.refPaths || 0) > 0
                         && exportPrefs.ufbLinks
                width: 400; wrapMode: Text.Wrap
                text: qsTr("ufb:/// links open in the ufb browser and resolve across machines through its path mappings.")
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton { text: qsTr("Cancel"); padding: 12
                             onClicked: exportOptionsDialog.close() }
                FlatButton { text: qsTr("Export…"); variant: "primary"; padding: 12
                             onClicked: { exportOptionsDialog.close(); win._openExportSave() } }
            }
        }
    }

    // --- Collect Media (Document menu): copy every externally referenced
    // file into .minnotes and re-point the descriptors (one undo step).
    // Scan-first like export: nothing external → a toast, no dialog. ---
    property var _collectScan: ({})
    property bool _collectVideos: false
    // `quiet`: the post-Save-As offer — no toasts when there's nothing to
    // collect (or the doc is a package); only a real offer surfaces.
    function startCollectMedia(quiet) {
        _collectScan = mediaCollector.scan()
        if (_collectScan.package) {
            if (!quiet) Toasts.show(qsTr("Packages are sealed — use Save As to make an editable copy"), 2)
            return
        }
        if ((_collectScan.files || 0) + (_collectScan.videos || 0) === 0) {
            if (!quiet) Toasts.show(qsTr("All media already lives with this document"))
            return
        }
        _collectVideos = false
        collectOptionsDialog.open()
    }
    Connections {
        target: mediaCollector
        function onCollectFinished(ok, copied, error) {
            if (ok && copied > 0) {
                Toasts.show(qsTr("Collected %1 %2").arg(copied)
                            .arg(copied === 1 ? qsTr("file") : qsTr("files")))
                // Auto-save (ruling 2026-08-19): the copies are already on
                // disk, so an unsaved rewrite is the worst half-state — the
                // user reads "document stays whole", deletes the originals,
                // and an unsaved close reverts the refs to deleted files.
                // Saving closes the promise. Untitled docs skip (no home
                // yet — Save As re-homes the scratch .minnotes anyway);
                // _saveOrSaveAs brings the conflict/failure dialogs along.
                if (blockModel.documentOpen && !blockModel.untitled)
                    win._saveOrSaveAs()
            }
            else if (ok) Toasts.show(qsTr("Nothing to collect"))
            else if (error === "Cancelled") Toasts.show(qsTr("Collect cancelled"))
            else Toasts.show(qsTr("Collect failed — ") + error, 2)
        }
    }
    Dialog {
        id: collectOptionsDialog
        modal: true; anchors.centerIn: Overlay.overlay; width: 440; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text { width: 400; wrapMode: Text.Wrap
                   text: qsTr("Collect Media")
                   color: Theme.colors.textBright; font.family: Theme.font.family
                   font.pixelSize: Theme.font.sizeBody; font.bold: true }
            Text {
                width: 400; wrapMode: Text.Wrap
                text: {
                    var n = win._collectScan.files || 0
                    return qsTr("Copies %1 %2 (%3) referenced outside the document into its media folder, so the document stays whole if the originals move.")
                        .arg(n).arg(n === 1 ? qsTr("file") : qsTr("files"))
                        .arg(win.fmtBytes(win._collectScan.fileBytes || 0))
                }
                color: Theme.colors.text
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
            }
            Row {   // detection-driven videos opt-in (default off — size)
                visible: (win._collectScan.videos || 0) > 0
                spacing: 8
                Rectangle {   // squared family checkbox (export-dialog pattern)
                    width: 16; height: 16
                    anchors.verticalCenter: parent.verticalCenter
                    color: win._collectVideos ? Theme.colors.divider : "transparent"
                    border.width: 1
                    border.color: win._collectVideos ? Theme.colors.textBright : Theme.colors.border
                    Text { anchors.centerIn: parent; visible: win._collectVideos
                           text: "✓"; color: Theme.colors.textBright; font.pixelSize: 11 }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: win._collectVideos = !win._collectVideos }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        var n = win._collectScan.videos || 0
                        return qsTr("Include videos (%1, %2)")
                            .arg(n).arg(win.fmtBytes(win._collectScan.videoBytes || 0))
                    }
                    color: Theme.colors.text
                    font.family: Theme.font.family; font.pixelSize: Theme.font.sizeBody
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: win._collectVideos = !win._collectVideos }
                }
            }
            Text {
                visible: (win._collectScan.videos || 0) > 0
                width: 400; wrapMode: Text.Wrap
                text: qsTr("Videos left out stay as references to their original locations.")
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton { text: qsTr("Cancel"); padding: 12
                             onClicked: collectOptionsDialog.close() }
                FlatButton { text: qsTr("Collect"); variant: "primary"; padding: 12
                             enabled: (win._collectScan.files || 0) > 0 || win._collectVideos
                             onClicked: { collectOptionsDialog.close()
                                          mediaCollector.startCollect(win._collectVideos) } }
            }
        }
    }
    Popup {
        id: collectProgressDialog
        visible: mediaCollector.running
        modal: true
        closePolicy: Popup.NoAutoClose   // cancel is the only way out
        anchors.centerIn: Overlay.overlay
        width: 440; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text {
                text: qsTr("Collecting media…")
                color: Theme.colors.textBright; font.family: Theme.font.family
                font.pixelSize: Theme.font.sizeBody; font.bold: true
            }
            Text {
                width: 400; elide: Text.ElideMiddle
                text: mediaCollector.currentItem
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            Rectangle {   // FlatSlider-background recipe: recessed track + fill
                width: 400; height: 6
                color: Theme.colors.surfaceRecess
                border.width: 1; border.color: Theme.colors.border
                Rectangle {
                    width: Math.round((parent.width - 2) * Math.min(1, mediaCollector.progress))
                    height: parent.height - 2
                    x: 1; y: 1
                    color: Theme.colors.divider
                }
            }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton {
                    text: qsTr("Cancel"); padding: 12
                    onClicked: mediaCollector.cancel()
                }
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
    // Persisted export preferences (2026-08-21): ufb deep links under file
    // paths — off by default; they only mean something to ufb users.
    Settings { id: exportPrefs; category: "export"; property bool ufbLinks: false }
    // Spell / grammar (0.5.0): the two toggles + the ignored-rule list persist
    // here; the service holds the live values and mirrors them back on change.
    Settings {
        id: spellPrefs; category: "spelling"
        property bool checkSpelling: true
        property bool checkGrammar: true
        property string ignoredRules: "[]"
        Component.onCompleted: {
spell.checkSpelling = checkSpelling; spell.checkGrammar = checkGrammar; spell.ignoredRulesJson = ignoredRules }
    }
    Connections {
        target: spell
        function onCheckSpellingChanged() { spellPrefs.checkSpelling = spell.checkSpelling }
        function onCheckGrammarChanged()  { spellPrefs.checkGrammar  = spell.checkGrammar }
        function onIgnoredRulesChanged()  { spellPrefs.ignoredRules  = spell.ignoredRulesJson }
    }
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
    function baseName(path) { var n = ("" + path).split("/").pop(); return n.replace(/\.(mnd|mnpkg)$/i, "") }
    // The enclosing folder's name ("Desktop"), from a path or file URL.
    function _folderName(path) { var p = ("" + path).replace(/^file:\/\//, "").split("/"); p.pop(); return decodeURIComponent(p.pop() || "/") }
    function removeRecent(path) {
        recentsStore.paths = JSON.stringify(
            recentPaths().filter(function (p) { return p !== path }))
    }
    // Open with failure feedback. A dead recents entry (the file was moved,
    // renamed, or deleted) used to fail SILENTLY — indistinguishable from the
    // app just not opening anything. Now it says so and prunes the entry.
    property string openFailedPath: ""
    property string openFailedReason: ""   // set when the file was refused (e.g. an earlier minNotes)
    function openDoc(path) {
        path = "" + path
        if (docs.openTab(path)) {
            // Packages are sealed snapshots: you're viewing, and persistence
            // goes through Save As (untitled semantics under the hood).
            if (/\.mnpkg$/i.test(path))
                Toasts.show(qsTr("Viewing a package — saving creates a new document"))
            else if (blockModel.readOnly)   // a 1.0 file, converted on open: the original is untouched
                Toasts.show(blockModel.conversionSummary() + qsTr(" — Save As to keep it"))
            return
        }
        openFailedPath = path
        openFailedReason = docs.lastOpenError()
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
                   text: (win.openFailedReason !== ""
                              ? win.openFailedReason + "\n"
                              : "The file may have been moved, renamed, or deleted:\n")
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
    // The probes' "live card" bootstrap (SPIKE S5): the canvas up, fitted, the first page live in its card.
    function _goLiveForProbe() {
        const ed = win._editor()
        if (!ed || !ed.canvasItem) { Qt.callLater(win._goLiveForProbe); return }
        ed.setActiveTab("canvas")
        ed.canvasItem.fit()
        ed.canvasItem.goLive(canvas.firstPageId())
        console.log("PROBE live card:", canvas.livePage !== "" ? "armed" : "FAILED")
    }
    readonly property var activeEditor: docContent.item ? docContent.item.editorItem : null   // the reactive form (menu checks)
    property var activeCanvasView: null   // the hoisted CanvasView (the search bar jumps through it)
    function findInFile() { if (chromeRows.item && chromeRows.item.searchBar) chromeRows.item.searchBar.toggle() }
    function _restoreActiveView() {
        var ed = win._editor()
        if (ed && docs.activeIndex >= 0)
            ed.restoreViewState(docs.viewState(docs.tabIdAt(docs.activeIndex)))
    }
    // A document dropped on the CANVAS: a new page at the drop point takes the import (the importer
    // follows the focused page; the focus returns to the canvas in the same turn, so no page view shows).
    Connections {
        target: canvas
        function onImportFileRequested(fileUrl, wx, wy) {
            Qt.callLater(function () {
                const id = canvas.addPageAt(wx, wy)
                if (id === "") return
                canvas.focusPage = id
                importer.importFile(fileUrl)
                canvas.focusPage = ""
            })
        }
        // ENEX / Notion dropped on the canvas (phase A8): one page per note, a grid at the drop point.
        function onImportMultiDocRequested(fileUrl, wx, wy) {
            Qt.callLater(function () {
                const r = importer.importMultiDocToCanvas(fileUrl, canvas, wx, wy)
                if (r.ok) Toasts.show(r.count === 1 ? qsTr("Imported 1 note") : qsTr("Imported %1 notes").arg(r.count))
                else Toasts.show(qsTr("Import failed") + (r.error ? " — " + r.error : ""), 2)
            })
        }
        function onImportNotice(text) { Toasts.show(text) }
    }
    // A focus change INSIDE the active canvas (page ↔ canvas, page → page) re-points blockModel
    // (main.cpp, synchronously, before this fires) without a tab switch: a fresh page view.
    Connections {
        target: docs
        function onActiveFocusChanged() { var ed = win._editor(); if (ed) ed.resetForPage() }
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

    // --- Merging a file into this page (2026-10-18: the tab-merge DRAG went with the tab strip; File ▸ Merge File
    // Into This Page… and the block menu's "Merge a file here…" pick the source from the open files). ---
    property string _mergeSrcName: ""
    property real _mergeBytes: 0
    property int _mergeGap: -1          // the gap the picker merges at; -1 = the end
    function openMergePicker(gap) {
        if (docs.count < 2) { Toasts.show(qsTr("Open another file to merge it in")); return }
        var ed = win._editor()
        if (ed && ed.activeFrameId !== "") ed.setActiveTab("")   // the merge lands in the Document view
        win._mergeGap = gap
        mergePicker.open()
    }
    function mergeFileAt(index) {
        var src = docs.modelAt(index), dest = docs.activeModel
        if (!src || !dest || src === dest) return
        var gap = win._mergeGap >= 0 ? Math.min(win._mergeGap, dest.count) : dest.count
        win._startMerge(src, dest, gap)
    }
    function _startMerge(srcModel, destModel, gap) {
        win._mergeSrcName = srcModel.documentName
        var s = merger.scan(srcModel, destModel)   // popup gate: worth a bar?
        win._mergeBytes = (s.fileBytes || 0) + (s.videoBytes || 0)
        merger.startMerge(srcModel, destModel, gap)
    }
    Connections {
        target: merger
        function onMergeFinished(ok, first, last, error) {
            if (!ok) {
                Toasts.show(error === "Cancelled" ? qsTr("Merge cancelled")
                                                  : qsTr("Merge failed — ") + error, 2)
                return
            }
            if (first < 0) { Toasts.show(qsTr("Nothing to merge")); return }
            var ed = win._editor()
            if (ed) ed.ensureVisible(first)
            var undoKey = Qt.platform.os === "osx" ? "⌘Z" : "Ctrl+Z"
            Toasts.show(qsTr("Merged “%1” — %2 %3 (%4 undoes)")
                        .arg(win._mergeSrcName).arg(last - first + 1)
                        .arg(last === first ? qsTr("block") : qsTr("blocks"))
                        .arg(undoKey))
        }
    }
    // The rich-paste engine copies assets on a worker only when they cross
    // documents; the bar shows once a copy has run for a moment (a few MB
    // finish before the popup would even paint).
    property bool _pasteBar: false
    Timer { interval: 300; running: paster.running; onTriggered: win._pasteBar = true }
    Connections { target: paster; function onRunningChanged() { if (!paster.running) win._pasteBar = false } }
    Popup {   // merge / paste copy progress — the collect chrome; only for real copies
        id: transferPopup
        readonly property bool pasting: paster.running && win._pasteBar
        visible: (merger.running && win._mergeBytes > 16 * 1024 * 1024) || pasting
        modal: true
        closePolicy: Popup.NoAutoClose   // cancel is the only way out
        anchors.centerIn: Overlay.overlay
        width: 440; padding: 20
        background: Rectangle { color: Theme.colors.surface; radius: 0
                                border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            spacing: 14
            Text {
                text: transferPopup.pasting ? qsTr("Pasting media…")
                                            : qsTr("Merging “%1”…").arg(win._mergeSrcName)
                color: Theme.colors.textBright; font.family: Theme.font.family
                font.pixelSize: Theme.font.sizeBody; font.bold: true
            }
            Text {
                width: 400; elide: Text.ElideMiddle
                text: transferPopup.pasting ? paster.currentItem : merger.currentItem
                color: Theme.colors.textMuted
                font.family: Theme.font.family; font.pixelSize: Theme.font.sizeSmall
            }
            Rectangle {   // FlatSlider-background recipe: recessed track + fill
                width: 400; height: 6
                color: Theme.colors.surfaceRecess
                border.width: 1; border.color: Theme.colors.border
                Rectangle {
                    width: Math.round((parent.width - 2) * Math.min(1, transferPopup.pasting ? paster.progress : merger.progress))
                    height: parent.height - 2
                    x: 1; y: 1
                    color: Theme.colors.divider
                }
            }
            Row {
                spacing: 8; anchors.right: parent.right
                FlatButton { text: qsTr("Cancel"); padding: 12
                             onClicked: { if (paster.running) paster.cancel(); else merger.cancel() } }
            }
        }
    }
    Popup {   // the merge picker: which open file to copy into this page
        id: mergePicker
        anchors.centerIn: Overlay.overlay
        width: 360; padding: 1
        modal: true; dim: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.colors.surface; radius: 0; border.width: 1; border.color: Theme.colors.border }
        contentItem: Column {
            width: 358
            Text { text: qsTr("Merge into this page"); leftPadding: 12; topPadding: 10; bottomPadding: 6
                   color: Theme.colors.textBright; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome; font.bold: true }
            Repeater {
                model: docs.models
                delegate: Rectangle {
                    required property int index
                    required property var modelData
                    readonly property bool self: index === docs.activeIndex
                    width: 358; height: self ? 0 : 28; visible: !self; radius: 0
                    color: mpMA.containsMouse ? Theme.colors.surfaceHover : "transparent"
                    Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; anchors.leftMargin: 12; anchors.right: parent.right; anchors.rightMargin: 10
                           text: modelData.documentName; elide: Text.ElideMiddle
                           color: Theme.colors.text; font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome }
                    MouseArea { id: mpMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { mergePicker.close(); win.mergeFileAt(parent.index) } }
                }
            }
            Item { width: 1; height: 6 }
        }
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
                Platform.MenuItem { text: qsTr("Import…"); role: Platform.MenuItem.NoRole; onTriggered: importDialog.open() }
                Platform.MenuItem { text: qsTr("Merge File Into This Page…"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen && docs.count > 1; onTriggered: win.openMergePicker(-1) }
                Platform.MenuSeparator {}
                Platform.MenuItem { text: qsTr("Save");    shortcut: StandardKey.Save;   enabled: blockModel.documentOpen; onTriggered: win._saveOrSaveAs() }
                Platform.MenuItem { text: qsTr("Save As…"); shortcut: StandardKey.SaveAs; enabled: blockModel.documentOpen; onTriggered: saveAsDialog.open() }
                Platform.MenuSeparator {}
                Platform.MenuItem { text: qsTr("Close"); shortcut: StandardKey.Close; enabled: blockModel.documentOpen; onTriggered: win.requestCloseTab(docs.activeIndex) }
            }
            // Menu split (2026-08-19): File / Export / Document / Help — one
            // File menu had grown every command in the app. NoRole everywhere
            // a non-standard title could trip macOS TextHeuristicRole
            // relocation (see the updater note on the Help menu).
            Platform.Menu {
                title: qsTr("Export")
                Platform.MenuItem { text: qsTr("as Markdown…"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen; onTriggered: win.startExport("md") }
                Platform.MenuItem { text: qsTr("as HTML…"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen; onTriggered: win.startExport("html") }
                Platform.MenuItem { text: qsTr("as Word Document…"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen; onTriggered: win.startExport("docx") }
                Platform.MenuItem { text: qsTr("as PDF…"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen; onTriggered: win.startExport("pdf") }
                Platform.MenuItem { text: qsTr("as Package…"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen; onTriggered: win.startExport("mnpkg") }
                Platform.MenuSeparator {}
                Platform.MenuItem { text: qsTr("Canvas as HTML…"); role: Platform.MenuItem.NoRole; enabled: canvas.documentOpen; onTriggered: win.startExport("canvas") }
                Platform.MenuItem { text: qsTr("Canvas Poster (PNG)…"); role: Platform.MenuItem.NoRole; enabled: canvas.documentOpen; onTriggered: win.startExport("poster") }
            }
            Platform.Menu {   // Edit (2.0.1): undo / redo of the surface in front (the page's log or the canvas's) and Find. Undo / Redo
                              // carry NO key equivalent on purpose: a native one would take ⌘Z before the search field or a chip's TextEdit sees it.
                title: qsTr("Edit")
                Platform.MenuItem { text: qsTr("Undo"); role: Platform.MenuItem.NoRole; enabled: !!win.activeEditor && win.activeEditor.undoTarget.canUndo
                                    onTriggered: { var ed = win._editor(); if (ed) ed.undoTarget.undo() } }
                Platform.MenuItem { text: qsTr("Redo"); role: Platform.MenuItem.NoRole; enabled: !!win.activeEditor && win.activeEditor.undoTarget.canRedo
                                    onTriggered: { var ed = win._editor(); if (ed) ed.undoTarget.redo() } }
                Platform.MenuSeparator {}
                Platform.MenuItem { text: qsTr("Find in File…"); role: Platform.MenuItem.NoRole; shortcut: StandardKey.Find; enabled: blockModel.documentOpen
                                    onTriggered: win.findInFile() }
            }
            Platform.Menu {
                title: qsTr("Document")
                Platform.MenuItem { text: qsTr("Check Spelling"); role: Platform.MenuItem.NoRole; checkable: true
                                    checked: spell.checkSpelling; onTriggered: spell.checkSpelling = !spell.checkSpelling }
                Platform.MenuItem { text: qsTr("Check Grammar"); role: Platform.MenuItem.NoRole; checkable: true
                                    checked: spell.checkGrammar; onTriggered: spell.checkGrammar = !spell.checkGrammar }
                Platform.MenuItem { text: qsTr("Reset Ignored Rules"); role: Platform.MenuItem.NoRole
                                    enabled: spell.ignoredRulesJson !== "[]"; onTriggered: spell.ignoredRulesJson = "[]" }
                Platform.MenuSeparator {}
                Platform.MenuItem { text: qsTr("Copy as Markdown"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen
                                    onTriggered: { clipboard.writeText(exporter.copyMarkdown(-1, -1)); Toasts.show(qsTr("Copied document as Markdown")) } }
                Platform.MenuItem { text: qsTr("Insert Choice Chip"); role: Platform.MenuItem.NoRole
                                    shortcut: "Ctrl+Alt+C"; enabled: blockModel.documentOpen
                                    onTriggered: { var ed = win._editor(); if (ed) ed.insertChoiceChip() } }
                Platform.MenuSeparator {}
                Platform.MenuItem { text: qsTr("Collect Media…"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen; onTriggered: win.startCollectMedia() }
                Platform.MenuItem { text: qsTr("Reveal Media Folder"); role: Platform.MenuItem.NoRole; enabled: blockModel.documentOpen; onTriggered: blockModel.revealMediaFolder() }
                Platform.MenuSeparator {}
                Platform.MenuItem { text: qsTr("Path Mappings…"); role: Platform.MenuItem.NoRole; onTriggered: pathMappingsDialog.open2() }
            }
            // View (2.0 stage 1C): the page is as wide as the window under a readable measure;
            // these are the two VIEW preferences behind it. ⌘1 lives in the editor (the zoom badge
            // teaches it), so the items carry no shortcut of their own.
            Platform.Menu {
                title: qsTr("View")
                Platform.MenuItem { text: qsTr("Full Width"); role: Platform.MenuItem.NoRole; checkable: true
                                    enabled: blockModel.documentOpen
                                    checked: win.activeEditor ? win.activeEditor.fullWidth : false
                                    onTriggered: { var ed = win._editor(); if (ed) ed.toggleFullWidth() } }
                Platform.MenuSeparator {}
                Platform.MenuItemGroup { id: measureGroup }
                Platform.MenuItem { text: qsTr("Measure 640"); role: Platform.MenuItem.NoRole; checkable: true; group: measureGroup
                                    enabled: blockModel.documentOpen; checked: !!win.activeEditor && win.activeEditor.measureCap === 640
                                    onTriggered: { var ed = win._editor(); if (ed) ed.setMeasureCap(640) } }
                Platform.MenuItem { text: qsTr("Measure 760"); role: Platform.MenuItem.NoRole; checkable: true; group: measureGroup
                                    enabled: blockModel.documentOpen; checked: !!win.activeEditor && win.activeEditor.measureCap === 760
                                    onTriggered: { var ed = win._editor(); if (ed) ed.setMeasureCap(760) } }
                Platform.MenuItem { text: qsTr("Measure 880"); role: Platform.MenuItem.NoRole; checkable: true; group: measureGroup
                                    enabled: blockModel.documentOpen; checked: !!win.activeEditor && win.activeEditor.measureCap === 880
                                    onTriggered: { var ed = win._editor(); if (ed) ed.setMeasureCap(880) } }
            }
            Platform.Menu {
                title: qsTr("Help")
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
                Platform.MenuItem {
                    text: qsTr("Third-Party Notices")
                    role: Platform.MenuItem.NoRole
                    onTriggered: Qt.openUrlExternally("https://minnotes.app/licenses/")
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
                Action { text: qsTr("&Import…"); onTriggered: importDialog.open() }
                Action { text: qsTr("&Merge File Into This Page…"); enabled: blockModel.documentOpen && docs.count > 1; onTriggered: win.openMergePicker(-1) }
                ThemedMenuSeparator {}
                Action { text: qsTr("&Save");    shortcut: StandardKey.Save;   enabled: blockModel.documentOpen; onTriggered: win._saveOrSaveAs() }
                Action { text: qsTr("Save &As…"); shortcut: StandardKey.SaveAs; enabled: blockModel.documentOpen; onTriggered: saveAsDialog.open() }
                ThemedMenuSeparator {}
                Action { text: qsTr("&Close"); shortcut: StandardKey.Close; enabled: blockModel.documentOpen; onTriggered: win.requestCloseTab(docs.activeIndex) }
            }
            // Menu split (2026-08-19): File / Export / Document / Help — the
            // exact macOS structure above.
            ThemedMenu {
                title: qsTr("&Export")
                Action { text: qsTr("as &Markdown…"); enabled: blockModel.documentOpen; onTriggered: win.startExport("md") }
                Action { text: qsTr("as &HTML…"); enabled: blockModel.documentOpen; onTriggered: win.startExport("html") }
                Action { text: qsTr("as &Word Document…"); enabled: blockModel.documentOpen; onTriggered: win.startExport("docx") }
                Action { text: qsTr("as P&DF…"); enabled: blockModel.documentOpen; onTriggered: win.startExport("pdf") }
                Action { text: qsTr("as &Package…"); enabled: blockModel.documentOpen; onTriggered: win.startExport("mnpkg") }
                MenuSeparator {}
                Action { text: qsTr("&Canvas as HTML…"); enabled: canvas.documentOpen; onTriggered: win.startExport("canvas") }
                Action { text: qsTr("Canvas P&oster (PNG)…"); enabled: canvas.documentOpen; onTriggered: win.startExport("poster") }
            }
            ThemedMenu {   // Edit (2.0.1): see the macOS menu — Undo / Redo without a bound shortcut (the editor routes ⌘Z itself)
                title: qsTr("&Edit")
                Action { text: qsTr("&Undo"); enabled: !!win.activeEditor && win.activeEditor.undoTarget.canUndo; onTriggered: { var ed = win._editor(); if (ed) ed.undoTarget.undo() } }
                Action { text: qsTr("&Redo"); enabled: !!win.activeEditor && win.activeEditor.undoTarget.canRedo; onTriggered: { var ed = win._editor(); if (ed) ed.undoTarget.redo() } }
                MenuSeparator {}
                Action { text: qsTr("&Find in File…"); shortcut: StandardKey.Find; enabled: blockModel.documentOpen; onTriggered: win.findInFile() }
            }
            ThemedMenu {
                title: qsTr("&Document")
                Action { text: qsTr("Check &Spelling"); checkable: true; checked: spell.checkSpelling
                         onToggled: spell.checkSpelling = checked }
                Action { text: qsTr("Check &Grammar"); checkable: true; checked: spell.checkGrammar
                         onToggled: spell.checkGrammar = checked }
                Action { text: qsTr("Reset &Ignored Rules"); enabled: spell.ignoredRulesJson !== "[]"
                         onTriggered: spell.ignoredRulesJson = "[]" }
                ThemedMenuSeparator {}
                Action { text: qsTr("Copy as &Markdown"); enabled: blockModel.documentOpen
                         onTriggered: { clipboard.writeText(exporter.copyMarkdown(-1, -1)); Toasts.show(qsTr("Copied document as Markdown")) } }
                Action { text: qsTr("&Insert Choice Chip"); enabled: blockModel.documentOpen
                         onTriggered: { var ed = win._editor(); if (ed) ed.insertChoiceChip() } }
                ThemedMenuSeparator {}
                Action { text: qsTr("&Collect Media…"); enabled: blockModel.documentOpen; onTriggered: win.startCollectMedia() }
                Action { text: qsTr("&Reveal Media Folder"); enabled: blockModel.documentOpen; onTriggered: blockModel.revealMediaFolder() }
                ThemedMenuSeparator {}
                Action { text: qsTr("Path &Mappings…"); onTriggered: pathMappingsDialog.open2() }
            }
            ThemedMenu {
                title: qsTr("&View")
                Action { text: qsTr("&Full Width"); checkable: true; enabled: blockModel.documentOpen
                         checked: win.activeEditor ? win.activeEditor.fullWidth : false
                         onToggled: { var ed = win._editor(); if (ed && ed.fullWidth !== checked) ed.toggleFullWidth() } }
                ThemedMenuSeparator {}
                ActionGroup { id: measureActions; exclusive: true }
                Action { text: qsTr("Measure 640"); checkable: true; ActionGroup.group: measureActions; enabled: blockModel.documentOpen
                         checked: !!win.activeEditor && win.activeEditor.measureCap === 640
                         onTriggered: { var ed = win._editor(); if (ed) ed.setMeasureCap(640) } }
                Action { text: qsTr("Measure 760"); checkable: true; ActionGroup.group: measureActions; enabled: blockModel.documentOpen
                         checked: !!win.activeEditor && win.activeEditor.measureCap === 760
                         onTriggered: { var ed = win._editor(); if (ed) ed.setMeasureCap(760) } }
                Action { text: qsTr("Measure 880"); checkable: true; ActionGroup.group: measureActions; enabled: blockModel.documentOpen
                         checked: !!win.activeEditor && win.activeEditor.measureCap === 880
                         onTriggered: { var ed = win._editor(); if (ed) ed.setMeasureCap(880) } }
            }
            ThemedMenu {
                title: qsTr("&Help")
                Action { text: qsTr("Check for &Updates…"); onTriggered: appUpdater.checkForUpdates() }
                Action { text: qsTr("&Third-Party Notices"); onTriggered: Qt.openUrlExternally("https://minnotes.app/licenses/") }
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
    // Dev-only (SR-2): --pool-probe=<file.md> imports the fixture into a fresh
    // tab at a fixed size; Editor.qml's poolProbe drives the run and exits.
    // Never touches the saved window geometry.
    readonly property string _poolProbeFixture: {
        var a = Qt.application.arguments
        for (var i = 0; i < a.length; ++i)
            if (a[i].indexOf("--pool-probe=") === 0) return a[i].substring(13)
        return ""
    }
    // Dev-only (A0 speed programme, 2026-10-16): --type-probe=<file.md> imports the fixture into
    // a fresh tab at a fixed size; Editor.qml's typeProbe types into it, times every keystroke
    // and exits. main.cpp sets MN_TYPE_PROBE for it so the model records its splits.
    readonly property string _typeProbeFixture: {
        var a = Qt.application.arguments
        for (var i = 0; i < a.length; ++i)
            if (a[i].indexOf("--type-probe=") === 0) return a[i].substring(13)
        return ""
    }
    // Dev-only (canvas B10): --canvas-probe=<md> imports the fixture and runs the canvas probe (Editor.qml).
    readonly property string _videoProbeClip: {
        var a = Qt.application.arguments
        for (var i = 0; i < a.length; ++i)
            if (a[i].indexOf("--video-probe=") === 0) return a[i].substring(14)
        return ""
    }
    readonly property string _canvasProbeFixture: {
        var a = Qt.application.arguments
        for (var i = 0; i < a.length; ++i)
            if (a[i].indexOf("--canvas-probe=") === 0) return a[i].substring(15)
        return ""
    }
    // Dev-only (2026-09-16): --import=<file> opens a fresh tab with the file IMPORTED (markdown,
    // docx, …) — pairs with --shot / --export-html for calibration samples written as markdown.
    readonly property string _importFixture: {
        var a = Qt.application.arguments
        for (var i = 0; i < a.length; ++i)
            if (a[i].indexOf("--import=") === 0) return a[i].substring(9)
        return ""
    }
    Component.onCompleted: {
        Overlay.overlay.modal = modalDim; Overlay.overlay.modeless = null   // the modal backdrop (see modalDim above)
        if (_poolProbeFixture !== "") {
            width = 1200; height = 900
            docs.newTab()
            if (!importer.importFile(_poolProbeFixture)) { console.log("POOL-PROBE FAIL import"); Qt.exit(101) }
            return
        }
        if (_typeProbeFixture !== "") {
            width = 1200; height = 900
            docs.newTab()
            if (!importer.importFile(_typeProbeFixture)) { console.log("TYPE-PROBE FAIL import"); Qt.exit(101) }
            // --live-card (SPIKE S5): the same probe, typing into the page made LIVE in its card on the canvas.
            if (Qt.application.arguments.indexOf("--live-card") >= 0) Qt.callLater(win._goLiveForProbe)
            return
        }
        if (_canvasProbeFixture !== "") {
            width = 1200; height = 900
            docs.newTab()
            if (!importer.importFile(_canvasProbeFixture)) { console.log("CANVAS-PROBE FAIL import"); Qt.exit(101) }
            return
        }
        if (_videoProbeClip !== "") {   // --video-probe=<clip>: a fresh tab with the clip as a block (Editor.qml drives the studio + a canvas element)
            width = 1200; height = 900
            docs.newTab()
            blockModel.insertVideoFromUrl(0, _videoProbeClip)
            return
        }
        if (_importFixture !== "") {
            width = 1200; height = 900
            docs.newTab()
            if (!importer.importFile(_importFixture)) { console.log("IMPORT FAIL"); Qt.exit(101) }
            return
        }
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
        if (!_geomRestored || _poolProbeFixture !== "" || _typeProbeFixture !== "") return
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

        // Rows 2 + 3 of the top chrome (2026-10-18, §2.14): the COMPONENTS row (pages / elements / frames — the
        // former bottom strip) and, from S3, the TOOLBAR. A Loader like docContent: nothing here touches the
        // empty model while no document is open.
        Loader {
            id: chromeRows
            width: parent.width
            active: blockModel.documentOpen
            // NO explicit height: a Loader with an explicit size resizes its ITEM to itself (the rows collapsed to 0
            // and the content slid up under them); unset, the Loader takes the item's height. Width only is fine —
            // the two dimensions are independent.
            sourceComponent: Column {
                property alias tabs: pathBar
                property alias toolbar: chromeToolbar
                property alias searchBar: searchBar
                PathBar { id: pathBar; width: parent.width; editor: win.activeEditor }
                ChromeToolbar { id: chromeToolbar; width: parent.width; editor: win.activeEditor; inspector: inspectorPanel }
                SearchBar { id: searchBar; width: parent.width; editor: win.activeEditor; canvasView: win.activeCanvasView }   // ⌘F (v2.0.1)
            }
        }

        Row {
            id: contentRow
            width: parent.width
            height: parent.height - chromeRows.height
            // editor is null while no document is open — the rail/inspector guard it,
            // and the welcome overlay covers everything.
            readonly property var editor: docContent.item ? docContent.item.editorItem : null
            // The editor surface only exists while a document is open, so no binding
            // ever queries the empty model (which would index empty vectors → crash).
            // The Inspector is anchored OUTSIDE this Row (below) so it can float
            // in annotation mode; when it isn't floating, subtracting its width
            // here reproduces the classic push layout exactly.
            Loader {
                id: docContent
                active: blockModel.documentOpen
                width: parent.width - (inspectorPanel.floating ? 0 : inspectorPanel.width)
                height: parent.height
                sourceComponent: Column {
                    anchors.fill: parent
                    property alias editorItem: editorInner
                    // The document surface (2.0 SPIKE S1): the ONE Editor in its home slot, the CanvasView a sibling
                    // OVER it in the same rect (shown when the canvas is up; the Editor stays visible underneath as
                    // the key sink, as it was when the canvas was its child). A live card reparents the Editor into
                    // the canvas and back to `editorSlot` (S4).
                    Item {
                        id: surface
                        width: parent.width
                        height: parent.height
                        clip: true   // the canvas's elements (and a selection frame's glyph) may extend past the view: the chrome rows above must still take their clicks (2026-10-18)
                        Item {
                            id: editorSlot
                            anchors.fill: parent
                            // The validated passive-surface editor (model owns the cursor; blocks
                            // are passive; overlay-drawn caret/selection). Ported from spike Arm C.
                            Editor { id: editorInner; anchors.fill: parent
                                     homeSlot: editorSlot; canvasItem: canvasView
                                     inspector: inspectorPanel
                                     tabsItem: chromeRows.item ? chromeRows.item.tabs : null
                                     toolbarItem: chromeRows.item ? chromeRows.item.toolbar.canvasTools : null
                                     chromeItem: chromeRows.item ? chromeRows.item.toolbar : null
                                     searchItem: chromeRows.item ? chromeRows.item.searchBar : null
                                     onMergeFileRequested: (gap) => win.openMergePicker(gap) }
                        }
                        CanvasView { id: canvasView; anchors.fill: parent; visible: editorInner.canvasShown; editor: editorInner
                                     Component.onCompleted: win.activeCanvasView = canvasView; Component.onDestruction: if (win.activeCanvasView === canvasView) win.activeCanvasView = null }
                    }
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
        height: parent.height - chromeRows.height
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

        // Drop a document right onto the welcome screen: .mnd opens, any
        // importable format (importer.formatFor) imports into a fresh tab.
        DropArea {
            id: welcomeDrop
            anchors.fill: parent
            onDropped: function (drop) {
                if (!drop.hasUrls) return
                for (var i = 0; i < drop.urls.length; ++i) {
                    var u = "" + drop.urls[i]
                    if (/\.(mnd|mnpkg)$/i.test(u)) win.openDoc(u)
                    else if (importer.formatFor(u) !== "") win.startImport(u)
                }
            }
        }
        Rectangle {
            anchors.fill: parent
            visible: welcomeDrop.containsDrag
            color: "transparent"
            border.width: 2; border.color: Theme.colors.accent
        }

        Column {
            anchors.centerIn: parent
            width: 440
            spacing: 22

            Column {
                width: parent.width; spacing: 4
                Text { text: "minNotes"; color: Theme.colors.textBright
                       font.family: Theme.font.family; font.pixelSize: 34; font.bold: true }
                Text { text: "Create a new document, open one, or drop a file here to import."
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
                  topMargin: chromeRows.height + 8; rightMargin: 12 }
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
