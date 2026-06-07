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

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

MediaStore::MediaStore(const QString& docPath) : docDir_(QFileInfo(docPath).absolutePath()) {}

bool MediaStore::isVideoPath(const QString& path) {
    static const QStringList exts = {
        QStringLiteral("mp4"),  QStringLiteral("mov"),  QStringLiteral("m4v"),
        QStringLiteral("mkv"),  QStringLiteral("webm"), QStringLiteral("avi"),
        QStringLiteral("mxf"),  QStringLiteral("m2ts"), QStringLiteral("mts"),
        QStringLiteral("ts"),   QStringLiteral("wmv"),  QStringLiteral("flv"),
        QStringLiteral("mpg"),  QStringLiteral("mpeg"), QStringLiteral("3gp"),
    };
    QString p = path;
    if (p.startsWith(QLatin1String("file:"))) p = QUrl(p).toLocalFile();
    return exts.contains(QFileInfo(p).suffix().toLower());
}

MediaStore::VideoRef MediaStore::importVideoFile(const QString& fileUrlOrPath) const {
    QString path = fileUrlOrPath;
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    if (!QFileInfo(path).isFile()) return {};

    // Header-only probe: open the container and read stream info. No codec is
    // opened and no frame is decoded, so this is cheap enough to run inline on
    // a drop (unlike VideoDecoder::open, which spins decode threads + hwaccel).
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) != 0)
        return {};
    VideoRef ref;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vs >= 0) {
            AVStream* st = fmt->streams[vs];
            ref.src = path;                        // referenced in place (absolute)
            ref.w = st->codecpar->width;
            ref.h = st->codecpar->height;
            const AVRational fr = (st->avg_frame_rate.num != 0)
                ? st->avg_frame_rate : st->r_frame_rate;
            ref.fps = (fr.den != 0) ? double(fr.num) / double(fr.den) : 0.0;
            if (st->duration != AV_NOPTS_VALUE && st->time_base.den != 0)
                ref.durationMs = qint64(1000.0 * st->duration * av_q2d(st->time_base));
            else if (fmt->duration != AV_NOPTS_VALUE)
                ref.durationMs = fmt->duration / (AV_TIME_BASE / 1000);
            if (st->nb_frames > 0)
                ref.frames = int(st->nb_frames);
            else if (ref.fps > 0.0 && ref.durationMs > 0)
                ref.frames = int(ref.fps * ref.durationMs / 1000.0 + 0.5);
        }
    }
    avformat_close_input(&fmt);
    return ref;
}

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
