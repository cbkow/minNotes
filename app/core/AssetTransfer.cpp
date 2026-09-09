#include "AssetTransfer.h"
#include "MediaStore.h"
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
#include <QSet>

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

} // namespace

AssetTransfer::Source AssetTransfer::Source::fromStore(MediaStore* store) {
    Source s;
    s.package = store ? store->packageSource() : QString();
    const QHash<QString, qint64> entries =
        s.package.isEmpty() ? QHash<QString, qint64>() : mnpkg::entrySizes(s.package);
    s.resolve = [store, entries](const QString& rel) -> PackageExporter::Resolved {
        if (!store) return {};
        return PackageExporter::resolveNoExtract(store, QJsonValue(rel), entries);
    };
    return s;
}

QString AssetTransfer::rewriteMediaJson(const QString& json, const QHash<QString, QString>& map) {
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

void AssetTransfer::forEachSrc(const std::vector<BlockModel::BlockSpec>& specs,
                               const std::function<void(const QJsonValue&, bool)>& fn) {
    for (const BlockModel::BlockSpec& sp : specs) {
        if (!sp.mediaJson.isEmpty()) {
            const QJsonObject root =
                QJsonDocument::fromJson(sp.mediaJson.toUtf8()).object();
            if (root.value(QStringLiteral("kind")).toString() == QLatin1String("sketch")) {
                for (const QJsonValue& v : root.value(QStringLiteral("images")).toArray())
                    fn(v.toObject().value(QStringLiteral("src")), /*isVideo*/false);
            } else {
                fn(root.value(QStringLiteral("src")),
                   root.value(QStringLiteral("kind")).toString() == QLatin1String("video"));
            }
        } else if (!sp.tableJson.isEmpty()) {
            const TableGrid g = TableGrid::fromJson(sp.tableJson);
            for (int tr = 0; tr < g.rows(); ++tr)
                for (int tc = 0; tc < g.cols(); ++tc) {
                    const QString desc = g.cellMedia(tr, tc);
                    if (desc.isEmpty()) continue;
                    fn(QJsonDocument::fromJson(desc.toUtf8()).object()
                           .value(QStringLiteral("src")), /*isVideo*/false);
                }
        }
    }
}

QStringList AssetTransfer::collectRelSrcs(const std::vector<BlockModel::BlockSpec>& specs) {
    QStringList out;
    QSet<QString> seen;
    forEachSrc(specs, [&](const QJsonValue& v, bool) {
        if (!v.isString()) return;
        const QString rel = v.toString();
        if (!rel.startsWith(QLatin1String(".minnotes/")) || seen.contains(rel)) return;
        seen.insert(rel);
        out << rel;
    });
    return out;
}

AssetTransfer::Plan AssetTransfer::plan(const std::vector<BlockModel::BlockSpec>& specs,
                                        const Source& src, MediaStore* destStore) {
    // Only rel `.minnotes/` sources (disk or still inside a source .mnpkg)
    // are copied — they'd dangle in the destination. Names dodge BOTH the
    // plan and what the dest sidecar already holds; an existing same-name
    // same-size file (the content-addressed pasted-image case) is reused
    // without copying.
    Plan plan;
    plan.sourcePackage = src.package;
    plan.destAssetsDir = destStore
        ? QDir::cleanPath(destStore->docDir()) + QStringLiteral("/.minnotes")
        : QString();

    QSet<QString> taken;
    const QDir destAssets(plan.destAssetsDir);
    for (const QString& name : destAssets.entryList(QDir::Files | QDir::Dirs
                                                    | QDir::NoDotAndDotDot | QDir::Hidden))
        taken.insert(name.toLower());

    auto planSrc = [&](const QJsonValue& v, bool isVideo) {
        if (!v.isString()) return;
        const QString rel = v.toString();
        if (!rel.startsWith(QLatin1String(".minnotes/"))) return;   // linked: pass through
        if (plan.srcRewrite.contains(rel)) return;                  // deduped
        if (!src.resolve) return;
        const PackageExporter::Resolved res = src.resolve(rel);
        if (!res.ok) return;   // broken ref stays as-is — honest either side
        const QString base = QFileInfo(res.absPath).fileName();
        const QFileInfo existing(plan.destAssetsDir + QLatin1Char('/') + base);
        if (existing.exists() && existing.isFile() && existing.size() == res.bytes) {
            plan.srcRewrite.insert(rel, QStringLiteral(".minnotes/") + base);
            if (isVideo) {
                // Reused byte-identical video: its .qcview tree must still
                // ride when the destination has none (a sidecar-only item;
                // copy never clobbers an existing dest sidecar).
                Item sc;
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
                    plan.items.push_back(std::move(sc));
            }
            return;                                                 // identical: reuse
        }
        Item it;
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
        plan.srcRewrite.insert(rel, QStringLiteral(".minnotes/") + it.destName);
        plan.totalBytes += it.bytes;
        plan.items.push_back(std::move(it));
    };
    forEachSrc(specs, planSrc);

    if (!plan.items.empty() && plan.destAssetsDir.isEmpty())
        plan.refuse = QStringLiteral("Destination has no media folder");
    return plan;
}

bool AssetTransfer::copy(const Plan& plan, std::atomic<bool>* cancelFlag,
                         const std::function<void(qint64, qint64, QString)>& progress,
                         QString* error) {
    QStringList madeFiles, madeDirs;
    auto fail = [&](const QString& why) {
        for (const QString& f : madeFiles) QFile::remove(f);
        for (const QString& d : madeDirs) QDir(d).removeRecursively();
        if (error) *error = why;
        return false;
    };
    QDir().mkpath(plan.destAssetsDir);
    qint64 done = 0;
    for (const Item& it : plan.items) {
        if (cancelFlag && cancelFlag->load()) return fail(QStringLiteral("Cancelled"));
        if (progress) progress(done, plan.totalBytes, it.destName);
        const QString dest = plan.destAssetsDir + QLatin1Char('/') + it.destName;
        if (!it.packageEntry.isEmpty()) {
            // Bytes still sealed in the source .mnpkg — stream the entry out.
            if (!mnpkg::extractEntry(plan.sourcePackage, it.packageEntry, dest))
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
                if (progress) progress(done + fileDone, plan.totalBytes, it.destName);
            }
        }
        // else: a sidecar-only item (a reused byte-identical video).
        if (it.isVideo && (!it.sidecarDir.isEmpty() || !it.pkgSidecarPrefix.isEmpty())) {
            // Sidecar tree rides along — content 1:1, association by layout.
            // An existing dest tree is NEVER touched (its annotations win).
            const QString sdst = plan.destAssetsDir + QStringLiteral("/.qcview/")
                + qcv::annotation_io::sanitizeMediaName(it.destName);
            if (!QFileInfo::exists(sdst)) {
                madeDirs << sdst;
                if (!it.pkgSidecarPrefix.isEmpty()) {
                    mnpkg::extractMatching(plan.sourcePackage, it.pkgSidecarPrefix,
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
        if (progress) progress(done, plan.totalBytes, it.destName);
    }
    return true;
}

void AssetTransfer::removeCopied(const Plan& plan) {
    for (const Item& it : plan.items) {
        // Sidecar-only items point at a REUSED pre-existing file (and
        // possibly the dest's own sidecar) — never remove those.
        if (it.srcPath.isEmpty() && it.packageEntry.isEmpty()) continue;
        QFile::remove(plan.destAssetsDir + QLatin1Char('/') + it.destName);
        if (it.isVideo)
            QDir(plan.destAssetsDir + QStringLiteral("/.qcview/")
                 + qcv::annotation_io::sanitizeMediaName(it.destName)).removeRecursively();
    }
}

void AssetTransfer::rewriteSpecs(std::vector<BlockModel::BlockSpec>& specs,
                                 const QHash<QString, QString>& map) {
    if (map.isEmpty()) return;
    for (BlockModel::BlockSpec& sp : specs) {
        if (!sp.mediaJson.isEmpty()) {
            const QString r = rewriteMediaJson(sp.mediaJson, map);
            if (!r.isEmpty()) sp.mediaJson = r;
        } else if (!sp.tableJson.isEmpty()) {
            TableGrid g = TableGrid::fromJson(sp.tableJson);
            bool changed = false;
            for (int tr = 0; tr < g.rows(); ++tr)
                for (int tc = 0; tc < g.cols(); ++tc) {
                    const QString desc = g.cellMedia(tr, tc);
                    if (desc.isEmpty()) continue;
                    const QString r = rewriteMediaJson(desc, map);
                    if (!r.isEmpty()) { g.setCellMedia(tr, tc, r); changed = true; }
                }
            if (changed) sp.tableJson = g.toJson();
        }
    }
}
