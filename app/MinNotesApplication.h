#pragma once

#include <QGuiApplication>
#include <QString>
#include <QStringList>

// QGuiApplication subclass that captures macOS QFileOpenEvent — Finder double-
// click / `open file.mndb` / `minnotes://` deep links — and forwards them.
// Cold-start events (delivered before the QML engine + model are ready) are
// stashed and drained from main() once the app signals readiness. The same
// `openRequested` path serves CLI/terminal arguments (parsed in main).
class MinNotesApplication : public QGuiApplication
{
    Q_OBJECT
public:
    MinNotesApplication(int &argc, char **argv);

    bool event(QEvent *e) override;
    void markReady();                 // call once the model/engine are up
    QStringList takePending();        // drain the stashed cold-start requests

signals:
    // A file path, file:// URL, or minnotes:// deep link to open.
    void openRequested(const QString &pathOrUri);

private:
    QStringList pending_;
    bool ready_ = false;
};
