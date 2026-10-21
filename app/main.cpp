#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSurfaceFormat>
#include <QColorSpace>
#include <QStyleHints>
#include <QTimer>
#include <QQuickWindow>
#include "KeyLatencyProbe.h"
#include <QTextStream>
#include <QPdfDocument>
#include <QPainter>
#include <QUrl>
#include <QLocalServer>      // Windows single-instance: primary listens,
#include <QLocalSocket>      // secondary forwards its file/URI args here
#include <QLockFile>         // the per-user gate that elects the primary
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QWindow>           // raise the existing window on a secondary launch
#include "MinNotesApplication.h"
#include "WindowChrome.h"
#include "AppUpdater.h"
#include "sparkle_updater_macos.h"
#include "core/BlockModel.h"
#include "core/DocumentManager.h"
#include "core/CanvasDocument.h"
#include "CanvasGestures.h"
#include "core/Exporter.h"
#include "core/CanvasExporter.h"
#include "core/SearchEngine.h"
#include "core/Importer.h"
#include "core/PackageExporter.h"
#include "core/DocumentMerger.h"
#include "core/PathMapController.h"
#include "core/ViewportSlots.h"
#include "core/Clipboard.h"
#include "core/ClipboardPaster.h"
#include "spell/SpellService.h"
#include "core/VideoFrameProvider.h"
#include "core/PdfPageProvider.h"

// Resolve a file path / file:// URL / minnotes:// deep link to a document path
// and open it in a tab. `minnotes:///abs/path/doc.mnd` opens that document,
// mirroring QCView's project deep links. openTab dedupes: a path that's already
// open just focuses its existing tab.
static void resolveAndOpen(DocumentManager &docs, const QString &s)
{
    QString path = s;
    if (s.startsWith(QStringLiteral("minnotes:"), Qt::CaseInsensitive)) {
        const QUrl u(s);
        path = u.toLocalFile();
        if (path.isEmpty()) path = u.path();   // minnotes:///Users/… → /Users/…
    }
    if (path.isEmpty()) return;
    // A QCView stream-notes folder (or its notes.json): the document beside it.
    const QFileInfo fi(path.startsWith(QStringLiteral("file:")) ? QUrl(path).toLocalFile() : path);
    if (fi.isDir() && QFileInfo::exists(fi.absoluteFilePath() + QStringLiteral("/notes.json"))) {
        docs.openStreamFolder(fi.absoluteFilePath());
        return;
    }
    if (fi.fileName() == QStringLiteral("notes.json") && fi.exists()) {
        docs.openStreamFolder(fi.absolutePath());
        return;
    }
    docs.openTab(path);                        // handles file:// + plain paths
}

int main(int argc, char *argv[])
{
    // Dev-only type probe (A0 speed programme): `--type-probe=<md>` turns the model's per-keystroke
    // timing splits on. Set BEFORE anything can call insertText (mn::perf::enabled() reads it once).
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]).startsWith("--type-probe=")) { qputenv("MN_TYPE_PROBE", "1"); break; }
#ifdef Q_OS_MACOS
    // Pin the default surface format to sRGB on macOS (the ufb recipe).
    // Qt 6's RHI lets the platform pick the swapchain colour space, and on
    // a Display P3 panel our sRGB-authored hex colours come out
    // oversaturated. Setting the colour space on the default format BEFORE
    // any QWindow exists makes Metal present in sRGB regardless of the
    // screen's native gamut.
    {
        QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
        fmt.setColorSpace(QColorSpace::SRgb);
        // Key latency (2026-10-16, A4): with the default swap interval Qt's threaded render loop asks
        // Cocoa for an update, Cocoa answers on the CVDisplayLink tick (0–16 ms later at 60 Hz), the
        // frame renders then and shows at the refresh AFTER that — a typed glyph waited a whole extra
        // frame. Swap interval 0 makes the update request timer-driven (QT_QPA_UPDATE_IDLE_TIME, set to
        // 0 below) and lets Metal present as soon as the frame is done; the compositor still shows it
        // at its own refresh, so nothing tears. Measured key→swap p50 11.9 → 3.4 ms; a wheel glide
        // still paces at ~60 fps (Qt drives animations from a refresh-rate timer when a window is
        // unthrottled — qsgthreadedrenderloop.cpp, "unthrottledWindows"); idle stays idle (5 swaps in
        // 3 s, the caret blink). MN_VSYNC=1 restores the display-link path for an A/B.
        if (qEnvironmentVariableIntValue("MN_VSYNC") <= 0) {
            fmt.setSwapInterval(0);
            if (!qEnvironmentVariableIsSet("QT_QPA_UPDATE_IDLE_TIME")) qputenv("QT_QPA_UPDATE_IDLE_TIME", "0");
        }
        QSurfaceFormat::setDefaultFormat(fmt);
    }
#endif
    MinNotesApplication app(argc, argv);
    app.setApplicationName("minNotes");
    app.setOrganizationName("minNotes");
    // Dev-only runs (probes, shots, headless imports) keep their OWN settings domain: the pool probe
    // toggles the page-width cap and restores it, but a run that exits in the same event-loop turn as
    // the restore left the user's `view.capMeasure` flipped (2026-10-17). Never touch the real prefs.
    for (int i = 1; i < argc; ++i) {
        const QByteArray a(argv[i]);
        if (a.startsWith("--pool-probe") || a.startsWith("--type-probe") || a.startsWith("--canvas-probe")
            || a.startsWith("--shot=") || a.startsWith("--import=") || a.startsWith("--key-probe") || a.startsWith("--video-probe=") || a.startsWith("--export-")) {
            app.setApplicationName("minNotes-dev");
            break;
        }
    }

    // ── Per-session working-copy dir (ephemeral SQLite scratch) ───────────
    // Documents are edited via a LOCAL working copy (BlockModel) so SQLite never
    // runs on smbfs/Dropbox/etc. Each instance gets its own lockfile-guarded
    // scratch subdir: concurrent instances (macOS has no single-instance gate)
    // never delete each other's live copies, and a crashed instance's copies are
    // reclaimed on the next launch (its lock is free).
    const QString scratchRoot =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/scratch");
    const QString sessionDir = scratchRoot + QStringLiteral("/s-") + makeUlid();
    QDir().mkpath(sessionDir);
    BlockModel::setScratchRoot(sessionDir);
    QLockFile sessionLock(sessionDir + QStringLiteral("/.lock"));
    sessionLock.setStaleLockTime(0);
    sessionLock.lock();   // held for the whole process lifetime (released on exit)
    for (const QFileInfo &fi : QDir(scratchRoot).entryInfoList(
             QStringList{QStringLiteral("s-*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (fi.absoluteFilePath() == sessionDir) continue;
        QLockFile other(fi.absoluteFilePath() + QStringLiteral("/.lock"));
        other.setStaleLockTime(0);
        if (other.tryLock(0)) {            // no live owner → crashed session, reclaim it
            other.unlock();
            QDir(fi.absoluteFilePath()).removeRecursively();
        }
    }
    // Drop this session's working copies on a clean quit. (POSIX removes even
    // open files; any Windows-locked file is reclaimed by the next launch's sweep.)
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [sessionDir, &sessionLock] {
        sessionLock.unlock();
        QDir(sessionDir).removeRecursively();
    });

#ifdef Q_OS_WIN
    // ── Single-instance gate (Windows) ────────────────────────────────
    // The first process holds a per-user lock and becomes the primary (it
    // listens on a local socket, below). A later launch — double-clicking a
    // .mnd or following a minnotes:// link while minNotes is open — forwards
    // its file/URI args to the primary and exits, so there's never a second
    // window contending for the same SQLite document. macOS needs none of this:
    // the OS reuses the .app and delivers the open as a QFileOpenEvent.
    // (lockFile/singletonName stay in main's scope so the lock is held — and
    // the name stays resolvable — for the whole process lifetime.)
    const QString singletonName = QStringLiteral("minNotes-singleton");
    const QString lockDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(lockDir);
    QLockFile lockFile(lockDir + QStringLiteral("/minNotes.lock"));
    lockFile.setStaleLockTime(0);   // a hard-killed primary's lock is reclaimable
    if (!lockFile.tryLock(100)) {
        // Secondary: forward our launch args to the primary, then exit(0).
        QStringList fwd;
        for (int i = 1; i < argc; ++i) {
            const QString a = QString::fromLocal8Bit(argv[i]);
            if (a.startsWith(QLatin1Char('-'))) continue;   // skip flags
            fwd << a;
        }
        QLocalSocket sock;
        sock.connectToServer(singletonName);
        if (sock.waitForConnected(800)) {
            sock.write(fwd.join(QLatin1Char('\n')).toUtf8());
            sock.flush();
            sock.waitForBytesWritten(800);
            sock.disconnectFromServer();
        }
        return 0;
    }
#endif

    // Non-mac: use Fusion (the sister-app standard) so the in-window menu themes
    // via `palette`, and force a dark color scheme so the window title bar
    // matches the dark UI. Scrollbars are MnScrollBar (a T.ScrollBar component),
    // so they're unaffected by the style. macOS keeps its native style + the
    // Qt.labs.platform system menu bar.
#ifndef Q_OS_MACOS
    app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
#endif

    // Open documents, one BlockModel per tab. Boots with no tab (the welcome
    // state, backed by the manager's empty fallback model); the File menu / a
    // passed file open or create one.
    DocumentManager docs;
    // The chromeless window (2026-10-21): `--native-frame` (dev) keeps the OS title bar.
    WindowChrome windowChrome(!app.arguments().contains(QStringLiteral("--native-frame")));
    Clipboard clipboard;
    AppUpdater appUpdater;   // "Check for Updates…" → Sparkle (no-op off-macOS)
    // Cross-OS path mappings for referenced media. Loads from QSettings + feeds
    // mn::activeMappings(), which the per-document MediaStore reads on resolve.
    PathMapController pathMap;
    // The editor pool's stable slot table (SR-2). Declared before the engine so
    // it outlives every binding that reads it.
    ViewportSlots viewSlots;
    ViewportSlots canvasSlots;   // the canvas' card pool (B3): slots keyed by CARD index; never attached (no per-slot revs needed yet)

    QQmlApplicationEngine engine;
    engine.addImageProvider("videoframe", new VideoFrameProvider);   // inline video posters
    engine.addImageProvider("pdfpage", new PdfPageProvider);          // inline PDF pages
    engine.rootContext()->setContextProperty("docs", &docs);
    engine.rootContext()->setContextProperty("canvasSlots", &canvasSlots);
    CanvasGestures gestures;   // dev-only: the canvas probe's real pointer / key events
    engine.rootContext()->setContextProperty("gestures", &gestures);
    // Keep `blockModel` pointed at the active tab's model so the existing ~530
    // QML bindings keep working unchanged; re-point it whenever the active tab
    // changes (activeModel() is never null — it falls back to the empty model).
    // 2.0: `blockModel` is the active canvas's FOCUSED PAGE model and `canvas` the active canvas;
    // activeModelChanged fires on a tab switch, a close, or a focus change inside the active canvas
    // (only when the pointer actually moved — re-setting a context property re-evaluates everything).
    engine.rootContext()->setContextProperty("blockModel", docs.activeModel());
    engine.rootContext()->setContextProperty("canvas", docs.activeCanvas());
    viewSlots.attach(docs.activeModel());   // A1: per-slot revisions follow the active model's row signals
    QObject::connect(&docs, &DocumentManager::activeModelChanged, &engine, [&engine, &docs, &viewSlots] {
        engine.rootContext()->setContextProperty("blockModel", docs.activeModel());
        viewSlots.attach(docs.activeModel());
    });
    QObject::connect(&docs, &DocumentManager::activeChanged, &engine, [&engine, &docs] {
        engine.rootContext()->setContextProperty("canvas", docs.activeCanvas());
    });
    engine.rootContext()->setContextProperty("clipboard", &clipboard);
    engine.rootContext()->setContextProperty("appUpdater", &appUpdater);
    engine.rootContext()->setContextProperty("pathMap", &pathMap);
    engine.rootContext()->setContextProperty("viewSlots", &viewSlots);
    // Document export (File ▸ Export…). Follows the active tab's model the
    // same way `blockModel` does.
    Exporter exporter;
    exporter.setModel(docs.activeModel());
    QObject::connect(&docs, &DocumentManager::activeModelChanged, &exporter, [&exporter, &docs] {
        exporter.setModel(docs.activeModel());
    });
    engine.rootContext()->setContextProperty("exporter", &exporter);
    // The CANVAS export (File ▸ Export ▸ Canvas as HTML… / Canvas Poster…; 2.0 step 8). Follows the active tab's canvas.
    CanvasExporter canvasExporter;
    canvasExporter.setCanvas(docs.activeCanvas());
    QObject::connect(&docs, &DocumentManager::activeChanged, &canvasExporter, [&canvasExporter, &docs] {
        canvasExporter.setCanvas(docs.activeCanvas());
    });
    engine.rootContext()->setContextProperty("canvasExporter", &canvasExporter);
    // Search (v2.0.1): one file, everything on its canvas. Follows the active tab's canvas.
    SearchEngine search;
    search.setCanvas(docs.activeCanvas());
    QObject::connect(&docs, &DocumentManager::activeChanged, &search, [&search, &docs] { search.setCanvas(docs.activeCanvas()); });
    engine.rootContext()->setContextProperty("search", &search);
    // Document import (File ▸ Import…, drop/paste intercepts, welcome screen).
    // Follows the active tab the same way the exporter does — a fresh tab is
    // opened first, so the import lands in the new active model.
    Importer importer;
    importer.setModel(docs.activeModel());
    QObject::connect(&docs, &DocumentManager::activeModelChanged, &importer, [&importer, &docs] {
        importer.setModel(docs.activeModel());
    });
    engine.rootContext()->setContextProperty("importer", &importer);
    // .mnpkg package export (File ▸ Export as Package…). Same model-following.
    PackageExporter packageExporter;
    packageExporter.setCanvas(docs.activeCanvas());   // packs the whole FILE: every page + media elements
    QObject::connect(&docs, &DocumentManager::activeChanged, &packageExporter,
                     [&packageExporter, &docs] {
        packageExporter.setCanvas(docs.activeCanvas());
    });
    engine.rootContext()->setContextProperty("packageExporter", &packageExporter);
    // Collect Media (Document ▸ Collect Media…). Same model-following.
    MediaCollector mediaCollector;
    mediaCollector.setCanvas(docs.activeCanvas());
    QObject::connect(&docs, &DocumentManager::activeChanged, &mediaCollector,
                     [&mediaCollector, &docs] {
        mediaCollector.setCanvas(docs.activeCanvas());
    });
    engine.rootContext()->setContextProperty("mediaCollector", &mediaCollector);
    // Tab merge (drag a tab onto another tab). Models are passed per call —
    // no model-following; the merger is stateless between merges.
    DocumentMerger merger;
    engine.rootContext()->setContextProperty("merger", &merger);
    // Rich clipboard paste (x-mnd-blocks): the merger's plan/copy/apply
    // shape for a pasted block run; assets travel on a worker when needed.
    ClipboardPaster paster;
    engine.rootContext()->setContextProperty("paster", &paster);
    // Spell + grammar checking (0.5.0): follows the active model like the
    // exporter; the engine lives on its own worker thread (joined in the dtor,
    // which runs before the models go — declared after `docs`).
    SpellService spell(QStringLiteral(MINNOTES_APP_VERSION));
    spell.setModel(docs.activeModel());
    QObject::connect(&docs, &DocumentManager::activeModelChanged, &spell,
                     [&spell, &docs] { spell.setModel(docs.activeModel()); });
    engine.rootContext()->setContextProperty("spell", &spell);
    // Quit gate: QEvent::Quit (⌘Q / logout / last-window-closed) is vetoed in
    // MinNotesApplication and surfaced as quitRequested; Main.qml runs the
    // unsaved-changes guard and calls minApp.forceQuit() when it's safe.
    engine.rootContext()->setContextProperty("minApp", &app);
    engine.rootContext()->setContextProperty("windowChrome", &windowChrome);
    // A mapping edit must re-resolve media in every open tab.
    QObject::connect(&pathMap, &PathMapController::mappingsChanged, &docs,
                     [&docs] { docs.refreshMedia(); });
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("MinNotes.App", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;
    for (QObject* o : engine.rootObjects())
        if (auto* w = qobject_cast<QWindow*>(o)) { windowChrome.attach(w); break; }

    // File / URI opening: Finder double-click and minnotes:// deep links arrive
    // as QFileOpenEvents (captured by MinNotesApplication); terminal/CLI args are
    // parsed here. Drain any cold-start events that fired before the engine loaded.
    QObject::connect(&app, &MinNotesApplication::openRequested, &docs,
                     [&docs](const QString &s) { resolveAndOpen(docs, s); });
    app.markReady();
    const QStringList pending = app.takePending();
    for (const QString &s : pending) resolveAndOpen(docs, s);
    const QStringList args = app.arguments();
    QString exportPdfTo, exportHtmlTo, exportCanvasHtmlTo, exportPosterTo, shotTo;
    int shotH = 900;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i].startsWith(QLatin1String("--export-pdf="))) { exportPdfTo = args[i].mid(13); continue; }
        if (args[i].startsWith(QLatin1String("--export-html="))) { exportHtmlTo = args[i].mid(14); continue; }
        if (args[i].startsWith(QLatin1String("--export-canvas-html="))) { exportCanvasHtmlTo = args[i].mid(21); continue; }
        if (args[i].startsWith(QLatin1String("--export-poster="))) { exportPosterTo = args[i].mid(16); continue; }
        if (args[i].startsWith(QLatin1String("--shot="))) { shotTo = args[i].mid(7); continue; }
        if (args[i].startsWith(QLatin1String("--shot-height="))) { shotH = args[i].mid(14).toInt(); continue; }
        if (args[i].startsWith(QLatin1Char('-'))) continue;   // skip flags
        resolveAndOpen(docs, args[i]);
    }
    // Dev-only (SR-4 walk 2): `minNotes --export-pdf=<out.pdf> <doc.mnd>` writes the active
    // document's PDF export headlessly (QT_QPA_PLATFORM=offscreen) and exits — 0 on success,
    // 102 when the export fails. Lets a PDF report be reproduced without driving the GUI.
    // Calibration (2026-09-16): `--shot=<out.png> <doc.mnd>` renders the document view offscreen at
    // 1200 × 900 and saves the window — the A/B against a headless browser's shot of the HTML export.
    if (!shotTo.isEmpty())
        QTimer::singleShot(qEnvironmentVariableIntValue("MN_SHOT_DELAY") > 0 ? qEnvironmentVariableIntValue("MN_SHOT_DELAY") : 2500,
                           &app, [&app, &engine, &docs, shotTo, shotH] {
            QQuickWindow* w = nullptr;
            for (QObject* o : engine.rootObjects()) if ((w = qobject_cast<QQuickWindow*>(o))) break;
            if (!w) { app.exit(103); return; }
            w->resize(1200, shotH);   // --shot-height=N for a taller sample
            QTimer::singleShot(1500, &app, [&app, &docs, w, shotTo] {
                if (BlockModel* m = docs.activeModel()) m->flushLayoutSpike();   // measured heights, not estimates
                if (QObject* ed = w->findChild<QObject*>(QStringLiteral("editorRoot"))) QMetaObject::invokeMethod(ed, "dumpDelegates");
                QTimer::singleShot(600, &app, [&app, &docs, w, shotTo] {
                const bool ok = w->grabWindow().save(shotTo);
                // …and the model's block geometry beside it (<out>.rows.txt: row type y h text) —
                // the app half of the A/B against the browser's element offsets.
                if (BlockModel* m = docs.activeModel()) {
                    QFile f(shotTo + QStringLiteral(".rows.txt"));
                    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        QTextStream ts(&f);
                        for (int r = 0; r < m->rowCountQml(); ++r)
                            ts << r << '\t' << m->typeForRow(r) << '\t' << m->yForRow(r) << '\t' << m->heightForRow(r)
                               << '\t' << m->contentForRow(r).left(24).replace('\n', ' ') << '\n';
                    }
                }
                app.exit(ok ? 0 : 103);
                });
            });
        });
    // Dev-only (2026-10-16): the keystroke's full pipeline (queue → turn → sync → swap) per key under
    // `--perf-log`; `--key-probe=N` posts N synthetic keys, prints percentiles and exits (the A/B
    // harness for render-loop settings: QSG_RENDER_LOOP, QT_QPA_UPDATE_IDLE_TIME, swap interval).
    {
        int synth = 0; bool arm = false;
        for (const QString& a : args) {
            if (a == QLatin1String("--perf-log")) arm = true;
            else if (a.startsWith(QLatin1String("--key-probe="))) { synth = a.mid(12).toInt(); arm = true; }
        }
        if (arm) QTimer::singleShot(0, &app, [&engine, synth] {
            for (QObject* o : engine.rootObjects())
                if (auto* w = qobject_cast<QQuickWindow*>(o)) { new KeyLatencyProbe(w, synth, w); break; }
        });
    }
    // Its twin (2026-09-16): `--export-html=<out.html> <doc.mnd>` — screenshot the result with a
    // headless browser to eyeball the sheet / tables / ink without the GUI.
    if (!exportHtmlTo.isEmpty())
        QTimer::singleShot(1500, &app, [&app, &exporter, exportHtmlTo] {   // (after an --import lands)
            app.exit(exporter.exportHtml(exportHtmlTo, true) ? 0 : 102);
        });
    // The canvas twins (2.0 step 8): `--export-canvas-html=<out.html>` / `--export-poster=<out.png>` on the file given.
    if (!exportCanvasHtmlTo.isEmpty() || !exportPosterTo.isEmpty())
        QTimer::singleShot(1500, &app, [&app, &canvasExporter, exportCanvasHtmlTo, exportPosterTo] {
            bool ok = true;
            if (!exportCanvasHtmlTo.isEmpty()) ok = canvasExporter.exportHtml(exportCanvasHtmlTo, true) && ok;
            if (!exportPosterTo.isEmpty()) ok = canvasExporter.exportPoster(exportPosterTo, 1600) && ok;
            app.exit(ok ? 0 : 103);
        });
    if (!exportPdfTo.isEmpty())
        QTimer::singleShot(0, &app, [&app, &exporter, exportPdfTo] {
            const bool ok = exporter.exportPdf(exportPdfTo, true, false);
            if (ok) {   // …and one PNG per page beside it (<out>-pageN.png) for eyeballing
                QPdfDocument pdoc;
                if (pdoc.load(exportPdfTo) == QPdfDocument::Error::None)
                    for (int pg = 0; pg < pdoc.pageCount(); ++pg) {
                        const QSizeF pts = pdoc.pagePointSize(pg);
                        const QImage r = pdoc.render(pg, QSize(int(pts.width() * 2), int(pts.height() * 2)));
                        QImage page(r.size(), QImage::Format_RGB32);   // on paper: the render is transparent
                        page.fill(Qt::white);
                        { QPainter p(&page); p.drawImage(0, 0, r); }
                        page.save(exportPdfTo.chopped(4) + QStringLiteral("-page%1.png").arg(pg + 1));
                    }
            }
            app.exit(ok ? 0 : 102);
        });

#ifdef Q_OS_WIN
    // ── Single-instance primary: serve forwarded launches ─────────────
    // We hold the lock (gate above), so listen for secondary processes that
    // forward their file/URI args here → open the doc(s) and raise our window.
    auto raiseMainWindow = [&engine]() {
        const auto roots = engine.rootObjects();
        if (roots.isEmpty()) return;
        if (auto *w = qobject_cast<QWindow *>(roots.constFirst())) {
            if (w->visibility() == QWindow::Minimized) w->showNormal();  // restore
            else                                       w->show();
            w->raise();
            w->requestActivate();
        }
    };
    QLocalServer singletonServer;
    // Clear a stale socket left by a primary that crashed without unlinking it
    // (no-op when none exists), then listen.
    QLocalServer::removeServer(singletonName);
    if (!singletonServer.listen(singletonName))
        qWarning("minNotes: single-instance listen failed (%s) — deep links from "
                 "later launches won't reach this window",
                 qPrintable(singletonServer.errorString()));
    QObject::connect(&singletonServer, &QLocalServer::newConnection, &app,
        [&singletonServer, &docs, raiseMainWindow]() {
            QLocalSocket *sock = singletonServer.nextPendingConnection();
            if (!sock) return;
            sock->waitForReadyRead(500);
            const QStringList items = QString::fromUtf8(sock->readAll())
                .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            sock->disconnectFromServer();
            sock->deleteLater();
            for (const QString &s : items) resolveAndOpen(docs, s);
            raiseMainWindow();   // also focuses on a bare re-launch (no args)
        });
#endif

    // Start Sparkle once the event loop is running and the window is up
    // (honors the Info.plist SU* keys; no-op when Sparkle isn't vendored).
    QTimer::singleShot(0, &app, [] { mn::startSparkleUpdater(); });

    return app.exec();
}
