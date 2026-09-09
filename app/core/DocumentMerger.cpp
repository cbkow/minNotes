#include "DocumentMerger.h"
#include "AssetTransfer.h"
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

    // Asset plan (AssetTransfer: the rules shared with the clipboard paster).
    job.assets = AssetTransfer::plan(job.specs,
                                     AssetTransfer::Source::fromStore(src->mediaStore()),
                                     dest->mediaStore());
    if (!job.assets.refuse.isEmpty()) { job.refuse = job.assets.refuse; return job; }

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
    return AssetTransfer::copy(job.assets, cancelFlag, progress, error);
}

void DocumentMerger::removeCopied(const MergeJob& job) {
    AssetTransfer::removeCopied(job.assets);
}

bool DocumentMerger::applyMerge(BlockModel* dest, MergeJob& job,
                                int* destFirst, int* destLast) {
    if (destFirst) *destFirst = -1;
    if (destLast) *destLast = -1;
    if (job.noop) return true;

    // Rewrite descriptor srcs IN THE SPECS — pre-insert, so the splice is
    // the only undo-visible pass and undo/redo never see interim srcs.
    AssetTransfer::rewriteSpecs(job.specs, job.assets.srcRewrite);

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
    if (!job.noop && !job.assets.items.empty()) {
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
    for (const AssetTransfer::Item& it : job.assets.items) {
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

    if (job.assets.items.empty()) {
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
