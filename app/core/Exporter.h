// Exporter — the document walker + per-format emitters (export arc, v1:
// Markdown). Reads the LOADED BlockModel through its public seam (never the
// raw DB — the model is canonical: markdown consumed to spans at load, media
// resolved via MediaStore, comments in memory).
//
// Shape: toMarkdown() is pure string generation over an AssetSink seam so the
// regression suite can drive it headless with a recording sink; the
// Q_INVOKABLE exportMarkdown() wraps it with a real file/assets-folder sink
// (`<name>.assets/` beside the output). scan() is the Export dialog's
// pre-scan: it reports what the document carries (video notes, ink, …) so
// options only appear when they apply.
//
// Format notes (design chat 2026-07-11):
//   - color/highlight spans DROP in markdown (HTML export carries fidelity).
//   - comments → footnotes ([^n] at the span end, thread messages as body).
//   - video/pdf/file → kind-labeled fenced REFERENCE blocks (filename +
//     resolved absolute path + probe metadata), poster image above.
//   - video notes (optional, detection-driven): QCView sidecar notes inline
//     under the video's reference block — annotated thumb + timecode + text.
//   - sketches rasterize to the assets folder; margin ink is skipped with an
//     HTML-comment note (rasterization is a later option).

#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QVariantMap>

class BlockModel;

class Exporter : public QObject {
    Q_OBJECT
public:
    explicit Exporter(QObject* parent = nullptr);

    void setModel(BlockModel* m) { model_ = m; }

    struct Options {
        bool includeVideoNotes = true;
    };

    // Where emitted assets go. addFile/addImage return the path to write into
    // the markdown ("" = failed; caller degrades gracefully). Implementations
    // own name-uniquing.
    class AssetSink {
    public:
        virtual ~AssetSink() = default;
        virtual QString addFile(const QString& srcPath, const QString& baseName) = 0;
        virtual QString addImage(const QImage& img, const QString& baseName) = 0;
    };

    // Pure generation (testable): the whole document as markdown, assets
    // routed through `sink`.
    QString toMarkdown(const Options& opt, AssetSink& sink) const;

    // Export-dialog pre-scan: {videos, videosWithNotes, videoNotes,
    // inkBlocks, sketches}. Cheap — sidecar JSON reads only.
    Q_INVOKABLE QVariantMap scan() const;

    // Write `<path>.md` + `<basename>.assets/` beside it. Accepts a file://
    // URL or a plain path. False on any write failure.
    Q_INVOKABLE bool exportMarkdown(const QString& fileUrlOrPath,
                                    bool includeVideoNotes);

private:
    BlockModel* model_ = nullptr;
};
