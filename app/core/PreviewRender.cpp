#include "PreviewRender.h"

// stb_image: the implementation lives in THIS translation unit only, HDR alone (jpg / png / … are Qt's).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#define STBI_NO_STDIO_OPEN
#include "stb_image.h"

#include <QFileInfo>
#include <QSet>
#include <QUrl>
#include <cmath>
#include <cstdint>

namespace mn::preview {

namespace {

const QSet<QString>& hdrExts()  { static const QSet<QString> s { QStringLiteral("hdr"), QStringLiteral("rgbe") }; return s; }
const QSet<QString>& psdExts()  { static const QSet<QString> s { QStringLiteral("psd"), QStringLiteral("psb") }; return s; }
const QSet<QString>& heifExts() { static const QSet<QString> s { QStringLiteral("heic"), QStringLiteral("heif"), QStringLiteral("hif") }; return s; }
const QSet<QString>& rawExts()  {
    static const QSet<QString> s {
        QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("nef"), QStringLiteral("arw"), QStringLiteral("dng"),
        QStringLiteral("raf"), QStringLiteral("orf"), QStringLiteral("rw2"), QStringLiteral("pef"), QStringLiteral("srw"),
    };
    return s;
}

// Linear scene-referred → 8-bit display: clamp, then 2.2 gamma (what RV / DJV show by default; ufb's curve).
inline uint8_t toneMap(float v) {
    if (!std::isfinite(v) || v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<uint8_t>(std::pow(v, 1.0f / 2.2f) * 255.0f + 0.5f);
}

QString localPath(const QString& p) { return p.startsWith(QLatin1String("file:")) ? QUrl(p).toLocalFile() : p; }

QImage capped(QImage img, int maxEdge) {
    if (img.isNull() || maxEdge <= 0) return img;
    const int edge = std::max(img.width(), img.height());
    if (edge <= maxEdge) return img;
    return img.scaled(img.width() * maxEdge / edge, img.height() * maxEdge / edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

}  // namespace

bool supportsExt(const QString& ext) {
    if (hdrExts().contains(ext)) return true;
#ifdef Q_OS_MACOS
    return psdExts().contains(ext) || heifExts().contains(ext) || rawExts().contains(ext);
#else
    return false;
#endif
}

QString formatLabel(const QString& ext) {
    if (hdrExts().contains(ext)) return QStringLiteral("HDR");
    if (psdExts().contains(ext)) return ext.toUpper();
    if (heifExts().contains(ext)) return QStringLiteral("HEIC");
    if (rawExts().contains(ext)) return QStringLiteral("RAW");
    return QString();
}

QImage render(const QString& pathOrUrl, int maxEdge) {
    const QString path = localPath(pathOrUrl);
    const QString ext = QFileInfo(path).suffix().toLower();
    if (!supportsExt(ext) || !QFileInfo::exists(path)) return {};
    if (hdrExts().contains(ext)) return renderHdr(path, maxEdge);
    return renderNative(path, maxEdge);
}

QImage renderHdr(const QString& path, int maxEdge) {
    int w = 0, h = 0, comp = 0;
    const QByteArray p = path.toUtf8();
    if (!stbi_info(p.constData(), &w, &h, &comp) || w <= 0 || h <= 0) return {};
    if (qint64(w) * qint64(h) > 64LL * 1024 * 1024) return {};   // the 64 MP guard: never allocate gigabytes of float
    float* data = stbi_loadf(p.constData(), &w, &h, &comp, 3);
    if (!data) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    if (img.isNull()) { stbi_image_free(data); return {}; }
    const float* src = data;
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < w; ++x, src += 3) {
            line[x * 4 + 0] = toneMap(src[0]);
            line[x * 4 + 1] = toneMap(src[1]);
            line[x * 4 + 2] = toneMap(src[2]);
            line[x * 4 + 3] = 255;
        }
    }
    stbi_image_free(data);
    return capped(img, maxEdge);
}

#ifndef Q_OS_MACOS
QImage renderNative(const QString&, int) { return {}; }
#endif

}  // namespace mn::preview
