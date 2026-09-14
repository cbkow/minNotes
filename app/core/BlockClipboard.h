// BlockClipboard — the in-app rich clipboard flavour (0.5.0):
// `application/x-mnd-blocks`, a JSON serialization of a BlockSpec list
// plus what a faithful paste needs beyond the specs — per-block margin ink,
// the bodies of every comment thread the spans anchor, and a copy-time
// snapshot of where each collected (".minnotes/") asset's bytes live, so a
// paste still works after the source tab is closed. Quick-free; the codec
// links into the headless test target.
//
// The payload is NEVER persisted (no document-format bump) but carries its
// own version plus the OLDEST reader version that can decode it (minReader).
// Additive fields bump neither. A decoder refuses a foreign format — including
// the pre-1.0 "minnotes-blocks" payload (no legacy decode) — or a payload whose
// minReader is newer than itself, and the paste falls back to the plain-text
// flavour the same copy always writes.
#pragma once

#include "AssetTransfer.h"
#include "BlockModel.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <vector>

namespace BlockClipboard {

inline constexpr const char* kMime = "application/x-mnd-blocks";
inline constexpr int kVersion = 1;     // what this build writes and understands
inline constexpr int kMinReader = 1;   // the oldest reader this build's payloads need

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
    // A cell fragment copied from a table (SR-4 S8c): {cols: [column spec…], header: n} — the
    // specs are records (no header role) + cells; empty for every other copy.
    QJsonObject grid;
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
