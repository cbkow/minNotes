#pragma once
#include <QString>
#include <QImage>

// Media asset bookkeeping for one document. Media is NEVER ingested into the DB
// (no byte bloat): file-referenced media is stored as an absolute path; pasted /
// clipboard media (no filesystem home) is written to a hidden `.minnotes/` folder
// beside the document, content-hashed (so the same paste dedupes) and referenced
// by a path relative to the document. `resolveUrl` turns a stored src back into a
// loadable file:// URL. Paths are kept relative-to-document so they survive the
// future open/new-file flow. (Built for images; video/poster will extend it.)
class MediaStore {
public:
    struct ImageRef { QString src; int w = 0; int h = 0; bool ok() const { return !src.isEmpty() && w > 0 && h > 0; } };
    // Video descriptor: dims + transport metadata (duration/fps/frame-count) so
    // the inline poster reserves correct height and the transport bar has its
    // range without re-probing. Like images, the file is referenced in place —
    // video is never copied into .minnotes (no byte bloat).
    struct VideoRef { QString src; int w = 0; int h = 0; qint64 durationMs = 0;
        int frames = 0; double fps = 0.0;
        bool ok() const { return !src.isEmpty() && w > 0 && h > 0; } };
    // PDF descriptor: page count + page-0 point size (for the inline fit-width
    // aspect). Referenced in place like images/video. w/h are PDF points.
    struct PdfRef { QString src; int pages = 0; int w = 0; int h = 0;
        bool ok() const { return !src.isEmpty() && pages > 0 && w > 0 && h > 0; } };

    explicit MediaStore(const QString& docPath);

    // True if the path's extension is a recognized video container.
    static bool isVideoPath(const QString& path);
    // True if the path's extension is .pdf.
    static bool isPdfPath(const QString& path);

    // Reference an existing PDF: probe page count + page-0 size via QPdfDocument.
    PdfRef importPdfFile(const QString& fileUrlOrPath) const;

    // Reference an existing file (a drag-drop). src = absolute path; probes dims.
    ImageRef importFile(const QString& fileUrlOrPath) const;
    // Reference an existing video file. Probes dims + duration/fps/frames via
    // libavformat (header-only: open + find_stream_info; no decode, no threads).
    VideoRef importVideoFile(const QString& fileUrlOrPath) const;

    // Decode a single frame (software) of a video to an RGBA image — used for
    // the inline poster thumbnail (frame 0 at rest; the remembered playhead
    // after playback). Seeks to the keyframe before `frameNo` and decodes
    // forward to it. `maxW` caps the width (0 = source size). Static: no doc
    // context needed (absolute path in). Returns a null QImage on failure.
    static QImage extractFrame(const QString& path, int frameNo, int maxW = 0);
    // Render one PDF page to an RGBA image (fit `maxW` wide), for the inline page
    // poster — served through an image provider so it caches across delegate
    // recycle (a PdfPageImage tied to a churning PdfDocument blanks on recycle).
    // Static + self-contained (loads a local QPdfDocument on the caller thread).
    static QImage renderPdfPage(const QString& path, int page, int maxW = 0);
    // Save a decoded image to `.minnotes/<sha>.png` (content-hashed; dedups),
    // returning a doc-relative src + dims. Used by clipboard paste AND by HTML
    // paste (a data:/embedded image already decoded by QTextDocument).
    ImageRef importImage(const QImage& img) const;
    // Read the system clipboard image, save it to `.minnotes/<sha>.png`, return a
    // doc-relative src + dims. Invalid ref if the clipboard has no image.
    ImageRef importClipboardImage() const;

    // Stored src (absolute, or ".minnotes/…" relative) → a loadable file:// URL.
    QString resolveUrl(const QString& src) const;

private:
    QString assetsDir() const;            // <docDir>/.minnotes (created on demand)
    QString resolvePath(const QString& src) const;
    QString docDir_;
};
