#pragma once
#include <QString>

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

    explicit MediaStore(const QString& docPath);

    // Reference an existing file (a drag-drop). src = absolute path; probes dims.
    ImageRef importFile(const QString& fileUrlOrPath) const;
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
