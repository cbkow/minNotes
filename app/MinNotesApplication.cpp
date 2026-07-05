#include "MinNotesApplication.h"

#include <QFileOpenEvent>
#include <QUrl>

MinNotesApplication::MinNotesApplication(int &argc, char **argv)
    : QGuiApplication(argc, argv)
{
}

bool MinNotesApplication::event(QEvent *e)
{
    if (e->type() == QEvent::FileOpen) {
        auto *foe = static_cast<QFileOpenEvent *>(e);
        // A custom-scheme deep link (minnotes://…) arrives as a URL with no file
        // path; a Finder/`open` file arrives with a local path (and a file://
        // URL). Prefer the path for files, the URL for the deep link.
        QString s;
        const QUrl url = foe->url();
        if (!url.isEmpty() && url.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) != 0)
            s = url.toString();
        else
            s = foe->file();
        if (!s.isEmpty()) {
            if (ready_) emit openRequested(s);
            else        pending_ << s;
        }
        return true;
    }
    if (e->type() == QEvent::Quit && !quitAllowed_) {
        // Veto termination (ignore() → NSTerminateCancel on macOS) and hand the
        // decision to QML — it prompts for unsaved work and calls forceQuit().
        e->ignore();
        emit quitRequested();
        return true;
    }
    return QGuiApplication::event(e);
}

void MinNotesApplication::forceQuit()
{
    quitAllowed_ = true;
    quit();
}

void MinNotesApplication::markReady() { ready_ = true; }

QStringList MinNotesApplication::takePending()
{
    const QStringList p = pending_;
    pending_.clear();
    return p;
}
