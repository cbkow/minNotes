// AssetTransfer — media DISPOSITION shared by tab merge (DocumentMerger) and
// the rich clipboard paste (ClipboardPaster). Hoisted verbatim from the
// merger in the 0.5.0 selection/clipboard program: PLAN on the GUI thread
// (no side effects), chunked byte-weighted COPY on a worker (cancel or
// failure rolls back every file made), then REWRITE the specs' srcs before
// they are inserted, so undo never sees an interim src.
//
// Rules: only rel ".minnotes/" sources are copied into the destination's
// sidecar (they would dangle otherwise); absolute paths, {vol,rel} NAS refs
// and http refs pass through untouched. A same-name same-size file already
// in the destination is reused without a copy (the content-addressed
// pasted-image case); other name collisions get a unique basename. Videos
// carry their .qcview sidecar tree — never clobbering an existing one.
#pragma once

#include "BlockModel.h"
#include "PackageExporter.h"

#include <QHash>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <vector>

class MediaStore;

class AssetTransfer {
public:
    struct Item {
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
    struct Plan {
        QHash<QString, QString> srcRewrite;   // rel src → rel src
        std::vector<Item> items;
        qint64 totalBytes = 0;
        QString sourcePackage;     // splice source ("" = none)
        QString destAssetsDir;
        QString refuse;            // non-empty = refused at plan time
    };
    // Where a rel src's bytes live: a live document's MediaStore, or a
    // clipboard payload's copy-time snapshot re-checked at paste time.
    struct Source {
        QString package;           // the source .mnpkg ("" = none)
        std::function<PackageExporter::Resolved(const QString& rel)> resolve;
        static Source fromStore(MediaStore* store);
    };

    static Plan plan(const std::vector<BlockModel::BlockSpec>& specs,
                     const Source& src, MediaStore* destStore);
    // Worker-safe copy phase (file IO only); rolls back everything it made
    // on cancel or failure.
    static bool copy(const Plan& plan, std::atomic<bool>* cancelFlag,
                     const std::function<void(qint64, qint64, QString)>& progress,
                     QString* error);
    // Cleanup for an apply that never ran (destination closed mid-copy).
    static void removeCopied(const Plan& plan);
    // Apply the src rewrite to every carrier in the specs (media rows, sketch
    // layers, table cell media).
    static void rewriteSpecs(std::vector<BlockModel::BlockSpec>& specs,
                             const QHash<QString, QString>& map);
    // One media descriptor JSON through the map (root src + sketch image
    // layers). Returns the rewritten JSON, or "" when unchanged.
    static QString rewriteMediaJson(const QString& json, const QHash<QString, QString>& map);
    // Visit every media carrier src in the specs, in document order.
    static void forEachSrc(const std::vector<BlockModel::BlockSpec>& specs,
                           const std::function<void(const QJsonValue& src, bool isVideo)>& fn);
    // Every rel ".minnotes/" src the specs reference, deduped, first-seen order.
    static QStringList collectRelSrcs(const std::vector<BlockModel::BlockSpec>& specs);
};
