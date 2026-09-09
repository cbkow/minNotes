// BlockClipboard — the in-app rich clipboard flavour (0.5.0):
// `application/x-minnotes-blocks`, a JSON serialization of a BlockSpec list
// plus what a faithful paste needs beyond the specs — per-block margin ink,
// the bodies of every comment thread the spans anchor, and a copy-time
// snapshot of where each collected (".minnotes/") asset's bytes live, so a
// paste still works after the source tab is closed. Quick-free; the codec
// links into the headless test target.
//
// The payload is NEVER persisted (no document-format bump) but carries its
// own version: decoders ignore a foreign format or a newer version and the
// paste falls back to the plain-text flavour the same copy always writes.
#pragma once

#include "AssetTransfer.h"
#include "BlockModel.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <vector>

namespace BlockClipboard {

inline constexpr const char* kMime = "application/x-minnotes-blocks";
inline constexpr int kVersion = 1;

struct Asset {
    QString rel;      // the ".minnotes/…" src as it appears in the specs
    QString abs;      // where the bytes are (or would be) on disk
    QString entry;    // "media/<rel>" when still sealed in the source .mnpkg
    qint64 bytes = 0;
    bool video = false;
};

struct Payload {
    int version = kVersion;
    QString docPath;          // source document identity ("" = untitled)
    QString docDir;           // where ".minnotes/" srcs resolve (BlockModel::mediaAnchorDir)
    QString package;          // the source .mnpkg ("" = none)
    qreal pageWidth = 760;
    std::vector<BlockModel::BlockSpec> specs;
    std::vector<QString> ink;                       // parallel to specs ("" = none)
    std::vector<BlockModel::ThreadImport> threads;  // SOURCE ids — remapped at paste
    std::vector<Asset> assets;
};

QByteArray encode(const Payload& p);
// False (and *error set) on a foreign format, a newer version, or malformed
// JSON. Unknown span kinds are dropped; span offsets are clamped to the text.
bool decode(const QByteArray& json, Payload* out, QString* error = nullptr);

QJsonObject specToJson(const BlockModel::BlockSpec& sp, const QString& ink);
BlockModel::BlockSpec specFromJson(const QJsonObject& o, QString* ink);

// An AssetTransfer source over the payload's snapshot, re-verified against
// the filesystem at paste time (a missing file resolves as broken → the src
// is left as-is, the merger's "honest either side" rule).
AssetTransfer::Source assetSource(const Payload& p);

} // namespace BlockClipboard
