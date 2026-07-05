#pragma once

#include <QGuiApplication>
#include <QString>
#include <QStringList>

// QGuiApplication subclass that captures macOS QFileOpenEvent — Finder double-
// click / `open file.mndb` / `minnotes://` deep links — and forwards them.
// Cold-start events (delivered before the QML engine + model are ready) are
// stashed and drained from main() once the app signals readiness. The same
// `openRequested` path serves CLI/terminal arguments (parsed in main).
//
// It also gates app termination: macOS ⌘Q (the auto-inserted Quit item),
// logout/shutdown, and last-window-closed all arrive as QEvent::Quit — a path
// that does NOT reliably run the QML window's onClosing guard. Unguarded, quit
// reaches main()'s aboutToQuit handler, which deletes the scratch dir holding
// every tab's unsaved working copy. So Quit is vetoed here and re-emitted as
// `quitRequested`; QML runs the unsaved-changes flow and calls forceQuit()
// once it's safe (immediately when nothing is dirty).
class MinNotesApplication : public QGuiApplication
{
    Q_OBJECT
public:
    MinNotesApplication(int &argc, char **argv);

    bool event(QEvent *e) override;
    void markReady();                 // call once the model/engine are up
    QStringList takePending();        // drain the stashed cold-start requests

    // QML calls this after the unsaved-changes guard clears (or the user
    // chose Discard). Lets the next Quit event through and quits.
    Q_INVOKABLE void forceQuit();

signals:
    // A file path, file:// URL, or minnotes:// deep link to open.
    void openRequested(const QString &pathOrUri);
    // Termination was requested (⌘Q / logout / last window closed) and is on
    // hold — QML must confirm unsaved work, then call forceQuit().
    void quitRequested();

private:
    QStringList pending_;
    bool ready_ = false;
    bool quitAllowed_ = false;
};
