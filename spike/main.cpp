#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QWindow>
#include <QDebug>
#include "BlockModel.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("minNotes virtualization spike");

    // What Qt thinks each connected screen's refresh rate is (vs. macOS's real
    // maximumFramesPerSecond). If Qt says 60 where AppKit says 120, that's the cap.
    for (QScreen* s : app.screens())
        qInfo().nospace() << "[spike] Qt screen '" << s->name() << "' "
                          << s->geometry().width() << "x" << s->geometry().height()
                          << " refreshRate=" << s->refreshRate() << "Hz";

    BlockModel model;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("blockModel", &model);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []{ QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("Spike", "Main");
    if (engine.rootObjects().isEmpty()) return -1;

    // Log which screen the window is on and Qt's refresh for it — re-logs when
    // dragged to another monitor, so we see the DELL's Qt refreshRate directly.
    if (auto* w = qobject_cast<QWindow*>(engine.rootObjects().first())) {
        auto logScreen = [w]{
            if (QScreen* s = w->screen())
                qInfo().nospace() << "[spike] WINDOW now on '" << s->name()
                                  << "'  Qt refreshRate=" << s->refreshRate() << "Hz";
        };
        QObject::connect(w, &QWindow::screenChanged, w, [logScreen](QScreen*){ logScreen(); });
        logScreen();
    }
    return app.exec();
}
