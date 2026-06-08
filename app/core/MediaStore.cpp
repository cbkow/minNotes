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
#include <QPdfDocument>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>   // AVCOL_SPC_* / AVCOL_RANGE_*
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

bool MediaStore::isPdfPath(const QString& path) {
    QString p = path;
    if (p.startsWith(QLatin1String("file:"))) p = QUrl(p).toLocalFile();
    return QFileInfo(p).suffix().toLower() == QLatin1String("pdf");
}

QImage MediaStore::renderPdfPage(const QString& path, int page, int maxW) {
    QPdfDocument doc;                          // local: load/render/destroy on this (worker) thread
    if (doc.load(path) != QPdfDocument::Error::None) return {};
    if (page < 0 || page >= doc.pageCount()) return {};
    const QSizeF pts = doc.pagePointSize(page);
    if (pts.width() <= 0 || pts.height() <= 0) return {};
    const int w = (maxW > 0) ? maxW : int(pts.width() + 0.5);
    const int h = int(w * pts.height() / pts.width() + 0.5);
    return doc.render(page, QSize(w, h));
}

MediaStore::PdfRef MediaStore::importPdfFile(const QString& fileUrlOrPath) const {
    QString path = fileUrlOrPath;
    if (path.startsWith(QLatin1String("file:"))) path = QUrl(path).toLocalFile();
    if (!QFileInfo(path).isFile()) return {};
    QPdfDocument doc;
    if (doc.load(path) != QPdfDocument::Error::None) return {};
    const int pages = doc.pageCount();
    if (pages < 1) return {};
    const QSizeF pts = doc.pagePointSize(0);   // page-0 size in PDF points
    if (pts.width() <= 0 || pts.height() <= 0) return {};
    return { QFileInfo(path).absoluteFilePath(), pages,
             int(pts.width() + 0.5), int(pts.height() + 0.5) };
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

MediaStore::ImageRef MediaStore::importImage(const QImage& img) const {
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

MediaStore::ImageRef MediaStore::importClipboardImage() const {
    const QImage img = QGuiApplication::clipboard() ? QGuiApplication::clipboard()->image() : QImage();
    return importImage(img);
}

static QImage frameToImage(const AVFrame* frame, int maxW) {
    const int w = frame->width, h = frame->height;
    if (w <= 0 || h <= 0) return {};
    int dw = w, dh = h;
    if (maxW > 0 && w > maxW) { dw = maxW; dh = int(double(h) * maxW / w + 0.5); }
    dw &= ~1; dh &= ~1; if (dw < 2) dw = 2; if (dh < 2) dh = 2;   // keep even dims
    SwsContext* sws = sws_getContext(w, h, static_cast<AVPixelFormat>(frame->format),
                                     dw, dh, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                     nullptr, nullptr, nullptr);
    if (!sws) return {};

    // Convert with the SOURCE colorspace matrix, not swscale's BT.601 default —
    // otherwise greens shift toward cyan and reds toward pink (very visible on
    // HD/ProRes-4444). Pick by metadata, with the standard HD=709/SD=601
    // fallback, and honor the source range. Mirrors ufb's decoder so the poster
    // matches the played frame.
    int srcCsp;
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709:      srcCsp = SWS_CS_ITU709;    break;
        case AVCOL_SPC_BT470BG:    srcCsp = SWS_CS_ITU601;    break;
        case AVCOL_SPC_SMPTE170M:  srcCsp = SWS_CS_SMPTE170M; break;
        case AVCOL_SPC_SMPTE240M:  srcCsp = SWS_CS_SMPTE240M; break;
        case AVCOL_SPC_FCC:        srcCsp = SWS_CS_FCC;       break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:  srcCsp = SWS_CS_BT2020;    break;
        default:                   srcCsp = (w >= 1280 || h >= 720) ? SWS_CS_ITU709
                                                                    : SWS_CS_SMPTE170M; break;
    }
    const int srcFullRange = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    sws_setColorspaceDetails(sws,
        sws_getCoefficients(srcCsp), srcFullRange,
        sws_getCoefficients(SWS_CS_ITU709), /*dstFullRange=*/1,
        /*brightness=*/0, /*contrast=*/1 << 16, /*saturation=*/1 << 16);

    QImage img(dw, dh, QImage::Format_RGBA8888);
    uint8_t* dst[4]   = { img.bits(), nullptr, nullptr, nullptr };
    int      dstLn[4] = { int(img.bytesPerLine()), 0, 0, 0 };
    sws_scale(sws, frame->data, frame->linesize, 0, h, dst, dstLn);
    sws_freeContext(sws);
    return img;
}

QImage MediaStore::extractFrame(const QString& path, int frameNo, int maxW) {
    if (path.isEmpty()) return {};
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) != 0) return {};
    QImage result;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vs >= 0) {
            AVStream* st = fmt->streams[vs];
            const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
            AVCodecContext* ctx = dec ? avcodec_alloc_context3(dec) : nullptr;
            if (ctx && avcodec_parameters_to_context(ctx, st->codecpar) >= 0
                    && avcodec_open2(ctx, dec, nullptr) >= 0) {
                // Target timestamp for frameNo; seek to the keyframe before it,
                // then decode forward until we reach (or pass) the target.
                const AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
                const double fps = fr.den ? double(fr.num) / fr.den : 0.0;
                const int64_t startTs = (st->start_time != AV_NOPTS_VALUE) ? st->start_time : 0;
                int64_t targetTs = startTs;
                if (frameNo > 0 && fps > 0.0 && st->time_base.den) {
                    targetTs += int64_t((frameNo / fps) / av_q2d(st->time_base));
                    av_seek_frame(fmt, vs, targetTs, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(ctx);
                }
                AVPacket* pkt = av_packet_alloc();
                AVFrame*  frame = av_frame_alloc();
                bool got = false;
                while (!got && av_read_frame(fmt, pkt) >= 0) {
                    if (pkt->stream_index == vs && avcodec_send_packet(ctx, pkt) >= 0) {
                        while (avcodec_receive_frame(ctx, frame) >= 0) {
                            const int64_t pts = frame->best_effort_timestamp;
                            if (frameNo <= 0 || pts == AV_NOPTS_VALUE || pts >= targetTs) {
                                result = frameToImage(frame, maxW); got = true; break;
                            }
                        }
                    }
                    av_packet_unref(pkt);
                }
                if (!got) {   // drain
                    avcodec_send_packet(ctx, nullptr);
                    if (avcodec_receive_frame(ctx, frame) >= 0) result = frameToImage(frame, maxW);
                }
                av_frame_free(&frame); av_packet_free(&pkt);
            }
            if (ctx) avcodec_free_context(&ctx);
        }
    }
    avformat_close_input(&fmt);
    return result;
}

QString MediaStore::resolvePath(const QString& src) const {
    if (src.startsWith(QLatin1String(".minnotes/")))
        return docDir_ + QStringLiteral("/") + src;
    return src;                            // absolute reference
}

QString MediaStore::resolveUrl(const QString& src) const {
    if (src.isEmpty()) return {};
    if (src.startsWith(QLatin1String("http://")) || src.startsWith(QLatin1String("https://")))
        return src;                       // remote (pasted <img>) — load directly until localized
    return QUrl::fromLocalFile(resolvePath(src)).toString();
}
