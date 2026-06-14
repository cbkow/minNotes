#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
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

    // Start Sparkle once the event loop is running and the window is up
    // (honors the Info.plist SU* keys; no-op when Sparkle isn't vendored).
    QTimer::singleShot(0, &app, [] { mn::startSparkleUpdater(); });

    return app.exec();
}
