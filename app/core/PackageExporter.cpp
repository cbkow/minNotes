#include "PackageExporter.h"
#include "BlockModel.h"
#include "Document.h"
#include "MediaStore.h"
#include "PackageFormat.h"
#include "TableGrid.h"
#include "../notes/annotation_io.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QUrl>

// NO-SIDE-EFFECT plan resolution for one descriptor src (public static since
// the tab-merge program — DocumentMerger plans with the same rules). Never
// extracts.
using Resolved = PackageExporter::Resolved;

Resolved PackageExporter::resolveNoExtract(MediaStore* store, const QJsonValue& src,
                                           const QHash<QString, qint64>& pkgEntries) {
    Resolved r;
    if (!store) return r;
    const QString s = src.isObject() ? QString() : src.toString();
    // {vol,rel} refs and absolute strings resolve side-effect-free through
    // the normal resolver; only relative sidecar srcs need the no-extract
    // package handling below.
    if (src.isObject() || !s.startsWith(QLatin1String(".minnotes/"))) {
        const QString url = store->resolveUrl(src.isObject() ? src : QJsonValue(s));
        if (url.isEmpty() || !url.startsWith(QLatin1String("file:"))) return r;
        const QString p = QUrl(url).toLocalFile();
        if (!QFileInfo::exists(p)) return r;
        r.absPath = QFileInfo(p).absoluteFilePath();
        r.bytes = QFileInfo(p).size();
        r.ok = true;
        return r;
    }
    // Relative sidecar src: compute the disk path directly (no resolver side
    // effects), then fall back to the source archive's entry table.
    const QString path = QDir::cleanPath(store->docDir() + QLatin1Char('/') + s);
    if (QFileInfo::exists(path)) {
        r.absPath = path;
        r.bytes = QFileInfo(path).size();
        r.ok = true;
        return r;
    }
    const QString entry = QStringLiteral("media/") + s.mid(int(qstrlen(".minnotes/")));
    const auto it = pkgEntries.constFind(entry);
    if (it != pkgEntries.constEnd()) {
        r.absPath = path;
        r.packageEntry = entry;
        r.bytes = it.value();
        r.ok = true;
    }
    return r;
}

// FileSink::unique pattern: keep the readable basename, suffix -2/-3/… on
// collision. Case-folded (zip consumers on case-insensitive filesystems).
QString PackageExporter::uniqueName(const QString& fileName, QSet<QString>& taken) {
    const QFileInfo fi(fileName);
    const QString stem = fi.completeBaseName().isEmpty()
                             ? QStringLiteral("media") : fi.completeBaseName();
    const QString ext = fi.suffix().isEmpty() ? QString()
                                              : QLatin1Char('.') + fi.suffix();
    QString name = stem + ext;
    for (int n = 2; taken.contains(name.toLower()); ++n)
        name = stem + QLatin1Char('-') + QString::number(n) + ext;
    taken.insert(name.toLower());
    return name;
}

namespace {

// One media source joins the plan (deduped by its would-be local path).
void planItem(PackageExporter::PackPlan& plan, QSet<QString>& taken,
              const Resolved& res, bool isVideo, bool includeVideos) {
    if (!res.ok || plan.packedBySrc.contains(res.absPath)) return;
    if (isVideo) { plan.videoCount++; plan.videoBytes += res.bytes; }
    if (isVideo && !includeVideos) { plan.excludedVideos++; return; }

    PackageExporter::PackItem it;
    it.srcPath = res.packageEntry.isEmpty() ? res.absPath : QString();
    it.packageEntry = res.packageEntry;
    it.packedName = PackageExporter::uniqueName(QFileInfo(res.absPath).fileName(), taken);
    it.bytes = res.bytes;
    it.isVideo = isVideo;
    if (isVideo && it.packageEntry.isEmpty()) {
        // dirname(media)/.qcview/<sanitized name>/ — include the whole tree
        // when it exists (notes.json + images/). Content never touched.
        // Splice items carry their sidecar by ENTRY PREFIX at zip time.
        const QString dir =
            QFileInfo(qcv::annotation_io::getNotesJsonPath(res.absPath)).absolutePath();
        if (QFileInfo::exists(dir)) it.sidecarDir = dir;
    }
    plan.totalBytes += res.bytes;
    plan.packedBySrc.insert(res.absPath, it.packedName);
    plan.items.push_back(std::move(it));
}

// Rewrite one descriptor object's "src" to `.minnotes/<packedName>` when its
// no-extract resolution is in the plan. Returns true if changed.
bool rewriteSrc(QJsonObject& o, MediaStore* store,
                const PackageExporter::PackPlan& plan,
                const QHash<QString, qint64>& pkgEntries) {
    const Resolved res = PackageExporter::resolveNoExtract(
        store, o.value(QStringLiteral("src")), pkgEntries);
    if (!res.ok) return false;
    const auto it = plan.packedBySrc.constFind(res.absPath);
    if (it == plan.packedBySrc.constEnd()) return false;
    o.insert(QStringLiteral("src"), QStringLiteral(".minnotes/") + it.value());
    return true;
}

} // namespace

PackageExporter::~PackageExporter() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

QVariantMap PackageExporter::scan() const {
    QVariantMap out;
    const PackPlan plan = buildPackPlan(model_, /*includeVideos*/true);
    out.insert(QStringLiteral("videos"), plan.videoCount);
    out.insert(QStringLiteral("videoBytes"), plan.videoBytes);
    out.insert(QStringLiteral("mediaCount"), static_cast<int>(plan.items.size()));
    out.insert(QStringLiteral("mediaBytes"), plan.totalBytes);
    return out;
}

PackageExporter::PackPlan PackageExporter::buildPackPlan(BlockModel* m,
                                                         bool includeVideos) {
    PackPlan plan;
    if (!m || !m->mediaStore()) return plan;
    MediaStore* store = m->mediaStore();
    plan.sourcePackage = store->packageSource();
    const QHash<QString, qint64> pkgEntries =
        plan.sourcePackage.isEmpty() ? QHash<QString, qint64>()
                                     : mnpkg::entrySizes(plan.sourcePackage);
    QSet<QString> taken;
    const int n = m->rowCountQml();
    for (int r = 0; r < n; ++r) {
        const int t = m->typeForRow(r);
        if (t == BlockModel::Media) {
            const QJsonObject root =
                QJsonDocument::fromJson(m->contentForRow(r).toUtf8()).object();
            if (root.value(QStringLiteral("kind")).toString() == QLatin1String("sketch")) {
                for (const QJsonValue& v : root.value(QStringLiteral("images")).toArray())
                    planItem(plan, taken,
                             resolveNoExtract(store, v.toObject().value(QStringLiteral("src")), pkgEntries),
                             /*isVideo*/false, includeVideos);
            } else {
                const bool isVideo =
                    root.value(QStringLiteral("kind")).toString() == QLatin1String("video");
                planItem(plan, taken,
                         resolveNoExtract(store, root.value(QStringLiteral("src")), pkgEntries),
                         isVideo, includeVideos);
            }
        } else if (t == BlockModel::Table) {
            const TableGrid g = TableGrid::fromJson(m->contentForRow(r));
            for (int tr = 0; tr < g.rows(); ++tr)
                for (int tc = 0; tc < g.cols(); ++tc) {
                    const QString desc = g.cellMedia(tr, tc);
                    if (desc.isEmpty()) continue;
                    const QJsonObject o = QJsonDocument::fromJson(desc.toUtf8()).object();
                    planItem(plan, taken,
                             resolveNoExtract(store, o.value(QStringLiteral("src")), pkgEntries),
                             /*isVideo*/false, includeVideos);
                }
        }
    }
    return plan;
}

bool PackageExporter::prepareDb(BlockModel* m, const PackPlan& plan,
                                const QString& tmpDb) {
    QFile::remove(tmpDb);
    if (!m->snapshotTo(tmpDb)) return false;
    MediaStore* store = m->mediaStore();
    const QHash<QString, qint64> pkgEntries =
        plan.sourcePackage.isEmpty() ? QHash<QString, qint64>()
                                     : mnpkg::entrySizes(plan.sourcePackage);
    Document d2;
    if (!d2.open(tmpDb)) return false;
    for (const Document::BlockMeta& bm : d2.skinnyScan()) {
        if (bm.type == QLatin1String("media")) {
            QJsonObject root =
                QJsonDocument::fromJson(d2.contentFor(bm.id).toUtf8()).object();
            bool changed = false;
            if (root.value(QStringLiteral("kind")).toString() == QLatin1String("sketch")) {
                QJsonArray images = root.value(QStringLiteral("images")).toArray();
                for (int i = 0; i < images.size(); ++i) {
                    QJsonObject o = images.at(i).toObject();
                    if (rewriteSrc(o, store, plan, pkgEntries)) {
                        images.replace(i, o);
                        changed = true;
                    }
                }
                if (changed) root.insert(QStringLiteral("images"), images);
            } else {
                changed = rewriteSrc(root, store, plan, pkgEntries);
            }
            if (changed)
                d2.updateContent(bm.id, QString::fromUtf8(
                    QJsonDocument(root).toJson(QJsonDocument::Compact)));
        } else if (bm.type == QLatin1String("table")) {
            TableGrid g = TableGrid::fromJson(d2.contentFor(bm.id));
            bool changed = false;
            for (int tr = 0; tr < g.rows(); ++tr)
                for (int tc = 0; tc < g.cols(); ++tc) {
                    const QString desc = g.cellMedia(tr, tc);
                    if (desc.isEmpty()) continue;
                    QJsonObject o = QJsonDocument::fromJson(desc.toUtf8()).object();
                    if (rewriteSrc(o, store, plan, pkgEntries)) {
                        g.setCellMedia(tr, tc, QString::fromUtf8(
                            QJsonDocument(o).toJson(QJsonDocument::Compact)));
                        changed = true;
                    }
                }
            if (changed) d2.updateContent(bm.id, g.toJson());
        }
    }
    d2.checkpoint();
    d2.close();
    return true;
}

bool PackageExporter::assembleZip(const PackPlan& plan, const QString& tmpDb,
                                  const QString& tmpZip,
                                  std::atomic<bool>* cancelFlag,
                                  const std::function<void(qint64, qint64, QString)>& progress) {
    mnpkg::PackageWriter w(tmpZip);
    if (!w.addCompressedFile(QLatin1String(mnpkg::kDbEntry), tmpDb)) return false;
    qint64 done = 0;
    for (const PackItem& it : plan.items) {
        if (cancelFlag && cancelFlag->load()) return false;
        if (progress) progress(done, plan.totalBytes, it.packedName);
        const QString entry = QLatin1String(mnpkg::kMediaDir) + QLatin1Char('/')
                            + it.packedName;
        if (!it.packageEntry.isEmpty()) {
            // Splice from the source package. A dedup-renamed splice can't
            // keep its raw entry (names are baked into zip headers) — pull
            // it through a temp file instead (rare: cross-source collision).
            if (it.packedName == QFileInfo(it.packageEntry).fileName()) {
                if (!w.spliceFrom(plan.sourcePackage, it.packageEntry)) return false;
            } else {
                const QString tmp = tmpZip + QStringLiteral(".item-tmp");
                const bool ok = mnpkg::extractEntry(plan.sourcePackage, it.packageEntry, tmp)
                                && w.addStoredFile(entry, tmp);
                QFile::remove(tmp);
                if (!ok) return false;
            }
            if (it.isVideo) {
                // Sidecar rides along by entry prefix (same sanitized name —
                // the packed name matched, so the prefix does too).
                w.splicePrefixFrom(plan.sourcePackage,
                                   QStringLiteral("media/.qcview/")
                                       + qcv::annotation_io::sanitizeMediaName(it.packedName)
                                       + QLatin1Char('/'));
                if (!w.ok()) return false;
            }
        } else {
            // Chunked: multi-GB videos move the bar continuously and honour
            // cancel mid-file (short read aborts the add).
            const qint64 base = done;
            if (!w.addStoredFileChunked(entry, it.srcPath,
                    [&](qint64 fileDone) {
                        if (progress) progress(base + fileDone, plan.totalBytes,
                                               it.packedName);
                    }, cancelFlag))
                return false;
            if (!it.sidecarDir.isEmpty()) {
                const QString prefix = QLatin1String(mnpkg::kMediaDir)
                    + QStringLiteral("/.qcview/")
                    + qcv::annotation_io::sanitizeMediaName(it.packedName) + QLatin1Char('/');
                QDirIterator sit(it.sidecarDir, QDir::Files | QDir::Hidden,
                                 QDirIterator::Subdirectories);
                while (sit.hasNext()) {
                    const QString f = sit.next();
                    const QString rel = QDir(it.sidecarDir).relativeFilePath(f);
                    if (!w.addStoredFile(prefix + rel, f)) return false;
                }
            }
        }
        done += it.bytes;
        if (progress) progress(done, plan.totalBytes, it.packedName);
    }
    const QJsonObject man = mnpkg::makeManifest(
        static_cast<int>(plan.items.size()), plan.totalBytes);
    return w.addCompressed(QLatin1String(mnpkg::kManifestEntry),
                           QJsonDocument(man).toJson(QJsonDocument::Compact))
           && w.finish();
}

bool PackageExporter::packDocument(BlockModel* m, const QString& destPath,
                                   bool includeVideos, QString* error) {
    auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    if (!m || !m->mediaStore()) return fail(QStringLiteral("No document"));
    const PackPlan plan = buildPackPlan(m, includeVideos);

    const QString tmpDb = BlockModel::scratchDir() + QStringLiteral("/pack-")
                        + QFileInfo(destPath).completeBaseName() + QStringLiteral(".mndb");
    if (!prepareDb(m, plan, tmpDb)) {
        QFile::remove(tmpDb);
        return fail(QStringLiteral("Snapshot failed"));
    }
    const QString destDir = QFileInfo(destPath).absolutePath();
    QDir().mkpath(destDir);
    const QString tmpZip = destDir + QStringLiteral("/.mn-pack-")
                         + QFileInfo(destPath).completeBaseName() + QStringLiteral(".tmp");
    const bool zipped = assembleZip(plan, tmpDb, tmpZip, nullptr, nullptr);
    QFile::remove(tmpDb);
    if (!zipped) {
        QFile::remove(tmpZip);
        return fail(QStringLiteral("Package write failed"));
    }
    if (!mnpkg::atomicReplace(tmpZip, destPath)) {
        QFile::remove(tmpZip);
        return fail(QStringLiteral("Package write failed"));
    }
    return true;
}

void PackageExporter::setProgress(double p, const QString& item) {
    QMetaObject::invokeMethod(this, [this, p, item] {
        progress_ = p;
        currentItem_ = item;
        emit progressChanged();
    }, Qt::QueuedConnection);
}

void PackageExporter::finishOnGui(bool ok, const QString& error) {
    QMetaObject::invokeMethod(this, [this, ok, error] {
        lastError_ = error;
        running_ = false;
        emit runningChanged();
        emit exportFinished(ok, error);
    }, Qt::QueuedConnection);
}

void PackageExporter::startExport(const QString& fileUrlOrPath, bool includeVideos) {
    if (running_ || !model_ || !model_->mediaStore()) return;
    if (worker_.joinable()) worker_.join();   // reap the previous run
    lastError_.clear();
    cancel_ = false;

    QString dest = fileUrlOrPath.startsWith(QLatin1String("file:"))
                       ? QUrl(fileUrlOrPath).toLocalFile() : fileUrlOrPath;
    if (dest.isEmpty()) { emit exportFinished(false, QStringLiteral("No path")); return; }
    if (!dest.endsWith(QLatin1String(".mnpkg"), Qt::CaseInsensitive))
        dest += QStringLiteral(".mnpkg");

    // GUI-thread phase: everything that touches the model (fast).
    const PackPlan plan = buildPackPlan(model_, includeVideos);
    const QString tmpDb = BlockModel::scratchDir() + QStringLiteral("/pack-")
                        + QFileInfo(dest).completeBaseName() + QStringLiteral(".mndb");
    if (!prepareDb(model_, plan, tmpDb)) {
        QFile::remove(tmpDb);
        lastError_ = QStringLiteral("Snapshot failed");
        emit exportFinished(false, lastError_);
        return;
    }
    const QString destDir = QFileInfo(dest).absolutePath();
    QDir().mkpath(destDir);
    const QString tmpZip = destDir + QStringLiteral("/.mn-pack-")
                         + QFileInfo(dest).completeBaseName() + QStringLiteral(".tmp");

    running_ = true;
    progress_ = 0.0;
    currentItem_.clear();
    emit runningChanged();
    emit progressChanged();

    // Worker phase: pure file IO — no model, no MediaStore.
    worker_ = std::thread([this, plan, tmpDb, tmpZip, dest] {
        // Chunked adds report every ~64KB — throttle to ~0.5% steps so the
        // GUI event queue isn't flooded by a multi-GB file.
        qint64 lastEmit = -1;
        const bool zipped = assembleZip(plan, tmpDb, tmpZip, &cancel_,
            [this, &lastEmit](qint64 doneB, qint64 totalB, QString name) {
                const qint64 step = std::max<qint64>(totalB / 200, qint64(1) << 20);
                if (lastEmit >= 0 && doneB - lastEmit < step && doneB != totalB)
                    return;
                lastEmit = doneB;
                setProgress(totalB > 0 ? double(doneB) / double(totalB) : 1.0,
                            name);
            });
        QFile::remove(tmpDb);
        bool ok = zipped;
        QString err;
        if (cancel_.load()) { ok = false; err = QStringLiteral("Cancelled"); }
        else if (!zipped)   { err = QStringLiteral("Package write failed"); }
        else if (!mnpkg::atomicReplace(tmpZip, dest)) {
            ok = false;
            err = QStringLiteral("Package write failed");
        }
        if (!ok) QFile::remove(tmpZip);
        finishOnGui(ok, err);
    });
}

// ============================ MediaCollector ============================

MediaCollector::~MediaCollector() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

MediaCollector::CollectPlan MediaCollector::buildCollectPlan(BlockModel* m,
                                                             bool includeVideos) {
    CollectPlan cp;
    if (!m || !m->mediaStore()) return cp;
    MediaStore* store = m->mediaStore();
    cp.package = !store->packageSource().isEmpty();
    if (cp.package) return cp;
    const QString root = QDir::cleanPath(store->docDir());

    // Names must dodge BOTH each other and what .minnotes already holds.
    QSet<QString> taken;
    const QDir assets(root + QStringLiteral("/.minnotes"));
    for (const QString& name : assets.entryList(QDir::Files | QDir::Dirs
                                                | QDir::NoDotAndDotDot | QDir::Hidden))
        taken.insert(name.toLower());

    // The pack plan walks every carrier and dedups by source; keep the
    // EXTERNAL disk items. (No package splice items can appear — guarded.)
    const PackageExporter::PackPlan plan =
        PackageExporter::buildPackPlan(m, /*includeVideos*/true);
    for (const PackageExporter::PackItem& it : plan.items) {
        if (it.srcPath.isEmpty()) continue;
        const QString src = QDir::cleanPath(it.srcPath);
        if (src == root || src.startsWith(root + QLatin1Char('/'))) continue;
        if (it.isVideo) {
            cp.videos++; cp.videoBytes += it.bytes;   // counted even when excluded
            if (!includeVideos) continue;
        } else {
            cp.files++; cp.fileBytes += it.bytes;
        }
        CollectItem ci;
        ci.srcPath = src;
        ci.destName = PackageExporter::uniqueName(QFileInfo(src).fileName(), taken);
        ci.sidecarDir = it.sidecarDir;
        ci.bytes = it.bytes;
        ci.isVideo = it.isVideo;
        cp.totalBytes += it.bytes;
        cp.items.push_back(std::move(ci));
    }
    return cp;
}

QVariantMap MediaCollector::scan() const {
    const CollectPlan cp = buildCollectPlan(model_, /*includeVideos*/true);
    QVariantMap out;
    out.insert(QStringLiteral("files"), cp.files);
    out.insert(QStringLiteral("fileBytes"), cp.fileBytes);
    out.insert(QStringLiteral("videos"), cp.videos);
    out.insert(QStringLiteral("videoBytes"), cp.videoBytes);
    out.insert(QStringLiteral("package"), cp.package);
    return out;
}

bool MediaCollector::copyAll(const CollectPlan& plan, const QString& assetsDir,
                             std::atomic<bool>* cancelFlag,
                             const std::function<void(qint64, qint64, QString)>& progress,
                             QString* error) {
    QStringList madeFiles, madeDirs;
    auto fail = [&](const QString& why) {
        for (const QString& f : madeFiles) QFile::remove(f);
        for (const QString& d : madeDirs) QDir(d).removeRecursively();
        if (error) *error = why;
        return false;
    };
    QDir().mkpath(assetsDir);
    qint64 done = 0;
    for (const CollectItem& it : plan.items) {
        if (cancelFlag && cancelFlag->load()) return fail(QStringLiteral("Cancelled"));
        if (progress) progress(done, plan.totalBytes, it.destName);
        const QString dest = assetsDir + QLatin1Char('/') + it.destName;
        {   // Chunked: multi-GB NAS videos move the bar and honour mid-file cancel.
            QFile in(it.srcPath), out(dest);
            if (!in.open(QIODevice::ReadOnly))
                return fail(QStringLiteral("Unreadable: ")
                            + QFileInfo(it.srcPath).fileName());
            if (!out.open(QIODevice::WriteOnly))
                return fail(QStringLiteral("Write failed: ") + it.destName);
            madeFiles << dest;
            qint64 fileDone = 0;
            while (!in.atEnd()) {
                if (cancelFlag && cancelFlag->load())
                    return fail(QStringLiteral("Cancelled"));
                const QByteArray chunk = in.read(8 << 20);
                if (chunk.isEmpty() && !in.atEnd())
                    return fail(QStringLiteral("Read failed: ")
                                + QFileInfo(it.srcPath).fileName());
                if (out.write(chunk) != chunk.size())
                    return fail(QStringLiteral("Write failed: ") + it.destName);
                fileDone += chunk.size();
                if (progress) progress(done + fileDone, plan.totalBytes, it.destName);
            }
        }
        if (it.isVideo && !it.sidecarDir.isEmpty()) {
            // Sidecar tree rides along — content 1:1, association by layout.
            const QString sdst = assetsDir + QStringLiteral("/.qcview/")
                + qcv::annotation_io::sanitizeMediaName(it.destName);
            madeDirs << sdst;
            QDirIterator sit(it.sidecarDir, QDir::Files | QDir::Hidden,
                             QDirIterator::Subdirectories);
            while (sit.hasNext()) {
                const QString f = sit.next();
                const QString df = sdst + QLatin1Char('/')
                    + QDir(it.sidecarDir).relativeFilePath(f);
                QDir().mkpath(QFileInfo(df).absolutePath());
                if (!QFile::copy(f, df))
                    return fail(QStringLiteral("Sidecar copy failed: ") + it.destName);
            }
        }
        done += it.bytes;
        if (progress) progress(done, plan.totalBytes, it.destName);
    }
    return true;
}

bool MediaCollector::collectDocument(BlockModel* m, bool includeVideos,
                                     int* copied, QString* error) {
    auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    if (!m || !m->mediaStore()) return fail(QStringLiteral("No document"));
    const CollectPlan cp = buildCollectPlan(m, includeVideos);
    if (cp.package) return fail(QStringLiteral("Packages are sealed"));
    if (!cp.items.empty()) {
        const QString assets = QDir::cleanPath(m->mediaStore()->docDir())
                             + QStringLiteral("/.minnotes");
        QString err;
        if (!copyAll(cp, assets, nullptr, nullptr, &err)) return fail(err);
        QHash<QString, QString> map;
        for (const CollectItem& it : cp.items)
            map.insert(it.srcPath, QStringLiteral(".minnotes/") + it.destName);
        m->rewriteMediaSrcs(map);
    }
    if (copied) *copied = static_cast<int>(cp.items.size());
    return true;
}

void MediaCollector::setProgress(double p, const QString& item) {
    QMetaObject::invokeMethod(this, [this, p, item] {
        progress_ = p;
        currentItem_ = item;
        emit progressChanged();
    }, Qt::QueuedConnection);
}

void MediaCollector::startCollect(bool includeVideos) {
    if (running_ || !model_ || !model_->mediaStore()) return;
    if (worker_.joinable()) worker_.join();   // reap the previous run
    cancel_ = false;

    // GUI-thread phase: plan against the live model (fast, no side effects).
    const CollectPlan plan = buildCollectPlan(model_, includeVideos);
    if (plan.package) {
        emit collectFinished(false, 0, QStringLiteral("Packages are sealed"));
        return;
    }
    if (plan.items.empty()) { emit collectFinished(true, 0, QString()); return; }
    const QString assets = QDir::cleanPath(model_->mediaStore()->docDir())
                         + QStringLiteral("/.minnotes");

    running_ = true;
    progress_ = 0.0;
    currentItem_.clear();
    emit runningChanged();
    emit progressChanged();

    // The rewrite must land on the model this plan was built FROM, even if
    // the user switches tabs mid-copy (setModel re-points at the active doc).
    QPointer<BlockModel> target(model_);
    worker_ = std::thread([this, plan, assets, target] {
        // Worker phase: pure file IO. Throttle progress like the packer.
        qint64 lastEmit = -1;
        QString err;
        const bool ok = copyAll(plan, assets, &cancel_,
            [this, &lastEmit](qint64 doneB, qint64 totalB, QString name) {
                const qint64 step = std::max<qint64>(totalB / 200, qint64(1) << 20);
                if (lastEmit >= 0 && doneB - lastEmit < step && doneB != totalB)
                    return;
                lastEmit = doneB;
                setProgress(totalB > 0 ? double(doneB) / double(totalB) : 1.0, name);
            }, &err);
        // Everything lands queued on the GUI thread — the ONLY place the
        // model may be touched. copyAll already rolled back on failure.
        QMetaObject::invokeMethod(this, [this, plan, target, ok, err] {
            int copied = 0;
            if (ok && target) {
                QHash<QString, QString> map;
                for (const CollectItem& it : plan.items)
                    map.insert(it.srcPath, QStringLiteral(".minnotes/") + it.destName);
                target->rewriteMediaSrcs(map);
                copied = static_cast<int>(plan.items.size());
            }
            running_ = false;
            emit runningChanged();
            emit collectFinished(ok, copied, err);
        }, Qt::QueuedConnection);
    });
}
