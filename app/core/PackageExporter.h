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
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

class BlockModel;

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
