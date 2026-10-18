#include "FileLinks.h"
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace mn {

QString ufbLinkFor(const QString& path) {
    if (path.isEmpty()) return {};
#if defined(Q_OS_WIN)
    const QString os = QStringLiteral("win");
#else
    const QString os = QStringLiteral("mac");
#endif
    const QString norm = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
    return QStringLiteral("ufb:///") + os + QLatin1Char('/')
         + QString::fromLatin1(QUrl::toPercentEncoding(norm, "/"));
}

void revealInFileManager(const QString& path) {
    if (path.isEmpty() || !QFileInfo::exists(path)) return;
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), { QStringLiteral("-R"), path });
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"),
                            { QStringLiteral("/select,") + QDir::toNativeSeparators(path) });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

void openFolder(const QString& dir) {
    if (dir.isEmpty() || !QFileInfo::exists(dir)) return;
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
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

} // namespace mn
