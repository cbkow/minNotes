// .mnpkg — the app's interchange package: a zip carrying `document.mndb` +
// a `media/` tree + `manifest.json`. Media entries are STORED (pre-compressed
// bytes; repack at disk-copy speed), db + manifest DEFLATE. This is the ONE
// archive layer — the packer (Lane A) and the archive-shaped importers
// (Notion zips, Lane B) both sit on it.
//
// Backend: vendored miniz (external/miniz — zip64 + chunked file streaming).
// Qt's private QZip was the first backend but caps archives at 4 GiB (no
// zip64) and buffers whole entries in RAM — real packages with video
// outgrew it. PackageWriter/extractArchive/readManifest are the narrow seam;
// nothing above this layer knows the backend.
//
// Why `media/` and not `.minnotes/` verbatim: the original QZipReader backend
// SANITIZED leading-dot path components; the layout contract predates the
// miniz swap and stays (it's also friendlier to humans peeking inside). The
// package-open path bridges the naming with ONE QDir::rename of extracted
// `media/` → `.minnotes/`, after which every relative descriptor resolves
// with zero rewriting. Mid-path dots (`media/.qcview/clip.mp4/`) pack
// verbatim — the sidecar convention survives untouched.
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <functional>

namespace mnpkg {

inline constexpr int kFormatVersion = 1;

// Package entry names (layout contract, format v1).
inline constexpr const char* kDbEntry       = "document.mndb";
inline constexpr const char* kManifestEntry = "manifest.json";
inline constexpr const char* kMediaDir      = "media";   // tree prefix (see header note)

// ".mnpkg" extension (path or file:// URL, case-insensitive).
bool isPackagePath(const QString& pathOrUrl);

// The package's manifest.json as an object ({} = unreadable/absent — caller
// decides how lenient to be; format v1 readers tolerate a missing manifest).
QJsonObject readManifest(const QString& zipPath);
// A fresh manifest for writing.
QJsonObject makeManifest(int mediaCount, qint64 mediaBytes);

// Extract every entry under destDir, streaming to disk. Zip-slip guarded:
// absolute-path or `..`-traversal entries fail the WHOLE extraction and
// remove destDir — a hostile package never leaves partial output. False on
// any IO error.
bool extractArchive(const QString& zipPath, const QString& destDir);

// --- Lazy-open primitives (hand-off packages: open instantly, move media
// bytes only when something actually needs them) ---

// Extract ONE entry to `outFilePath` (parent dirs created). False if the
// entry is absent or IO fails. Writes to a sibling temp + atomic rename, so
// concurrent extractions of the same entry can't tear the file.
bool extractEntry(const QString& zipPath, const QString& entryName,
                  const QString& outFilePath);
// Extract every entry whose name starts with `entryPrefix` to
// destRoot/<name minus stripPrefix>. Returns the number extracted (0 is
// fine — e.g. a video with no sidecar). Escaping entries are skipped.
int extractMatching(const QString& zipPath, const QString& entryPrefix,
                    const QString& stripPrefix, const QString& destRoot);


// Atomic same-directory replace (POSIX rename(2) / Win32 MoveFileEx with
// REPLACE_EXISTING) — the save write-back primitive, shared with BlockModel.
bool atomicReplace(const QString& src, const QString& dst);

// Entry name → uncompressed size for every file entry (empty on unreadable
// archive). The exporter's NO-SIDE-EFFECT plan resolution: whether a lazily
// opened package still holds a media file, without extracting it.
QHash<QString, qint64> entrySizes(const QString& zipPath);

// Streaming package writer: per-entry STORE/DEFLATE, zip64 when needed,
// entries stream from disk in chunks (no RAM spike). Write everything, then
// finish(); any failed add poisons the writer (ok() false, finish() false).
class PackageWriter {
public:
    explicit PackageWriter(const QString& zipPath);
    ~PackageWriter();
    PackageWriter(const PackageWriter&) = delete;
    PackageWriter& operator=(const PackageWriter&) = delete;

    bool ok() const { return ok_; }
    qint64 bytesAdded() const { return bytes_; }

    // Media path: entry STORED verbatim from a file on disk.
    bool addStoredFile(const QString& entryName, const QString& srcFilePath);
    // Same, streamed through a read callback so multi-GB files report
    // progress per chunk and honour cancel MID-FILE (a cancelled add poisons
    // the writer — the caller discards the archive). `onBytes` receives the
    // running byte count within this file.
    bool addStoredFileChunked(const QString& entryName, const QString& srcFilePath,
                              const std::function<void(qint64)>& onBytes,
                              const std::atomic<bool>* cancelFlag);
    // db/manifest path: entry DEFLATEd from bytes.
    bool addCompressed(const QString& entryName, const QByteArray& bytes);
    bool addCompressedFile(const QString& entryName, const QString& srcFilePath);
    // RAW-splice one entry (or every entry under a prefix) from another
    // archive — no recompress, no CRC recompute, entry names kept. The
    // export path for media still sitting inside a lazily-opened source
    // package. splicePrefixFrom returns entries spliced (0 = none matched;
    // not an error).
    bool spliceFrom(const QString& srcZipPath, const QString& entryName);
    int splicePrefixFrom(const QString& srcZipPath, const QString& prefix);

    bool finish();   // finalize + close; false if anything failed

private:
    void* zip_ = nullptr;   // mz_zip_archive* (kept out of this header)
    qint64 bytes_ = 0;
    bool ok_ = false;
    bool add(const QString& entryName, const QString& srcFilePath, bool store);
};

} // namespace mnpkg
