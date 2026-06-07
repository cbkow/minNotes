#include "MediaStore.h"
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QImage>
#include <QImageReader>
#include <QGuiApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QBuffer>

MediaStore::MediaStore(const QString& docPath) : docDir_(QFileInfo(docPath).absolutePath()) {}

QString MediaStore::assetsDir() const {
    const QString dir = docDir_ + QStringLiteral("/.minnotes");
    QDir().mkpath(dir);
    return dir;
}

MediaStore::ImageRef MediaStore::importFile(const QString& fileUrlOrPath) const {
    QString path = fileUrlOrPath;
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    QImageReader r(path);
    const QSize sz = r.size();            // header-only probe; no full decode
    if (!sz.isValid() || sz.isEmpty()) return {};
    return { path, sz.width(), sz.height() };   // referenced in place (absolute)
}

MediaStore::ImageRef MediaStore::importClipboardImage() const {
    const QImage img = QGuiApplication::clipboard() ? QGuiApplication::clipboard()->image() : QImage();
    if (img.isNull()) return {};

    // Encode to PNG once, hash the bytes → content-addressed filename (dedup).
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    buf.close();
    const QString sha = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha1).toHex());
    const QString rel = QStringLiteral(".minnotes/") + sha + QStringLiteral(".png");
    const QString abs = docDir_ + QStringLiteral("/") + rel;
    if (!QFileInfo::exists(abs)) {        // skip the write if this exact image is already stored
        assetsDir();                       // ensure the folder
        QFile f(abs);
        if (f.open(QIODevice::WriteOnly)) { f.write(png); f.close(); }
    }
    return { rel, img.width(), img.height() };
}

QString MediaStore::resolvePath(const QString& src) const {
    if (src.startsWith(QLatin1String(".minnotes/")))
        return docDir_ + QStringLiteral("/") + src;
    return src;                            // absolute reference
}

QString MediaStore::resolveUrl(const QString& src) const {
    if (src.isEmpty()) return {};
    return QUrl::fromLocalFile(resolvePath(src)).toString();
}
