#include "FileLinks.h"
#include "MinPaths.h"
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace mn {

namespace {
// The path ANOTHER process must be handed for a file we can see (2026-10-24, the Microsoft Store build). Inside an
// MSIX package, files the app creates under AppData land in the package's private LocalCache while the app keeps
// seeing them at the AppData path — so a sealed package's media, extracted into the scratch dir, would reach Explorer
// or the default app as a path that does not exist for them. The opened handle knows the real location; outside a
// package (or for anything not redirected) it is the same path.
QString pathForOtherApps(const QString& path) {
    return QDir::fromNativeSeparators(mp::take(mp_external_path(mp::u8(path).constData())));   // min-paths `volume::external_path`
}
} // namespace

QString ufbLinkFor(const QString& path) {
    return mp::take(mp_ufb_link(mp::u8(path).constData()));   // min-paths `links::ufb_link` (slashes literal, the rest percent-encoded)
}

void revealInFileManager(const QString& p) {
    if (p.isEmpty() || !QFileInfo::exists(p)) return;
    const QString path = pathForOtherApps(p);
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), { QStringLiteral("-R"), path });
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"),
                            { QStringLiteral("/select,") + QDir::toNativeSeparators(path) });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

void openFolder(const QString& d) {
    if (d.isEmpty() || !QFileInfo::exists(d)) return;
    const QString dir = pathForOtherApps(d);
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), { dir });
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"), { QDir::toNativeSeparators(dir) });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
#endif
}

void openInUfb(const QString& path) {
    const QString link = ufbLinkFor(path);
    if (!link.isEmpty()) QDesktopServices::openUrl(QUrl(link));
}

void openWithDefaultApp(const QString& path) {
    if (path.isEmpty() || !QFileInfo::exists(path)) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(pathForOtherApps(path)));
}

} // namespace mn
