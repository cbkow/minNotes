// PackageExporter — builds .mnpkg packages from the LOADED model (Exporter's
// sibling; same context-property shape). Packing is a DESCRIPTOR WALK, not a
// folder copy: every media carrier (media rows, sketch image layers, table
// cell media) resolves to either a local file (STOREd into the package) or —
// when the source document is itself a lazily-opened package view — the
// source archive's entry, RAW-SPLICED across without ever extracting. Plan
// resolution is strictly NO-SIDE-EFFECT (planning never extracts anything).
// The copied db's descriptors are rewritten to `.minnotes/<uniqueName>`; the
// live document is untouched.
//
// Export runs ASYNC: startExport() does the fast model-touching work on the
// GUI thread (plan + snapshot + rewrite, milliseconds), then assembles the
// zip on a worker thread with byte-weighted progress + per-item cancel; the
// finished signal lands queued on the GUI thread. packDocument() remains the
// synchronous core (headless tests).
//
// Video QCView sidecars travel with their video under
// `media/.qcview/<packedName>/` — re-association after extraction is
// automatic by layout; sidecar CONTENT is never touched (1:1 QCView purity).
#pragma once

#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

class BlockModel;
class MediaStore;

class PackageExporter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString currentItem READ currentItem NOTIFY progressChanged)
public:
    explicit PackageExporter(QObject* parent = nullptr) : QObject(parent) {}
    ~PackageExporter() override;

    void setModel(BlockModel* m) { model_ = m; }

    bool running() const { return running_; }
    double progress() const { return progress_; }
    QString currentItem() const { return currentItem_; }

    // Export-dialog pre-scan: {videos, videoBytes, mediaCount, mediaBytes} —
    // drives the detection-driven "Include videos" option. No side effects.
    Q_INVOKABLE QVariantMap scan() const;
    // Async export: plan+snapshot now, zip on a worker; progress via the
    // properties, completion via exportFinished. No-op if already running.
    Q_INVOKABLE void startExport(const QString& fileUrlOrPath, bool includeVideos);
    // Cancel between items (a single in-flight file finishes first). The
    // temp zip is removed; the destination is untouched.
    Q_INVOKABLE void cancel() { cancel_ = true; }
    Q_INVOKABLE QString lastError() const { return lastError_; }

    struct PackItem {
        QString srcPath;       // absolute local source ("" for splice items)
        QString packageEntry;  // source-archive entry to raw-splice ("" for disk items)
        QString packedName;    // unique basename → media/<packedName>
        qint64 bytes = 0;
        bool isVideo = false;
        QString sidecarDir;    // disk items: absolute .qcview/<name> dir ("" = none)
    };
    struct PackPlan {
        std::vector<PackItem> items;
        QHash<QString, QString> packedBySrc;   // no-extract abs path → packedName
        QString sourcePackage;                 // splice source ("" = none)
        int videoCount = 0;
        qint64 videoBytes = 0;
        qint64 totalBytes = 0;
        int excludedVideos = 0;
    };

    // Walk the model's media carriers into a plan WITHOUT extracting
    // anything. Missing/remote sources are skipped (descriptors stay as-is).
    static PackPlan buildPackPlan(BlockModel* m, bool includeVideos);

    // --- Shared plan primitives (public statics since the tab-merge
    // program — DocumentMerger plans with the same rules). ---
    // NO-SIDE-EFFECT resolution of one descriptor src: where the bytes are
    // (a local file, or an entry still inside a lazily-opened source
    // package), keyed by the path the file WOULD occupy on disk.
    struct Resolved {
        QString absPath;       // the (possibly not-yet-existing) local path — the map key
        QString packageEntry;  // "media/<rel>" when the bytes live in the source archive
        qint64 bytes = 0;
        bool ok = false;
    };
    static Resolved resolveNoExtract(MediaStore* store, const QJsonValue& src,
                                     const QHash<QString, qint64>& pkgEntries);
    // FileSink::unique pattern: keep the readable basename, suffix -2/-3/…
    // on collision. Case-folded (case-insensitive filesystems).
    static QString uniqueName(const QString& fileName, QSet<QString>& taken);

    // Synchronous core: plan → snapshot+rewrite → zip → atomic replace.
    static bool packDocument(BlockModel* m, const QString& destPath,
                             bool includeVideos, QString* error = nullptr);

signals:
    void runningChanged();
    void progressChanged();
    void exportFinished(bool ok, const QString& error);

private:
    // Snapshot the doc + rewrite descriptors on the copy (GUI thread; fast).
    static bool prepareDb(BlockModel* m, const PackPlan& plan, const QString& tmpDb);
    // Assemble the zip (worker-safe: file IO only). `progress` is called with
    // (doneBytes, totalBytes, currentName); null cancel/progress are fine.
    static bool assembleZip(const PackPlan& plan, const QString& tmpDb,
                            const QString& tmpZip,
                            std::atomic<bool>* cancelFlag,
                            const std::function<void(qint64, qint64, QString)>& progress);

    void setProgress(double p, const QString& item);
    void finishOnGui(bool ok, const QString& error);

    BlockModel* model_ = nullptr;
    QString lastError_;
    bool running_ = false;
    double progress_ = 0.0;
    QString currentItem_;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};

// MediaCollector — "Collect Media": copies every EXTERNAL media source (an
// absolute-path reference outside the document's folder — NAS refs, legacy
// docs) into .minnotes/ and rewrites the live descriptors to the copies as
// ONE undo entry. The packer's walker + naming rules apply (readable
// basenames, -2 collision suffixes, video .qcview sidecars ride along under
// .minnotes/.qcview/<name>/, sidecar content never touched). Package views
// are sealed — collect refuses (Save As materializes instead).
//
// Runs ASYNC like PackageExporter: plan on the GUI thread, byte-weighted
// chunked copy on a worker (cancel/failure removes everything copied so far
// — the document is never half-collected), then the descriptor rewrite lands
// queued on the GUI thread only after every copy finished.
class MediaCollector : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString currentItem READ currentItem NOTIFY progressChanged)
public:
    explicit MediaCollector(QObject* parent = nullptr) : QObject(parent) {}
    ~MediaCollector() override;

    void setModel(BlockModel* m) { model_ = m; }
    bool running() const { return running_; }
    double progress() const { return progress_; }
    QString currentItem() const { return currentItem_; }

    // Options-dialog pre-scan (no side effects): {files, fileBytes, videos,
    // videoBytes, package} — external sources only, videos counted apart
    // (they're the opt-in), package=true → collect is unavailable.
    Q_INVOKABLE QVariantMap scan() const;
    // Async collect; progress via the properties, completion via
    // collectFinished. No-op if already running.
    Q_INVOKABLE void startCollect(bool includeVideos);
    // Cancel any time (mid-file included): copies made so far are removed.
    Q_INVOKABLE void cancel() { cancel_ = true; }

    // Synchronous core (headless tests). copied ← files copied in.
    static bool collectDocument(BlockModel* m, bool includeVideos,
                                int* copied = nullptr, QString* error = nullptr);

signals:
    void runningChanged();
    void progressChanged();
    void collectFinished(bool ok, int copied, const QString& error);

private:
    struct CollectItem { QString srcPath; QString destName; QString sidecarDir;
                         qint64 bytes = 0; bool isVideo = false; };
    struct CollectPlan { std::vector<CollectItem> items; qint64 totalBytes = 0;
                         int files = 0; qint64 fileBytes = 0;
                         int videos = 0; qint64 videoBytes = 0;
                         bool package = false; };
    // The pack plan filtered to external disk items, renamed against BOTH the
    // plan and the files already in .minnotes/. Videos are counted even when
    // excluded (the scan needs them).
    static CollectPlan buildCollectPlan(BlockModel* m, bool includeVideos);
    // Worker-safe copy phase (file IO only). Rolls back everything it made on
    // cancel or failure.
    static bool copyAll(const CollectPlan& plan, const QString& assetsDir,
                        std::atomic<bool>* cancelFlag,
                        const std::function<void(qint64, qint64, QString)>& progress,
                        QString* error);

    void setProgress(double p, const QString& item);

    BlockModel* model_ = nullptr;
    bool running_ = false;
    double progress_ = 0.0;
    QString currentItem_;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};
