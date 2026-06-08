#pragma once
#include <QQuickImageProvider>
#include <QByteArray>
#include "MediaStore.h"

// Serves inline PDF page images to QML. A QML Image with
//   source: "image://pdfpage/<base64url(path)>@<page>"
// renders that page (off the GUI thread) via MediaStore::renderPdfPage. Going
// through an image provider (cache:true, keyed by the URL) makes the inline page
// survive delegate recycle — a PdfPageImage bound to a churning PdfDocument
// blanks to white when scrolled away and back. Mirrors VideoFrameProvider.
class PdfPageProvider : public QQuickImageProvider {
public:
    PdfPageProvider()
        : QQuickImageProvider(QQuickImageProvider::Image,
                              QQuickImageProvider::ForceAsynchronousImageLoading) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
        const int at = id.lastIndexOf('@');
        const QString b64  = (at >= 0) ? id.left(at) : id;
        const int     page = (at >= 0) ? id.mid(at + 1).toInt() : 0;
        const QString path = QString::fromUtf8(
            QByteArray::fromBase64(b64.toUtf8(), QByteArray::Base64UrlEncoding));
        const int maxW = requestedSize.width() > 0 ? requestedSize.width() : 760;
        QImage img = MediaStore::renderPdfPage(path, page, maxW);
        if (size) *size = img.size();
        return img;
    }
};
