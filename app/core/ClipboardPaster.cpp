#include "ClipboardPaster.h"
#include "Document.h"
#include "MediaStore.h"

#include <QHash>
#include <QPointer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

ClipboardPaster::~ClipboardPaster() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool ClipboardPaster::looksTabular(const QString& text) {
    if (!text.contains(QLatin1Char('\t'))) return false;
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList lines = t.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    if (lines.size() < 2) return false;
    int cols = -1;
    for (const QString& line : lines) {
        if (line.isEmpty()) return false;                     // a blank interior line → not a grid
        const int tabs = static_cast<int>(line.count(QLatin1Char('\t')));
        if (tabs < 1) return false;
        if (cols < 0) cols = tabs; else if (tabs != cols) return false;
    }
    return true;
}

QString ClipboardPaster::route(const PasteInput& in, const PasteTarget& at) {
    const bool hasText = !in.text.isEmpty();
    const bool tableText = in.text.contains(QLatin1Char('\t')) || in.text.contains(QLatin1Char('\n'));
    // A sketch tab takes images (files or raster) onto its canvas; nothing else has a target.
    if (at.sketchTab) return in.urls > 0 ? QStringLiteral("sketchUrls") : in.hasImage ? QStringLiteral("sketchRaster") : QStringLiteral("nothing");
    // Our own blocks: a faithful run — not into a code block (verbatim text wins).
    if (in.hasBlocks && !at.codeBlock) return QStringLiteral("blocks");
    // A code block: text verbatim (no HTML, no markdown prefixes, no table detection).
    if (at.codeBlock && hasText) return QStringLiteral("codeVerbatim");
    // Rich HTML → structured blocks, unless it's a browser's Copy Image (the raster is right here).
    if (in.hasHtml && !(in.bareRemoteImage && in.hasImage)) return QStringLiteral("html");
    if (in.urls > 0) return QStringLiteral("urls");
    if (in.hasImage) return QStringLiteral("raster");
    if (!hasText) return QStringLiteral("nothing");
    if (at.inTable && tableText) return QStringLiteral("tableTsv");                 // A5: fill cells from the anchor
    if (!in.noTable && looksTabular(in.text)) return QStringLiteral("tableFromTsv");
    return QStringLiteral("text");
}

QString ClipboardPaster::routePaste(const QVariantMap& input, const QVariantMap& target) const {
    PasteInput in;
    in.hasBlocks = input.value(QStringLiteral("hasBlocks")).toBool();
    in.hasHtml = input.value(QStringLiteral("hasHtml")).toBool();
    in.hasImage = input.value(QStringLiteral("hasImage")).toBool();
    in.bareRemoteImage = input.value(QStringLiteral("bareRemoteImage")).toBool();
    in.noTable = input.value(QStringLiteral("noTable")).toBool();
    in.urls = input.value(QStringLiteral("urls")).toInt();
    in.text = input.value(QStringLiteral("text")).toString();
    PasteTarget at;
    at.sketchTab = target.value(QStringLiteral("sketchTab")).toBool();
    at.codeBlock = target.value(QStringLiteral("codeBlock")).toBool();
    at.inTable = target.value(QStringLiteral("inTable")).toBool();
    return route(in, at);
}

ClipboardPaster::Job ClipboardPaster::planPaste(BlockModel* dest, const QString& json,
                                                int row, int col,
                                                int selLo, int selLoCol, int selHi, int selHiCol, bool intoCells) {
    Job job;
    job.intoCells = intoCells;
    if (!dest) { job.refuse = QStringLiteral("No document"); return job; }
    QString err;
    if (!BlockClipboard::decode(json.toUtf8(), &job.payload, &err)) { job.refuse = err; return job; }
    if (job.payload.specs.empty()) { job.refuse = QStringLiteral("Nothing to paste"); return job; }
    job.row = row; job.col = col;
    job.selLo = selLo; job.selLoCol = selLoCol; job.selHi = selHi; job.selHiCol = selHiCol;
    const int destN = dest->rowCountQml();
    job.anchorId = (destN > 0) ? dest->idForRow(std::clamp(row, 0, destN - 1)) : QString();

    // Comment threads. Same document + the anchor is gone (a cut pasted
    // back) → re-anchor the original thread. Otherwise mint a new thread
    // carrying the payload's history (or, same-document copy of a still-
    // anchored thread, the live thread's history — a duplicate conversation,
    // by ruling). No history anywhere → the anchor is dropped.
    const bool sameDoc = !job.payload.docPath.isEmpty()
        && job.payload.docPath == dest->documentPath();
    QHash<QString, const BlockModel::ThreadImport*> carried;
    for (const BlockModel::ThreadImport& t : job.payload.threads) carried.insert(t.id, &t);
    QHash<QString, QVariantMap> live;
    for (const QVariant& tv : dest->commentThreads()) {
        const QVariantMap m = tv.toMap();
        live.insert(m.value(QStringLiteral("id")).toString(), m);
    }
    QHash<QString, QString> remap;
    for (BlockModel::BlockSpec& sp : job.payload.specs) {
        for (auto it = sp.spans.begin(); it != sp.spans.end();) {
            if (it->kind != BlockModel::SpanComment) { ++it; continue; }
            const QString srcId = it->href;
            const auto rm = remap.constFind(srcId);
            if (rm != remap.constEnd()) { it->href = rm.value(); ++it; continue; }
            const bool inLive = live.contains(srcId);
            if (sameDoc && inLive && dest->threadAnchorRow(srcId) < 0) {
                remap.insert(srcId, srcId);                 // re-anchor the cut
                ++it; continue;
            }
            const auto cp = carried.constFind(srcId);
            if (cp == carried.constEnd() && !inLive) { it = sp.spans.erase(it); continue; }
            BlockModel::ThreadImport ti;
            ti.id = makeUlid();
            if (cp != carried.constEnd()) {
                ti.created = (*cp)->created;
                ti.resolved = (*cp)->resolved;
                for (const BlockModel::ThreadMessage& m : (*cp)->messages)
                    ti.messages.push_back({makeUlid(), m.body, m.created, m.modified});
            } else {
                const QVariantMap& lm = live.value(srcId);
                ti.created = lm.value(QStringLiteral("created")).toLongLong();
                ti.resolved = lm.value(QStringLiteral("resolved")).toBool();
                for (const QVariant& mv : dest->commentMessages(srcId)) {
                    const QVariantMap mm = mv.toMap();
                    ti.messages.push_back({makeUlid(), mm.value(QStringLiteral("body")).toString(),
                                           mm.value(QStringLiteral("created")).toLongLong(),
                                           mm.value(QStringLiteral("modified")).toLongLong()});
                }
            }
            remap.insert(srcId, ti.id);
            it->href = ti.id;
            job.threads.push_back(std::move(ti));
            ++it;
        }
    }

    // Assets: same sidecar → the srcs are already right (the very same file,
    // no copy, no re-encode). Otherwise plan the copy off the payload's
    // snapshot, re-verified against the filesystem now.
    const bool sameDir = !job.payload.docDir.isEmpty()
        && job.payload.docDir == dest->mediaAnchorDir();
    if (!sameDir) {
        job.assets = AssetTransfer::plan(job.payload.specs,
                                         BlockClipboard::assetSource(job.payload),
                                         dest->mediaStore());
        if (!job.assets.refuse.isEmpty()) job.refuse = job.assets.refuse;
    }
    return job;
}

bool ClipboardPaster::applyPaste(BlockModel* dest, Job& job, int* caretRow, int* caretCol) {
    if (caretRow) *caretRow = -1;
    if (caretCol) *caretCol = -1;
    const int destN = dest->rowCountQml();
    if (destN == 0) return false;

    // Re-derive the target from its block id (the destination may have been
    // edited during an async copy); a moved anchor drops the selection
    // delete — the paste still lands where the caret was.
    int row = std::clamp(job.row, 0, destN - 1), col = job.col;
    bool withSel = job.selLo >= 0 && job.selHi >= 0
        && job.selLo < destN && job.selHi < destN;
    if (!job.anchorId.isEmpty()) {
        const int r = dest->rowForId(job.anchorId);
        if (r >= 0 && r != row) { row = r; withSel = false; }
        else if (r < 0) withSel = false;
    }

    AssetTransfer::rewriteSpecs(job.payload.specs, job.assets.srcRewrite);

    // Thread rows first (never a dangling anchor, even transiently); they sit
    // outside the undo entry by existing design.
    for (const BlockModel::ThreadImport& ti : job.threads) dest->importCommentThread(ti);

    // ONE undo entry: selection delete + splice + ink. Band in pre-mutation
    // coordinates covers the selection and the target row; the paste's
    // inserted rows land inside [lo, hi + delta].
    // Whole split rows (SR-3 S8): a split row or table pasted into a lane lands
    // below its split row, which the band must reach.
    const auto [lo, hi] = dest->wholeSplitRows(withSel ? std::min(job.selLo, row) : row,
                                               withSel ? std::max(job.selHi, row) : row);
    dest->beginGroup(lo, hi);
    if (withSel) {
        const QVariantList land = dest->deleteSelectionRange(job.selLo, job.selLoCol,
                                                             job.selHi, job.selHiCol);
        if (land.size() == 2) { row = land.at(0).toInt(); col = land.at(1).toInt(); }
    }
    // The paste rules (SR-4 S8d, §4.11): a grid — a cell fragment (the payload's `grid`) or copied
    // table rows — into a table cell fills by position or appends by label; outside a table a
    // fragment becomes a table of its own.
    std::pair<int, int> caret{ -1, -1 };
    bool routed = false;
    {
        const QJsonArray cols = job.payload.grid.value(QStringLiteral("cols")).toArray();
        const int header = job.payload.grid.value(QStringLiteral("header")).toInt(0);
        const bool fragment = !job.payload.grid.isEmpty();
        BlockModel::GridPaste grid;
        const int head = dest->tableHeadOf(row);
        if (head >= 0 && dest->laneForRow(row) >= 0
            && BlockModel::parseGridSpecs(job.payload.specs, cols, fragment ? header : 0, &grid)) {
            if (fragment) grid.header = header;
            const int land = dest->pasteGrid(head, dest->tableRowOf(row), dest->tableColumnOf(row), grid, job.intoCells);
            if (land >= 0) { caret = { land, static_cast<int>(dest->contentForRow(land).size()) }; routed = true; }
        } else if (fragment) {
            BlockModel::promoteGridSpecs(job.payload.specs, cols, header);
        }
    }
    if (!routed)
        caret = dest->pasteSpecsAt(row, col, job.payload.specs, job.payload.ink, job.payload.pageWidth);
    dest->endGroup();
    dest->localizeRemoteMedia(row, std::max(caret.first, row + static_cast<int>(job.payload.specs.size())));

    if (caretRow) *caretRow = caret.first;
    if (caretCol) *caretCol = caret.second;
    return true;
}

bool ClipboardPaster::pasteBlocks(BlockModel* dest, const QString& json, int row, int col,
                                  int selLo, int selLoCol, int selHi, int selHiCol,
                                  int* caretRow, int* caretCol, QString* error, bool intoCells) {
    Job job = planPaste(dest, json, row, col, selLo, selLoCol, selHi, selHiCol, intoCells);
    if (!job.refuse.isEmpty()) { if (error) *error = job.refuse; return false; }
    if (!job.assets.items.empty()) {
        QString err;
        if (!AssetTransfer::copy(job.assets, nullptr, nullptr, &err)) {
            if (error) *error = err;
            return false;
        }
    }
    return applyPaste(dest, job, caretRow, caretCol);
}

void ClipboardPaster::setProgress(double p, const QString& item) {
    QMetaObject::invokeMethod(this, [this, p, item] {
        progress_ = p;
        currentItem_ = item;
        emit progressChanged();
    }, Qt::QueuedConnection);
}

void ClipboardPaster::startPaste(BlockModel* dest, const QString& json, int row, int col,
                                 int selLo, int selLoCol, int selHi, int selHiCol, bool intoCells) {
    if (running_) return;
    if (worker_.joinable()) worker_.join();   // reap the previous run
    cancel_ = false;

    Job job = planPaste(dest, json, row, col, selLo, selLoCol, selHi, selHiCol, intoCells);
    if (!job.refuse.isEmpty()) { emit pasteFinished(false, -1, -1, job.refuse); return; }

    if (job.assets.items.empty()) {
        // Nothing to copy: apply directly, no worker round-trip.
        int cr = -1, cc = -1;
        const bool ok = applyPaste(dest, job, &cr, &cc);
        emit pasteFinished(ok, cr, cc, ok ? QString() : QStringLiteral("Nothing to paste into"));
        return;
    }

    running_ = true;
    progress_ = 0.0;
    currentItem_.clear();
    emit runningChanged();
    emit progressChanged();

    QPointer<BlockModel> target(dest);
    worker_ = std::thread([this, job = std::move(job), target]() mutable {
        qint64 lastEmit = -1;
        QString err;
        const bool ok = AssetTransfer::copy(job.assets, &cancel_,
            [this, &lastEmit](qint64 doneB, qint64 totalB, QString name) {
                const qint64 step = std::max<qint64>(totalB / 200, qint64(1) << 20);
                if (lastEmit >= 0 && doneB - lastEmit < step && doneB != totalB) return;
                lastEmit = doneB;
                setProgress(totalB > 0 ? double(doneB) / double(totalB) : 1.0, name);
            }, &err);
        QMetaObject::invokeMethod(this, [this, job = std::move(job), target, ok, err]() mutable {
            int cr = -1, cc = -1;
            bool applied = false;
            if (ok) {
                if (target) applied = applyPaste(target, job, &cr, &cc);
                else        AssetTransfer::removeCopied(job.assets);   // destination closed
            }
            running_ = false;
            emit runningChanged();
            emit pasteFinished(ok && applied, cr, cc,
                               (ok && !applied) ? QStringLiteral("Destination closed") : err);
        }, Qt::QueuedConnection);
    });
}
