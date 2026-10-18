#pragma once
// The CANVAS exporter (2.0 step 8): one self-contained HTML file — the whole canvas as an absolutely
// positioned world you can pan and zoom in a browser — and a PNG poster. Pages are the PAGE export
// (Exporter::toHtml in fragment mode under the shared page CSS), positioned at their element rects
// and capped at their card height like the app; media elements are figures (an image, a video's
// poster frame, a PDF's first page) or cards (file, folder, bookmark, audio) that ALWAYS print the
// plain path (ruling: a path a reader can copy is the baseline) and the ufb:/// deep link only when
// asked; ink is SVG; text chips are text. No JavaScript is needed to read it; ~60 lines pan and zoom.
#include "Exporter.h"
#include <QImage>
#include <QObject>
#include <QString>
#include <QVariantMap>

class CanvasDocument;

class CanvasExporter : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
public:
    explicit CanvasExporter(QObject* parent = nullptr) : QObject(parent) {}
    void setCanvas(CanvasDocument* c) { canvas_ = c; }
    QString lastError() const { return lastError_; }

    struct Options {
        bool includeVideoNotes = true;   // (the page fragments' video-note cards)
        bool ufbLinks = false;           // ufb:///{os}/{path} under every printed path
        int posterWidth = 1600;          // the poster's pixel width
    };
    // Pure generation (testable): the canvas as one HTML document; assets through `sink`.
    QString toHtml(const Options& opt, Exporter::AssetSink& sink) const;
    // The poster: the world scaled to `posterWidth` — pages as sheets with their title and greek bars
    // (the zoomed-out canvas's tier-1 look), media thumbnails, ink through the app's own painter.
    QImage toPoster(const Options& opt) const;
    Q_INVOKABLE QVariantMap scan() const;   // {pages, media, videos, pdfs, ink, shapes, refPaths}
    Q_INVOKABLE bool exportHtml(const QString& fileUrlOrPath, bool includeVideoNotes, bool ufbLinks = false);
    Q_INVOKABLE bool exportPoster(const QString& fileUrlOrPath, int width = 1600);
signals:
    void lastErrorChanged();
private:
    bool fail(const QString& why);
    CanvasDocument* canvas_ = nullptr;
    QString lastError_;
};
