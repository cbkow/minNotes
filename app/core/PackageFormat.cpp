#include "PackageFormat.h"

#include "miniz.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QUrl>

#include <cstdio>
#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace mnpkg {

bool isPackagePath(const QString& pathOrUrl) {
    const QString p = pathOrUrl.startsWith(QLatin1String("file:"))
                          ? QUrl(pathOrUrl).toLocalFile() : pathOrUrl;
    return p.endsWith(QLatin1String(".mnpkg"), Qt::CaseInsensitive);
}

QJsonObject readManifest(const QString& zipPath) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0)) return {};
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, kManifestEntry, &size, 0);
    QJsonObject out;
    if (data) {
        out = QJsonDocument::fromJson(
                  QByteArray(static_cast<const char*>(data),
                             static_cast<qsizetype>(size))).object();
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    return out;
}

QJsonObject makeManifest(int mediaCount, qint64 mediaBytes) {
    QJsonObject o;
    o.insert(QStringLiteral("format"), QStringLiteral("mnpkg"));
    o.insert(QStringLiteral("formatVersion"), kFormatVersion);
    o.insert(QStringLiteral("appVersion"), QCoreApplication::applicationVersion());
    o.insert(QStringLiteral("created"),
             QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    o.insert(QStringLiteral("mediaCount"), mediaCount);
    o.insert(QStringLiteral("mediaBytes"), static_cast<double>(mediaBytes));
    return o;
}

// True for entry names that could write outside the extraction root: absolute,
// drive-absolute, or `..`-traversing (after cleanPath, so `a/../../x` is
// caught while a literal `notes..md` filename is not).
static bool entryEscapes(const QString& name) {
    if (name.isEmpty()) return true;
    if (name.startsWith(QLatin1Char('/')) || name.startsWith(QLatin1Char('\\'))) return true;
    if (name.size() >= 2 && name.at(1) == QLatin1Char(':')) return true;
    const QString clean = QDir::cleanPath(name);
    return clean == QLatin1String("..") || clean.startsWith(QLatin1String("../"));
}

bool extractArchive(const QString& zipPath, const QString& destDir) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0)) return false;
    // On ANY bad entry or IO error: remove everything written so a hostile or
    // unsupported archive never leaves partial output behind.
    auto fail = [&] {
        mz_zip_reader_end(&zip);
        QDir(destDir).removeRecursively();
        return false;
    };
    if (!QDir().mkpath(destDir)) return fail();
    const QDir dest(destDir);
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    // Hostile-package guard FIRST — one escaping entry fails the whole extract.
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) return fail();
        if (entryEscapes(QString::fromUtf8(st.m_filename))) return fail();
    }
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) return fail();
        const QString name = QString::fromUtf8(st.m_filename);
        const QString outPath = dest.filePath(name);
        if (st.m_is_directory) {
            if (!QDir().mkpath(outPath)) return fail();
            continue;
        }
        if (!QDir().mkpath(QFileInfo(outPath).absolutePath())) return fail();
        if (!mz_zip_reader_extract_to_file(&zip, i, outPath.toUtf8().constData(), 0))
            return fail();
    }
    mz_zip_reader_end(&zip);
    return true;
}

bool extractEntry(const QString& zipPath, const QString& entryName,
                  const QString& outFilePath) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0)) return false;
    const int idx = mz_zip_reader_locate_file(&zip, entryName.toUtf8().constData(),
                                              nullptr, 0);
    bool ok = false;
    if (idx >= 0) {
        QDir().mkpath(QFileInfo(outFilePath).absolutePath());
        // Temp + atomic rename: a concurrent extraction of the same entry
        // (e.g. poster thread + player) can't observe a half-written file.
        const QString tmp = outFilePath + QStringLiteral(".mn-extract-tmp");
        ok = mz_zip_reader_extract_to_file(&zip, static_cast<mz_uint>(idx),
                                           tmp.toUtf8().constData(), 0)
             && (atomicReplace(tmp, outFilePath)
                 || QFileInfo::exists(outFilePath));   // lost the race → fine
        if (!ok) QFile::remove(tmp);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

int extractMatching(const QString& zipPath, const QString& entryPrefix,
                    const QString& stripPrefix, const QString& destRoot) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0)) return 0;
    int count = 0;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        const QString name = QString::fromUtf8(st.m_filename);
        if (!name.startsWith(entryPrefix) || entryEscapes(name)) continue;
        QString rel = name;
        if (rel.startsWith(stripPrefix)) rel = rel.mid(stripPrefix.size());
        const QString out = destRoot + QLatin1Char('/') + rel;
        if (QFileInfo::exists(out)) continue;   // disk wins (possibly edited)
        QDir().mkpath(QFileInfo(out).absolutePath());
        const QString tmp = out + QStringLiteral(".mn-extract-tmp");
        if (mz_zip_reader_extract_to_file(&zip, i, tmp.toUtf8().constData(), 0)
            && (atomicReplace(tmp, out) || QFileInfo::exists(out)))
            ++count;
        else
            QFile::remove(tmp);
    }
    mz_zip_reader_end(&zip);
    return count;
}

bool atomicReplace(const QString& src, const QString& dst) {
#ifdef Q_OS_WIN
    return MoveFileExW(reinterpret_cast<const wchar_t*>(src.utf16()),
                       reinterpret_cast<const wchar_t*>(dst.utf16()),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(src.toUtf8().constData(), dst.toUtf8().constData()) == 0;
#endif
}

QHash<QString, qint64> entrySizes(const QString& zipPath) {
    QHash<QString, qint64> out;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0)) return out;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st) && !st.m_is_directory)
            out.insert(QString::fromUtf8(st.m_filename),
                       static_cast<qint64>(st.m_uncomp_size));
    }
    mz_zip_reader_end(&zip);
    return out;
}

QHash<QString, EntrySpan> entrySpans(const QString& zipPath) {
    QHash<QString, EntrySpan> out;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0)) return out;
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly)) { mz_zip_reader_end(&zip); return out; }
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        // Data offset = local header + fixed 30 bytes + ITS name/extra
        // lengths (fields 26..29; zip64 stores may carry a local extra
        // field the central directory doesn't mention).
        unsigned char hdr[30];
        if (!f.seek(static_cast<qint64>(st.m_local_header_ofs))
            || f.read(reinterpret_cast<char*>(hdr), 30) != 30
            || !(hdr[0] == 'P' && hdr[1] == 'K' && hdr[2] == 3 && hdr[3] == 4))
            continue;
        const int nameLen  = hdr[26] | (hdr[27] << 8);
        const int extraLen = hdr[28] | (hdr[29] << 8);
        EntrySpan span;
        span.offset = static_cast<qint64>(st.m_local_header_ofs) + 30 + nameLen + extraLen;
        span.size = static_cast<qint64>(st.m_uncomp_size);
        span.stored = (st.m_method == 0);
        out.insert(QString::fromUtf8(st.m_filename), span);
    }
    mz_zip_reader_end(&zip);
    return out;
}

QByteArray readEntry(const QString& zipPath, const QString& entryName) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0)) return {};
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(
        &zip, entryName.toUtf8().constData(), &size, 0);
    QByteArray out;
    if (data) {
        out = QByteArray(static_cast<const char*>(data),
                         static_cast<qsizetype>(size));
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    return out;
}

static mz_zip_archive* Z(void* p) { return static_cast<mz_zip_archive*>(p); }

PackageWriter::PackageWriter(const QString& zipPath) {
    QFile::remove(zipPath);
    auto* zip = new mz_zip_archive;
    memset(zip, 0, sizeof(*zip));
    zip_ = zip;
    ok_ = mz_zip_writer_init_file_v2(zip, zipPath.toUtf8().constData(), 0,
                                     MZ_ZIP_FLAG_WRITE_ZIP64);
}

PackageWriter::~PackageWriter() {
    if (zip_) {
        mz_zip_writer_end(Z(zip_));   // safe after finalize; frees state
        delete Z(zip_);
        zip_ = nullptr;
    }
}

bool PackageWriter::add(const QString& entryName, const QString& srcFilePath, bool store) {
    if (!ok_) return false;
    const qint64 sz = QFileInfo(srcFilePath).size();
    ok_ = mz_zip_writer_add_file(Z(zip_), entryName.toUtf8().constData(),
                                 srcFilePath.toUtf8().constData(), nullptr, 0,
                                 store ? MZ_NO_COMPRESSION : MZ_DEFAULT_LEVEL);
    if (ok_) bytes_ += sz;
    return ok_;
}

bool PackageWriter::addStoredFile(const QString& entryName, const QString& srcFilePath) {
    return add(entryName, srcFilePath, /*store*/true);
}

namespace {
struct ChunkCtx {
    QFile* file;
    const std::function<void(qint64)>* onBytes;
    const std::atomic<bool>* cancel;
};
// miniz pull-callback: sequential-ish reads at file_ofs. Returning short
// aborts the add (how cancel lands mid-file).
size_t chunkRead(void* opaque, mz_uint64 ofs, void* buf, size_t n) {
    auto* c = static_cast<ChunkCtx*>(opaque);
    if (c->cancel && c->cancel->load()) return 0;
    if (!c->file->seek(static_cast<qint64>(ofs))) return 0;
    const qint64 got = c->file->read(static_cast<char*>(buf), static_cast<qint64>(n));
    if (got <= 0) return 0;
    if (c->onBytes && *c->onBytes) (*c->onBytes)(static_cast<qint64>(ofs) + got);
    return static_cast<size_t>(got);
}
} // namespace

bool PackageWriter::addStoredFileChunked(const QString& entryName,
                                         const QString& srcFilePath,
                                         const std::function<void(qint64)>& onBytes,
                                         const std::atomic<bool>* cancelFlag) {
    if (!ok_) return false;
    QFile f(srcFilePath);
    if (!f.open(QIODevice::ReadOnly)) { ok_ = false; return false; }
    ChunkCtx ctx{&f, &onBytes, cancelFlag};
    ok_ = mz_zip_writer_add_read_buf_callback(
        Z(zip_), entryName.toUtf8().constData(), chunkRead, &ctx,
        static_cast<mz_uint64>(f.size()), nullptr, nullptr, 0,
        MZ_NO_COMPRESSION, nullptr, 0, nullptr, 0);
    if (ok_) bytes_ += f.size();
    return ok_;
}

bool PackageWriter::addCompressedFile(const QString& entryName, const QString& srcFilePath) {
    return add(entryName, srcFilePath, /*store*/false);
}

bool PackageWriter::addCompressed(const QString& entryName, const QByteArray& bytes) {
    if (!ok_) return false;
    ok_ = mz_zip_writer_add_mem(Z(zip_), entryName.toUtf8().constData(),
                                bytes.constData(), static_cast<size_t>(bytes.size()),
                                MZ_DEFAULT_LEVEL);
    if (ok_) bytes_ += bytes.size();
    return ok_;
}

bool PackageWriter::spliceFrom(const QString& srcZipPath, const QString& entryName) {
    if (!ok_) return false;
    mz_zip_archive src;
    memset(&src, 0, sizeof(src));
    if (!mz_zip_reader_init_file(&src, srcZipPath.toUtf8().constData(), 0)) {
        ok_ = false;
        return false;
    }
    const int idx = mz_zip_reader_locate_file(&src, entryName.toUtf8().constData(),
                                              nullptr, 0);
    ok_ = idx >= 0 && mz_zip_writer_add_from_zip_reader(Z(zip_), &src,
                                                        static_cast<mz_uint>(idx));
    if (ok_) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&src, static_cast<mz_uint>(idx), &st))
            bytes_ += static_cast<qint64>(st.m_uncomp_size);
    }
    mz_zip_reader_end(&src);
    return ok_;
}

int PackageWriter::splicePrefixFrom(const QString& srcZipPath, const QString& prefix) {
    if (!ok_) return 0;
    mz_zip_archive src;
    memset(&src, 0, sizeof(src));
    if (!mz_zip_reader_init_file(&src, srcZipPath.toUtf8().constData(), 0)) return 0;
    int count = 0;
    const mz_uint n = mz_zip_reader_get_num_files(&src);
    for (mz_uint i = 0; i < n && ok_; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&src, i, &st) || st.m_is_directory) continue;
        if (!QString::fromUtf8(st.m_filename).startsWith(prefix)) continue;
        ok_ = mz_zip_writer_add_from_zip_reader(Z(zip_), &src, i);
        if (ok_) { ++count; bytes_ += static_cast<qint64>(st.m_uncomp_size); }
    }
    mz_zip_reader_end(&src);
    return count;
}

bool PackageWriter::finish() {
    if (!zip_) return false;
    const bool good = ok_ && mz_zip_writer_finalize_archive(Z(zip_));
    mz_zip_writer_end(Z(zip_));
    delete Z(zip_);
    zip_ = nullptr;
    return good;
}

} // namespace mnpkg
