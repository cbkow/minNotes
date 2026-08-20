// DocumentMerger — the tab-merge engine (0.4.0 program): COPY every block of
// one open document into another at a chosen gap, with full fidelity. The
// source is never modified; the destination gets the blocks as ONE undo
// entry. Fidelity holes the plain spec path would leave are closed here:
//   - margin ink travels per block (width-migrated when the page measures
//     differ, via BlockModel::migrateInkForWidth — never forked math);
//   - comment threads are re-minted in the destination with their history
//     (created/resolved/message timestamps) intact, span hrefs rewritten;
//   - assets keep their DISPOSITION: collected (rel `.minnotes/`) sources —
//     on disk or still inside a source .mnpkg — are copied into the
//     destination's sidecar (they would dangle otherwise); linked sources
//     (absolute, {vol,rel} NAS refs, http) pass through untouched.
//
// Runs ASYNC in the MediaCollector shape: plan on the GUI thread, chunked
// byte-weighted copy on a worker (cancel/failure rolls back every file
// made), then the model apply lands queued on the GUI thread. Thread rows
// and copied files sit OUTSIDE the undo entry by existing design.
#pragma once

#include "BlockModel.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

class DocumentMerger : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString currentItem READ currentItem NOTIFY progressChanged)
public:
    explicit DocumentMerger(QObject* parent = nullptr) : QObject(parent) {}
    ~DocumentMerger() override;

    bool running() const { return running_; }
    double progress() const { return progress_; }
    QString currentItem() const { return currentItem_; }

    // Pre-flight (no side effects): {ok, reason, blocks, files, fileBytes,
    // videos, videoBytes} — drives the drop affordance + progress-popup gate.
    Q_INVOKABLE QVariantMap scan(BlockModel* src, BlockModel* dest) const;
    // Async merge of the whole source document into `dest` at `gap` (gap g =
    // between rows g-1 and g; count = end). No-op if already running.
    Q_INVOKABLE void startMerge(BlockModel* src, BlockModel* dest, int gap);
    // Cancel between chunks; files copied so far are removed.
    Q_INVOKABLE void cancel() { cancel_ = true; }

    // Synchronous core (headless tests). destFirst/destLast ← the inserted
    // row range in the destination (-1/-1 for a pristine-empty-source no-op).
    static bool mergeDocuments(BlockModel* src, BlockModel* dest, int gap,
                               int* destFirst = nullptr, int* destLast = nullptr,
                               QString* error = nullptr);

signals:
    void runningChanged();
    void progressChanged();
    void mergeFinished(bool ok, int destFirst, int destLast, const QString& error);

private:
    struct MergeItem {
        QString srcRel;            // the raw ".minnotes/…" source src string
        QString srcPath;           // absolute disk source ("" for package items)
        QString packageEntry;      // "media/<rel>" inside the source .mnpkg
        QString destName;          // unique basename → dest .minnotes/<destName>
        QString sidecarDir;        // disk videos: absolute .qcview dir ("" = none)
        QString pkgSidecarPrefix;  // package videos: archive sidecar prefix
        qint64 bytes = 0;
        bool isVideo = false;
    };
    // Everything the worker + apply need, snapshotted at plan time; the
    // worker never touches a model.
    struct MergeJob {
        std::vector<BlockModel::BlockSpec> specs;
        std::vector<QString> ink;                        // parallel to specs
        std::vector<BlockModel::ThreadImport> threads;   // NEW ids already minted
        QHash<QString, QString> srcRewrite;              // rel src → rel src
        std::vector<MergeItem> items;
        qint64 totalBytes = 0;
        QString sourcePackage;     // splice source ("" = none)
        QString destAssetsDir;
        QString anchorId;          // dest block id above the gap ("" = top)
        int gap = 0;
        qreal srcWidth = 760, destWidth = 760;
        bool noop = false;         // pristine-empty source → success, no rows
        QString refuse;            // non-empty = refused at plan time
    };

    static MergeJob planMerge(BlockModel* src, BlockModel* dest, int gap);
    // Worker-safe copy phase (file IO only); rolls back everything it made
    // on cancel or failure.
    static bool copyAssets(const MergeJob& job, std::atomic<bool>* cancelFlag,
                           const std::function<void(qint64, qint64, QString)>& progress,
                           QString* error);
    // GUI-thread apply: rewrite srcs in the specs, mint threads, splice as
    // ONE undo entry, lay the ink per row (width-migrated).
    static bool applyMerge(BlockModel* dest, MergeJob& job,
                           int* destFirst, int* destLast);
    // Cleanup for an apply that never ran (dest closed mid-copy).
    static void removeCopied(const MergeJob& job);

    void setProgress(double p, const QString& item);

    bool running_ = false;
    double progress_ = 0.0;
    QString currentItem_;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};
