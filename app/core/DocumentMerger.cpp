#include "DocumentMerger.h"
#include "Document.h"
#include "MediaStore.h"
#include "PackageExporter.h"
#include "PackageFormat.h"
#include "TableGrid.h"
#include "../notes/annotation_io.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QVariant>

namespace {

// Rewrite one descriptor object's "src" through the rel→rel map. Only raw
// ".minnotes/…" strings ever appear as keys — {vol,rel} objects, absolute
// paths and http refs pass through by construction.
bool rewriteRel(QJsonObject& o, const QHash<QString, QString>& map) {
    const QJsonValue v = o.value(QStringLiteral("src"));
    if (!v.isString()) return false;
    const auto it = map.constFind(v.toString());
    if (it == map.constEnd()) return false;
    o.insert(QStringLiteral("src"), it.value());
    return true;
}

// Apply the src rewrite to a media descriptor JSON string (root src +
// sketch image layers). Returns the rewritten JSON, or "" when unchanged.
QString rewriteMediaJson(const QString& json, const QHash<QString, QString>& map) {
    QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    bool changed = false;
    if (root.value(QStringLiteral("kind")).toString() == QLatin1String("sketch")) {
        QJsonArray images = root.value(QStringLiteral("images")).toArray();
        for (int i = 0; i < images.size(); ++i) {
            QJsonObject o = images.at(i).toObject();
            if (rewriteRel(o, map)) { images.replace(i, o); changed = true; }
        }
        if (changed) root.insert(QStringLiteral("images"), images);
    } else {
        changed = rewriteRel(root, map);
    }
    if (!changed) return {};
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

} // namespace

DocumentMerger::~DocumentMerger() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

DocumentMerger::MergeJob DocumentMerger::planMerge(BlockModel* src, BlockModel* dest,
                                                   int gap) {
    MergeJob job;
    if (!src || !dest) { job.refuse = QStringLiteral("No document"); return job; }
    if (src == dest) {
        job.refuse = QStringLiteral("A document can't merge into itself");
        return job;
    }
    const int n = src->rowCountQml();
    job.srcWidth = src->pageWidth();
    job.destWidth = dest->pageWidth();

    // Snapshot: specs + parallel ink, straight off the source model.
    job.specs.reserve(size_t(n));
    job.ink.reserve(size_t(n));
    for (int r = 0; r < n; ++r) {
        job.specs.push_back(src->specForRow(r));
        job.ink.push_back(src->inkForRow(r));
    }

    // Pristine-empty source (the fresh scratch tab): success, nothing to do.
    if (n == 1 && job.ink[0].isEmpty()) {
        const BlockModel::BlockSpec& sp = job.specs[0];
        if (sp.type == BlockModel::Paragraph && sp.text.isEmpty()
            && sp.mediaJson.isEmpty() && sp.tableJson.isEmpty()) {
            job.noop = true;
            return job;
        }
    }

    // Comment harvest: threads referenced by snapshot spans get NEW ids with
    // their history preserved; ghost anchors (spans whose thread row is gone)
    // are stripped rather than exported dangling. The href rewrite happens
    // right here — the specs are already our private copy.
    QHash<QString, QVariantMap> srcThreads;
    for (const QVariant& tv : src->commentThreads()) {
        const QVariantMap m = tv.toMap();
        srcThreads.insert(m.value(QStringLiteral("id")).toString(), m);
    }
    QHash<QString, QString> threadRemap;
    for (BlockModel::BlockSpec& sp : job.specs) {
        for (auto it = sp.spans.begin(); it != sp.spans.end();) {
            if (it->kind != BlockModel::SpanComment) { ++it; continue; }
            const auto th = srcThreads.constFind(it->href);
            if (th == srcThreads.constEnd()) { it = sp.spans.erase(it); continue; }
            QString newId = threadRemap.value(it->href);
            if (newId.isEmpty()) {
                newId = makeUlid();
                threadRemap.insert(it->href, newId);
                BlockModel::ThreadImport ti;
                ti.id = newId;
                ti.created = th->value(QStringLiteral("created")).toLongLong();
                ti.resolved = th->value(QStringLiteral("resolved")).toBool();
                for (const QVariant& mv : src->commentMessages(it->href)) {
                    const QVariantMap mm = mv.toMap();
                    ti.messages.push_back({makeUlid(),
                                           mm.value(QStringLiteral("body")).toString(),
                                           mm.value(QStringLiteral("created")).toLongLong(),
                                           mm.value(QStringLiteral("modified")).toLongLong()});
                }
                job.threads.push_back(std::move(ti));
            }
            it->href = newId;
            ++it;
        }
    }

    // Asset plan: only rel `.minnotes/` sources (disk or still inside a
    // source .mnpkg) are copied — they'd dangle in the destination. Names
    // dodge BOTH the plan and what the dest sidecar already holds; an
    // existing same-name same-size file (the content-addressed pasted-image
    // case) is reused without copying.
    MediaStore* srcStore = src->mediaStore();
    MediaStore* destStore = dest->mediaStore();
    job.sourcePackage = srcStore ? srcStore->packageSource() : QString();
    const QHash<QString, qint64> pkgEntries =
        job.sourcePackage.isEmpty() ? QHash<QString, qint64>()
                                    : mnpkg::entrySizes(job.sourcePackage);
    job.destAssetsDir = destStore
        ? QDir::cleanPath(destStore->docDir()) + QStringLiteral("/.minnotes")
        : QString();

    QSet<QString> taken;
    const QDir destAssets(job.destAssetsDir);
    for (const QString& name : destAssets.entryList(QDir::Files | QDir::Dirs
                                                    | QDir::NoDotAndDotDot | QDir::Hidden))
        taken.insert(name.toLower());

    auto planSrc = [&](const QJsonValue& v, bool isVideo) {
        if (!v.isString()) return;
        const QString rel = v.toString();
        if (!rel.startsWith(QLatin1String(".minnotes/"))) return;   // linked: pass through
        if (job.srcRewrite.contains(rel)) return;                   // deduped
        if (!srcStore) return;
        const PackageExporter::Resolved res =
            PackageExporter::resolveNoExtract(srcStore, v, pkgEntries);
        if (!res.ok) return;   // broken ref stays as-is — honest either side
        const QString base = QFileInfo(res.absPath).fileName();
        const QFileInfo existing(job.destAssetsDir + QLatin1Char('/') + base);
        if (existing.exists() && existing.isFile() && existing.size() == res.bytes) {
            job.srcRewrite.insert(rel, QStringLiteral(".minnotes/") + base);
            if (isVideo) {
                // Reused byte-identical video: its .qcview tree must still
                // ride when the destination has none (a sidecar-only item;
                // copyAssets never clobbers an existing dest sidecar).
                MergeItem sc;
                sc.srcRel = rel;
                sc.destName = base;
                sc.isVideo = true;
                if (res.packageEntry.isEmpty()) {
                    const QString dir = QFileInfo(
                        qcv::annotation_io::getNotesJsonPath(res.absPath)).absolutePath();
                    if (QFileInfo::exists(dir)) sc.sidecarDir = dir;
                } else {
                    sc.pkgSidecarPrefix = QStringLiteral("media/.qcview/")
                        + qcv::annotation_io::sanitizeMediaName(base) + QLatin1Char('/');
                }
                if (!sc.sidecarDir.isEmpty() || !sc.pkgSidecarPrefix.isEmpty())
                    job.items.push_back(std::move(sc));
            }
            return;                                                 // identical: reuse
        }
        MergeItem it;
        it.srcRel = rel;
        it.srcPath = res.packageEntry.isEmpty() ? res.absPath : QString();
        it.packageEntry = res.packageEntry;
        it.destName = PackageExporter::uniqueName(base, taken);
        it.bytes = res.bytes;
        it.isVideo = isVideo;
        if (isVideo) {
            if (it.packageEntry.isEmpty()) {
                const QString dir = QFileInfo(
                    qcv::annotation_io::getNotesJsonPath(res.absPath)).absolutePath();
                if (QFileInfo::exists(dir)) it.sidecarDir = dir;
            } else {
                it.pkgSidecarPrefix = QStringLiteral("media/.qcview/")
                    + qcv::annotation_io::sanitizeMediaName(base) + QLatin1Char('/');
            }
        }
        job.srcRewrite.insert(rel, QStringLiteral(".minnotes/") + it.destName);
        job.totalBytes += it.bytes;
        job.items.push_back(std::move(it));
    };

    for (const BlockModel::BlockSpec& sp : job.specs) {
        if (!sp.mediaJson.isEmpty()) {
            const QJsonObject root =
                QJsonDocument::fromJson(sp.mediaJson.toUtf8()).object();
            if (root.value(QStringLiteral("kind")).toString() == QLatin1String("sketch")) {
                for (const QJsonValue& v : root.value(QStringLiteral("images")).toArray())
                    planSrc(v.toObject().value(QStringLiteral("src")), /*isVideo*/false);
            } else {
                planSrc(root.value(QStringLiteral("src")),
                        root.value(QStringLiteral("kind")).toString()
                            == QLatin1String("video"));
            }
        } else if (!sp.tableJson.isEmpty()) {
            const TableGrid g = TableGrid::fromJson(sp.tableJson);
            for (int tr = 0; tr < g.rows(); ++tr)
                for (int tc = 0; tc < g.cols(); ++tc) {
                    const QString desc = g.cellMedia(tr, tc);
                    if (desc.isEmpty()) continue;
                    planSrc(QJsonDocument::fromJson(desc.toUtf8()).object()
                                .value(QStringLiteral("src")), /*isVideo*/false);
                }
        }
    }
    if (!job.items.empty() && job.destAssetsDir.isEmpty()) {
        job.refuse = QStringLiteral("Destination has no media folder");
        return job;
    }

    // Gap → stable anchor (the block id ABOVE the gap) so an edited
    // destination re-derives the drop point at apply time.
    const int destN = dest->rowCountQml();
    job.gap = std::clamp(gap, 0, destN);
    job.anchorId = (job.gap > 0) ? dest->idForRow(job.gap - 1) : QString();
    return job;
}

bool DocumentMerger::copyAssets(const MergeJob& job, std::atomic<bool>* cancelFlag,
                                const std::function<void(qint64, qint64, QString)>& progress,
                                QString* error) {
    QStringList madeFiles, madeDirs;
    auto fail = [&](const QString& why) {
        for (const QString& f : madeFiles) QFile::remove(f);
        for (const QString& d : madeDirs) QDir(d).removeRecursively();
        if (error) *error = why;
        return false;
    };
    QDir().mkpath(job.destAssetsDir);
    qint64 done = 0;
    for (const MergeItem& it : job.items) {
        if (cancelFlag && cancelFlag->load()) return fail(QStringLiteral("Cancelled"));
        if (progress) progress(done, job.totalBytes, it.destName);
        const QString dest = job.destAssetsDir + QLatin1Char('/') + it.destName;
        if (!it.packageEntry.isEmpty()) {
            // Bytes still sealed in the source .mnpkg — stream the entry out.
            if (!mnpkg::extractEntry(job.sourcePackage, it.packageEntry, dest))
                return fail(QStringLiteral("Package read failed: ") + it.destName);
            madeFiles << dest;
        } else if (!it.srcPath.isEmpty()) {
            // Chunked disk copy — multi-GB NAS videos honour mid-file cancel.
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
                if (progress) progress(done + fileDone, job.totalBytes, it.destName);
            }
        }
        // else: a sidecar-only item (a reused byte-identical video).
        if (it.isVideo && (!it.sidecarDir.isEmpty() || !it.pkgSidecarPrefix.isEmpty())) {
            // Sidecar tree rides along — content 1:1, association by layout.
            // An existing dest tree is NEVER touched (its annotations win).
            const QString sdst = job.destAssetsDir + QStringLiteral("/.qcview/")
                + qcv::annotation_io::sanitizeMediaName(it.destName);
            if (!QFileInfo::exists(sdst)) {
                madeDirs << sdst;
                if (!it.pkgSidecarPrefix.isEmpty()) {
                    mnpkg::extractMatching(job.sourcePackage, it.pkgSidecarPrefix,
                                           it.pkgSidecarPrefix, sdst);   // 0 is fine
                } else {
                    QDirIterator sit(it.sidecarDir, QDir::Files | QDir::Hidden,
                                     QDirIterator::Subdirectories);
                    while (sit.hasNext()) {
                        const QString f = sit.next();
                        const QString df = sdst + QLatin1Char('/')
                            + QDir(it.sidecarDir).relativeFilePath(f);
                        QDir().mkpath(QFileInfo(df).absolutePath());
                        if (!QFile::copy(f, df))
                            return fail(QStringLiteral("Sidecar copy failed: ")
                                        + it.destName);
                    }
                }
            }
        }
        done += it.bytes;
        if (progress) progress(done, job.totalBytes, it.destName);
    }
    return true;
}

void DocumentMerger::removeCopied(const MergeJob& job) {
    for (const MergeItem& it : job.items) {
        // Sidecar-only items point at a REUSED pre-existing file (and
        // possibly the dest's own sidecar) — never remove those.
        if (it.srcPath.isEmpty() && it.packageEntry.isEmpty()) continue;
        QFile::remove(job.destAssetsDir + QLatin1Char('/') + it.destName);
        if (it.isVideo)
            QDir(job.destAssetsDir + QStringLiteral("/.qcview/")
                 + qcv::annotation_io::sanitizeMediaName(it.destName)).removeRecursively();
    }
}

bool DocumentMerger::applyMerge(BlockModel* dest, MergeJob& job,
                                int* destFirst, int* destLast) {
    if (destFirst) *destFirst = -1;
    if (destLast) *destLast = -1;
    if (job.noop) return true;

    // Rewrite descriptor srcs IN THE SPECS — pre-insert, so the splice is
    // the only undo-visible pass and undo/redo never see interim srcs.
    if (!job.srcRewrite.isEmpty()) {
        for (BlockModel::BlockSpec& sp : job.specs) {
            if (!sp.mediaJson.isEmpty()) {
                const QString r = rewriteMediaJson(sp.mediaJson, job.srcRewrite);
                if (!r.isEmpty()) sp.mediaJson = r;
            } else if (!sp.tableJson.isEmpty()) {
                TableGrid g = TableGrid::fromJson(sp.tableJson);
                bool changed = false;
                for (int tr = 0; tr < g.rows(); ++tr)
                    for (int tc = 0; tc < g.cols(); ++tc) {
                        const QString desc = g.cellMedia(tr, tc);
                        if (desc.isEmpty()) continue;
                        const QString r = rewriteMediaJson(desc, job.srcRewrite);
                        if (!r.isEmpty()) { g.setCellMedia(tr, tc, r); changed = true; }
                    }
                if (changed) sp.tableJson = g.toJson();
            }
        }
    }

    // Mint the migrated threads FIRST so no anchor ever points at a missing
    // thread, even transiently. Thread rows are outside undo by design.
    for (const BlockModel::ThreadImport& ti : job.threads)
        dest->importCommentThread(ti);

    // Re-derive the gap from the anchor id (the destination may have been
    // edited since plan time); a deleted anchor falls back to the planned
    // gap clamped into today's document.
    const int destN = dest->rowCountQml();
    int gap;
    if (job.anchorId.isEmpty()) {
        gap = 0;
    } else {
        const int row = dest->rowForId(job.anchorId);
        gap = (row >= 0) ? row + 1 : std::min(job.gap, destN);
    }

    // The fold-eligibility mirror of spliceSpecsAt: the outer band must
    // COVER the anchor when the first spec will consume it, or undo could
    // not restore the blank row.
    const int anchor = gap - 1;
    bool willFold = false;
    if (anchor >= 0) {
        const int t = dest->typeForRow(anchor);
        willFold = (t == BlockModel::Paragraph || t == BlockModel::Heading
                    || t == BlockModel::Quote || t == BlockModel::ListItem)
            && dest->contentForRow(anchor).isEmpty();
    }

    dest->beginGroup(willFold ? anchor : gap, gap - 1);
    dest->spliceSpecsAt(gap, job.specs, /*allowReuseAnchorAbove=*/true);
    const int firstRow = willFold ? anchor : gap;
    const int lastRow = firstRow + static_cast<int>(job.specs.size()) - 1;
    const bool migrate = !qFuzzyCompare(job.srcWidth, job.destWidth);
    for (size_t k = 0; k < job.ink.size(); ++k) {
        if (job.ink[k].isEmpty()) continue;
        QString inkJson = job.ink[k];
        if (migrate) {
            const QString m = BlockModel::migrateInkForWidth(inkJson, job.srcWidth,
                                                             job.destWidth);
            if (!m.isEmpty()) inkJson = m;
        }
        dest->setBlockInk(firstRow + static_cast<int>(k), inkJson);
    }
    dest->endGroup();
    dest->localizeRemoteMedia(firstRow, lastRow);

    if (destFirst) *destFirst = firstRow;
    if (destLast) *destLast = lastRow;
    return true;
}

bool DocumentMerger::mergeDocuments(BlockModel* src, BlockModel* dest, int gap,
                                    int* destFirst, int* destLast, QString* error) {
    MergeJob job = planMerge(src, dest, gap);
    if (!job.refuse.isEmpty()) { if (error) *error = job.refuse; return false; }
    if (!job.noop && !job.items.empty()) {
        QString err;
        if (!copyAssets(job, nullptr, nullptr, &err)) {
            if (error) *error = err;
            return false;
        }
    }
    return applyMerge(dest, job, destFirst, destLast);
}

QVariantMap DocumentMerger::scan(BlockModel* src, BlockModel* dest) const {
    const MergeJob job = planMerge(src, dest, dest ? dest->rowCountQml() : 0);
    QVariantMap out;
    out.insert(QStringLiteral("ok"), job.refuse.isEmpty());
    out.insert(QStringLiteral("reason"), job.refuse);
    out.insert(QStringLiteral("blocks"),
               job.noop ? 0 : static_cast<int>(job.specs.size()));
    int files = 0, videos = 0;
    qint64 fileBytes = 0, videoBytes = 0;
    for (const MergeItem& it : job.items) {
        if (it.isVideo) { videos++; videoBytes += it.bytes; }
        else            { files++;  fileBytes += it.bytes; }
    }
    out.insert(QStringLiteral("files"), files);
    out.insert(QStringLiteral("fileBytes"), fileBytes);
    out.insert(QStringLiteral("videos"), videos);
    out.insert(QStringLiteral("videoBytes"), videoBytes);
    return out;
}

void DocumentMerger::setProgress(double p, const QString& item) {
    QMetaObject::invokeMethod(this, [this, p, item] {
        progress_ = p;
        currentItem_ = item;
        emit progressChanged();
    }, Qt::QueuedConnection);
}

void DocumentMerger::startMerge(BlockModel* src, BlockModel* dest, int gap) {
    if (running_) return;
    if (worker_.joinable()) worker_.join();   // reap the previous run
    cancel_ = false;

    // GUI-thread phase: plan against both live models (fast, no side effects).
    MergeJob job = planMerge(src, dest, gap);
    if (!job.refuse.isEmpty()) { emit mergeFinished(false, -1, -1, job.refuse); return; }
    if (job.noop) { emit mergeFinished(true, -1, -1, QString()); return; }

    if (job.items.empty()) {
        // Nothing to copy: apply directly, no worker round-trip.
        int first = -1, last = -1;
        applyMerge(dest, job, &first, &last);
        emit mergeFinished(true, first, last, QString());
        return;
    }

    running_ = true;
    progress_ = 0.0;
    currentItem_.clear();
    emit runningChanged();
    emit progressChanged();

    // The apply must land on the model this plan was built FOR, even if the
    // user switches tabs or closes the destination mid-copy.
    QPointer<BlockModel> target(dest);
    worker_ = std::thread([this, job = std::move(job), target]() mutable {
        // Worker phase: pure file IO. Throttled progress like the packer.
        qint64 lastEmit = -1;
        QString err;
        const bool ok = copyAssets(job, &cancel_,
            [this, &lastEmit](qint64 doneB, qint64 totalB, QString name) {
                const qint64 step = std::max<qint64>(totalB / 200, qint64(1) << 20);
                if (lastEmit >= 0 && doneB - lastEmit < step && doneB != totalB)
                    return;
                lastEmit = doneB;
                setProgress(totalB > 0 ? double(doneB) / double(totalB) : 1.0, name);
            }, &err);
        // Everything lands queued on the GUI thread — the ONLY place the
        // models may be touched. copyAssets already rolled back on failure.
        QMetaObject::invokeMethod(this, [this, job = std::move(job), target, ok, err]() mutable {
            int first = -1, last = -1;
            bool applied = false;
            if (ok) {
                if (target) {
                    applied = applyMerge(target, job, &first, &last);
                } else {
                    removeCopied(job);   // destination closed — no orphan files
                }
            }
            running_ = false;
            emit runningChanged();
            emit mergeFinished(ok && applied, first, last,
                               (ok && !applied) ? QStringLiteral("Destination closed") : err);
        }, Qt::QueuedConnection);
    });
}
