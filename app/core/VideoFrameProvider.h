#pragma once
#include <QQuickImageProvider>
#include <QByteArray>
#include "MediaStore.h"

// Serves inline video poster thumbnails to QML. A QML Image with
//   source: "image://videoframe/<base64url(path)>@<frame>"
// decodes that frame (software, off the GUI thread) via MediaStore::extractFrame.
// The local path is base64url-encoded so slashes/spaces survive the image URL.
class VideoFrameProvider : public QQuickImageProvider {
public:
    VideoFrameProvider()
        : QQuickImageProvider(QQuickImageProvider::Image,
                              QQuickImageProvider::ForceAsynchronousImageLoading) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
        const int at = id.lastIndexOf('@');
        const QString b64   = (at >= 0) ? id.left(at) : id;
        const int     frame = (at >= 0) ? id.mid(at + 1).toInt() : 0;
        const QString path = QString::fromUtf8(
            QByteArray::fromBase64(b64.toUtf8(), QByteArray::Base64UrlEncoding));
        const int maxW = requestedSize.width() > 0 ? requestedSize.width() : 760;
        QImage img = MediaStore::extractFrame(path, frame, maxW);
        if (size) *size = img.size();
        return img;
    }
};
