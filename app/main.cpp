#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "core/BlockModel.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("minNotes");
    app.setOrganizationName("minNotes");

    // The document model. Phase 1 backs this with the SQLite store (DESIGN §3–4);
    // for now it serves the synthetic blocks the spike proved the editor against.
    BlockModel model;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("blockModel", &model);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []{ QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("MinNotes.App", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;
    return app.exec();
}
