#include "FileLinks.h"
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>
#if defined(Q_OS_WIN)
#include <iterator>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mn {

namespace {
// The path ANOTHER process must be handed for a file we can see (2026-10-24, the Microsoft Store build). Inside an
// MSIX package, files the app creates under AppData land in the package's private LocalCache while the app keeps
// seeing them at the AppData path — so a sealed package's media, extracted into the scratch dir, would reach Explorer
// or the default app as a path that does not exist for them. The opened handle knows the real location; outside a
// package (or for anything not redirected) it is the same path.
QString pathForOtherApps(const QString& path) {
#if defined(Q_OS_WIN)
    const HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);   // BACKUP_SEMANTICS: directories open too
    if (h == INVALID_HANDLE_VALUE) return path;
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, DWORD(std::size(buf)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= std::size(buf)) return path;
    QString real = QString::fromWCharArray(buf, int(n));
    if (real.startsWith(QLatin1String("\\\\?\\UNC\\"))) real = QLatin1String("\\\\") + real.mid(8);
    else if (real.startsWith(QLatin1String("\\\\?\\"))) real = real.mid(4);
    return QDir::fromNativeSeparators(real);
#else
    return path;
#endif
}
} // namespace

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
