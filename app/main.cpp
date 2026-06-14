#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include <QLocalServer>      // Windows single-instance: primary listens,
#include <QLocalSocket>      // secondary forwards its file/URI args here
#include <QLockFile>         // the per-user gate that elects the primary
#include <QStandardPaths>
#include <QDir>
#include <QWindow>           // raise the existing window on a secondary launch
#include "MinNotesApplication.h"
#include "AppUpdater.h"
#include "sparkle_updater_macos.h"
#include "core/BlockModel.h"
#include "core/Clipboard.h"
#include "core/VideoFrameProvider.h"
#include "core/PdfPageProvider.h"

// Resolve a file path / file:// URL / minnotes:// deep link to a document path
// and open it. `minnotes:///abs/path/doc.mndb` opens that document, mirroring
// QCView's project deep links.
static void resolveAndOpen(BlockModel &model, const QString &s)
{
    QString path = s;
    if (s.startsWith(QStringLiteral("minnotes:"), Qt::CaseInsensitive)) {
        const QUrl u(s);
        path = u.toLocalFile();
        if (path.isEmpty()) path = u.path();   // minnotes:///Users/… → /Users/…
    }
    if (!path.isEmpty())
        model.openDocument(path);              // handles file:// + plain paths
}

int main(int argc, char *argv[])
{
    MinNotesApplication app(argc, argv);
    app.setApplicationName("minNotes");
    app.setOrganizationName("minNotes");

#ifdef Q_OS_WIN
    // ── Single-instance gate (Windows) ────────────────────────────────
    // The first process holds a per-user lock and becomes the primary (it
    // listens on a local socket, below). A later launch — double-clicking a
    // .mndb or following a minnotes:// link while minNotes is open — forwards
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

    // The document model. Boots with no document (the welcome state); the File
    // menu / a passed file open or create one.
    BlockModel model;
    Clipboard clipboard;
    AppUpdater appUpdater;   // "Check for Updates…" → Sparkle (no-op off-macOS)

    QQmlApplicationEngine engine;
    engine.addImageProvider("videoframe", new VideoFrameProvider);   // inline video posters
    engine.addImageProvider("pdfpage", new PdfPageProvider);          // inline PDF pages
    engine.rootContext()->setContextProperty("blockModel", &model);
    engine.rootContext()->setContextProperty("clipboard", &clipboard);
    engine.rootContext()->setContextProperty("appUpdater", &appUpdater);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("MinNotes.App", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    // File / URI opening: Finder double-click and minnotes:// deep links arrive
    // as QFileOpenEvents (captured by MinNotesApplication); terminal/CLI args are
    // parsed here. Drain any cold-start events that fired before the engine loaded.
    QObject::connect(&app, &MinNotesApplication::openRequested, &model,
                     [&model](const QString &s) { resolveAndOpen(model, s); });
    app.markReady();
    const QStringList pending = app.takePending();
    for (const QString &s : pending) resolveAndOpen(model, s);
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i].startsWith(QLatin1Char('-'))) continue;   // skip flags
        resolveAndOpen(model, args[i]);
    }

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
        [&singletonServer, &model, raiseMainWindow]() {
            QLocalSocket *sock = singletonServer.nextPendingConnection();
            if (!sock) return;
            sock->waitForReadyRead(500);
            const QStringList items = QString::fromUtf8(sock->readAll())
                .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            sock->disconnectFromServer();
            sock->deleteLater();
            for (const QString &s : items) resolveAndOpen(model, s);
            raiseMainWindow();   // also focuses on a bare re-launch (no args)
        });
#endif

    // Start Sparkle once the event loop is running and the window is up
    // (honors the Info.plist SU* keys; no-op when Sparkle isn't vendored).
    QTimer::singleShot(0, &app, [] { mn::startSparkleUpdater(); });

    return app.exec();
}
