#include "BlockModel.h"
#include "Importer.h"
#include "AssetTransfer.h"
#include "BlockClipboard.h"
#include "MediaStore.h"
#include "TableGrid.h"
#include "PathMap.h"
#include "CodeSyntax.h"                     // the language chip's picker feed
#include "../notes/sketch_text.h"
#include "../notes/doc_ink.h"               // page-width ink migration (setPageWidth)
#include "../notes/annotation_thumbnail.h"  // qcv::strokeBoundsNorm (oval-aware bbox)
#include <QStringBuilder>
#include <QStandardPaths>
#include <QDir>
#include <QDirIterator>
#include <QPointer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QUrl>
#include <QSet>
#include <QFileInfo>
#include <QProcess>
#include <QDesktopServices>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextList>
#include <QTextTable>
#include <QTextFrame>
#include <QTextCharFormat>
#include <QImage>
#include <QPixmap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDateTime>
#include <functional>
#include <algorithm>
#include <limits>
#include <QFont>
#include <QFontMetricsF>
#include <cmath>
#include <cstdio>
#include "PackageFormat.h"   // mnpkg::atomicReplace — the save write-back primitive

namespace {
// Deterministic per-row hash (no RNG: must be stable across rebuilds and frames).
inline uint32_t rowHash(int row) {
    uint32_t x = static_cast<uint32_t>(row) * 2654435761u + 0x9E3779B9u;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15;
    return x;
}

// Spike layout constants (square-pixel; real heights come from the delegate).
constexpr double kLine     = 22.0;   // px per text line
constexpr double kPadV     = 16.0;   // vertical padding per block
constexpr double kHeading  = 40.0;
constexpr double kWidthEst = 760.0;  // assumed content width for media estimate
constexpr double kVideoBar = 40.0;   // transport toolbar reserved under a video
constexpr double kFileChip = 56.0;   // fixed height of an unsupported-file attachment chip
constexpr double kPdfNav   = 40.0;   // page-nav strip reserved under an inline PDF page

// Span kinds whose `href` field carries a payload (URL for links, color hex for
// color/highlight, thread id for comments) — serialized as "u", and pushed
// whole (never merged by kind).
static inline bool spanHasPayload(uint8_t k) { return mn::inl::hasPayload(k); }
                                     // (keep in sync with Editor.qml videoTransportH)

// Fractional-rank alphabet: 62 digits in ascending ASCII order, so plain
// string comparison == numeric order. Shared by encode62 + rankBetween.
const QString kRankAlpha =
    QStringLiteral("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");

// Fixed-width base-62 of v. Used to seed evenly-spaced, non-minimum ranks
// (NOT all-'0', which would leave no room for inserts before the first block).
QString encode62(quint64 v, int width) {
    QString s(width, QLatin1Char('0'));
    for (int i = width - 1; i >= 0; --i) { s[i] = kRankAlpha[int(v % 62)]; v /= 62; }
    return s;
}
}

BlockModel::BlockModel(QObject* parent) : QAbstractListModel(parent) {
    // No document at launch — the app opens to the welcome / no-doc state; the
    // File menu (or a recent) opens or creates one. (The model stays empty and
    // every mutation is doc-guarded until a document is loaded.)
    untitled_ = false;

    // The QML `count` property reads rowCountQml; its NOTIFY is countChanged.
    // Wire countChanged to every structural change so bindings like poolSize
    // (Math.min(blockModel.count, cap)) re-evaluate on incremental insert/remove
    // and not only on full model resets (modelReset).
    connect(this, &QAbstractItemModel::rowsInserted, this, &BlockModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved,  this, &BlockModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset,   this, &BlockModel::countChanged);
}

// --- Document lifecycle ----------------------------------------------------

QString BlockModel::documentName() const {
    if (!documentOpen()) return QString();
    // Package views are untitled (snapshot semantics) but keep the package's
    // name — the tab should say what you're looking at.
    if (untitled_ && pkgDir_.isEmpty()) return QStringLiteral("Untitled");
    return QFileInfo(docPath_).completeBaseName();   // basename without .mnd/.mnpkg
}

void BlockModel::closeDocument() {
    doc_.close();
    cleanupScratch();            // working copy is ephemeral — discard it on close
    mediaStore_.reset();
    docPath_.clear();
    untitled_ = false;
    dirty_ = false;
    setSaveState(SaveClean);
    beginResetModel();
    rows_.clear(); ids_.clear(); ranks_.clear(); content_.clear();
    reindex(std::vector<double>{});
    endResetModel();
    clearUndo();
    if (!inkByBlock_.isEmpty()) { inkByBlock_.clear(); ++inkRevision_; emit inkChanged(); }
    if (!qFuzzyCompare(pageWidth_, 760.0)) { pageWidth_ = 760; emit pageWidthChanged(); }
    emit documentChanged();
    emit dirtyChanged();
}

// Recursively copy <src>/.minnotes → <dst>/.minnotes (pasted media). Referenced
// files live at absolute paths and need no copy. No-op if src == dst.
static void copyDirRecursive(const QString& src, const QString& dst) {
    QDir sd(src);
    if (!sd.exists()) return;
    QDir().mkpath(dst);
    for (const QFileInfo& fi : sd.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString d = dst + QLatin1Char('/') + fi.fileName();
        if (fi.isDir()) copyDirRecursive(fi.absoluteFilePath(), d);
        else { QFile::remove(d); QFile::copy(fi.absoluteFilePath(), d); }
    }
}
static void copyMediaSidecar(const QString& srcDocDir, const QString& dstDocDir) {
    if (srcDocDir == dstDocDir) return;
    copyDirRecursive(srcDocDir + QStringLiteral("/.minnotes"),
                     dstDocDir + QStringLiteral("/.minnotes"));
}

// Replace `dst` with `src` atomically (same directory → same filesystem), so an
// interrupted save never leaves a half-written original. Definition lives in
// PackageFormat (shared with package repack).
static bool atomicReplace(const QString& src, const QString& dst) {
    return mnpkg::atomicReplace(src, dst);
}

static QString g_scratchRoot;   // set once by main() to this session's subdir

void BlockModel::setScratchRoot(const QString& dir) { g_scratchRoot = dir; }

QString BlockModel::scratchDir() {
    if (!g_scratchRoot.isEmpty()) return g_scratchRoot;
    // Fallback (e.g. unit tests that construct BlockModel without main()).
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
         + QStringLiteral("/scratch");
}

QString BlockModel::newScratchPath() {
    const QString dir = scratchDir();
    QDir().mkpath(dir);
    return dir + QStringLiteral("/work-") + makeUlid() + QStringLiteral(".mnd");
}

void BlockModel::cleanupScratch() {
    if (!pkgDir_.isEmpty()) {          // package extraction dir (db + media)
        QDir(pkgDir_).removeRecursively();
        pkgDir_.clear();
    }
    if (scratchPath_.isEmpty()) return;
    QFile::remove(scratchPath_);
    QFile::remove(scratchPath_ + QStringLiteral("-wal"));
    QFile::remove(scratchPath_ + QStringLiteral("-shm"));
    scratchPath_.clear();
}

QString BlockModel::mediaAnchorDir() const {
    return pkgDir_.isEmpty() ? QFileInfo(docPath_).absolutePath() : pkgDir_;
}

void BlockModel::recordOriginalStat() {
    QFileInfo fi(docPath_);
    if (!untitled_ && fi.exists()) {
        origMtime_ = fi.lastModified().toMSecsSinceEpoch();
        origSize_  = fi.size();
    } else { origMtime_ = -1; origSize_ = -1; }
}

bool BlockModel::externalChangeDetected() const {
    if (untitled_ || origMtime_ < 0) return false;   // no baseline → nothing to conflict with
    QFileInfo fi(docPath_);
    if (!fi.exists()) return false;                  // original gone → recreate, nothing to clobber
    return fi.lastModified().toMSecsSinceEpoch() != origMtime_ || fi.size() != origSize_;
}

void BlockModel::markDirty() {
    if (dirty_) return;
    dirty_ = true;
    emit dirtyChanged();
}

void BlockModel::setSaveState(int s) {
    if (saveState_ == s) return;
    saveState_ = s;
    emit saveStateChanged();
}

bool BlockModel::loadDocument(const QString& path, bool untitled) {
    // We never run SQLite on the document's real location (network filesystems
    // can't back WAL/-shm or honour locking — see the plan). Stage a LOCAL working
    // copy and open THAT; the original is only ever read (byte copy) and written
    // (atomic replace on save). `path` stays the identity + media anchor.
    doc_.close();
    cleanupScratch();
    lastOpenError_.clear();

    // .mnpkg fork: packages are SEALED SNAPSHOTS (user ruling 2026-08-18 —
    // .mnd is the ONE editable format; a package is produced by Export and
    // received for viewing). Opening one is a LAZY read: only document.mnd
    // extracts up front (instant regardless of package size); media stays in
    // the archive until something touches it (MediaStore::resolvePath pulls
    // `media/<x>` → `.minnotes/<x>` on first access; videos bring their
    // .qcview sidecar). The doc opens with UNTITLED semantics — you can read,
    // play, even type, but nothing persists until Save As materializes a
    // real .mnd; the package file itself is NEVER written. docPath_ stays
    // the .mnpkg for tab dedupe, recents, and documentName.
    if (!untitled && mnpkg::isPackagePath(path)) {
        if (!QFileInfo::exists(path)) return false;
        // The 1.0 clean break (R-I6 6b): a package from an earlier minNotes —
        // or one with no manifest — is refused, not converted.
        const QJsonObject manifest = mnpkg::readManifest(path);
        if (manifest.value(QStringLiteral("format")).toString() != QLatin1String("mnpkg")
            || manifest.value(QStringLiteral("formatVersion")).toInt() != mnpkg::kFormatVersion) {
            lastOpenError_ = tr("This package was made with an earlier minNotes and can't be opened.");
            return false;
        }
        pkgDir_ = scratchDir() + QStringLiteral("/pkg-") + makeUlid();
        scratchPath_ = pkgDir_ + QStringLiteral("/document.mnd");
        if (!mnpkg::extractEntry(path, QLatin1String(mnpkg::kDbEntry), scratchPath_)) {
            qWarning() << "BlockModel: package has no readable document.mnd" << path;
            cleanupScratch();
            return false;
        }
        QFile::setPermissions(scratchPath_, QFile::ReadOwner | QFile::WriteOwner
                                          | QFile::ReadUser  | QFile::WriteUser);
        if (!doc_.open(scratchPath_)) {
            qWarning() << "BlockModel: cannot open package working copy" << scratchPath_;
            cleanupScratch();
            return false;
        }
        if (!acceptOpenedFormat()) return false;
        docPath_ = path;
        untitled_ = true;   // snapshot: save() routes to Save As, no conflict baseline
        mediaStore_ = std::make_unique<MediaStore>(scratchPath_);  // anchored to the extraction
        mediaStore_->setPackageSource(path);                       // lazy media source
        // QCView sidecars extract EAGERLY (notes.json + thumbs — small):
        // note models and file watchers anchor to real files immediately,
        // while the videos themselves STREAM from the archive and never
        // extract for playback.
        mnpkg::extractMatching(path, QStringLiteral("media/.qcview/"),
                               QStringLiteral("media/"),
                               pkgDir_ + QStringLiteral("/.minnotes"));
        // A background extraction landing bumps contentRevision (queued to
        // the GUI thread) so display bindings re-resolve and reveal.
        {
            QPointer<BlockModel> self(this);
            mediaStore_->setLazyNotify([self] {
                if (self)
                    QMetaObject::invokeMethod(self, [self] {
                        if (self) self->refreshMedia();
                    }, Qt::QueuedConnection);
            });
        }
        recordOriginalStat();
        dirty_ = false;
        setSaveState(SaveClean);
        clearUndo();
        return true;
    }

    scratchPath_ = newScratchPath();
    if (!untitled && QFileInfo::exists(path)) {
        QFile::remove(scratchPath_);
        if (!QFile::copy(path, scratchPath_)) {
            qWarning() << "BlockModel: cannot stage working copy of" << path;
            scratchPath_.clear();
            return false;
        }
        // The source may be read-only (read-only share); the working copy must be
        // writable for WAL.
        QFile::setPermissions(scratchPath_, QFile::ReadOwner | QFile::WriteOwner
                                          | QFile::ReadUser  | QFile::WriteUser);
        // Carry a sibling -wal so uncommitted data from an unclean prior writer is
        // recovered into the working copy (normally absent — saves are checkpointed).
        if (QFileInfo::exists(path + QStringLiteral("-wal")))
            QFile::copy(path + QStringLiteral("-wal"), scratchPath_ + QStringLiteral("-wal"));
    }
    if (!doc_.open(scratchPath_)) {
        qWarning() << "BlockModel: cannot open working copy" << scratchPath_;
        cleanupScratch();
        return false;
    }
    if (!untitled && QFileInfo::exists(path) && !acceptOpenedFormat()) return false;
    docPath_ = path;
    untitled_ = untitled;
    mediaStore_ = std::make_unique<MediaStore>(path);   // media anchored to the ORIGINAL folder
    recordOriginalStat();
    dirty_ = false;
    setSaveState(SaveClean);
    clearUndo();
    return true;
}

// === The layout index (SR-3) ================================================

std::vector<mn::LayoutIndex::Entry> BlockModel::layoutEntries() const {
    std::vector<mn::LayoutIndex::Entry> e(rows_.size());
    const std::vector<int>& heads = tableHeads();
    const int n = static_cast<int>(rows_.size());
    for (size_t i = 0; i < rows_.size(); ++i) {
        e[i] = { rows_[i].type == Split && rows_[i].cell < 0, rows_[i].cell };
        if (!e[i].split || heads[i] < 0) continue;
        // SR-4 S5b: a table's pocket — the old Table block's 32 px above and below.
        if (heads[i] == static_cast<int>(i)) e[i].padTop = kTablePocket;
        const int next = splitRowEnd(static_cast<int>(i)) + 1;
        if (next >= n || rows_[size_t(next)].type != Split || rows_[size_t(next)].cell >= 0 || rows_[size_t(next)].header > 0)
            e[i].padBottom = kTablePocket;
    }
    return e;
}

const mn::LayoutIndex& BlockModel::layout() const {
    if (indexDirty_ && pendingHeights_.size() == rows_.size()) {
        if (layout_.reset(layoutEntries(), pendingHeights_)) {
            indexDirty_ = false;
        } else {
            // A structure the index can't represent — a mutation part-way, or a
            // bug: index flat so nothing reads garbage, and retry on the next query.
            layout_.reset(std::vector<mn::LayoutIndex::Entry>(rows_.size()), pendingHeights_);
        }
    }
    return layout_;
}

void BlockModel::reindex(std::vector<double> heights) {
    pendingHeights_ = std::move(heights);
    indexDirty_ = true;
    tablesDirty_ = true;
}

void BlockModel::indexInsert(std::size_t at, double h) {
    tablesDirty_ = true;
    if (!indexDirty_) { pendingHeights_ = layout_.heights(); indexDirty_ = true; }
    at = std::min(at, pendingHeights_.size());
    pendingHeights_.insert(pendingHeights_.begin() + static_cast<std::ptrdiff_t>(at), h);
}

void BlockModel::indexErase(std::size_t at) {
    tablesDirty_ = true;
    if (!indexDirty_) { pendingHeights_ = layout_.heights(); indexDirty_ = true; }
    if (at < pendingHeights_.size())
        pendingHeights_.erase(pendingHeights_.begin() + static_cast<std::ptrdiff_t>(at));
}

double BlockModel::setIndexHeight(std::size_t row, double h) {
    layout();
    if (indexDirty_) {                     // mid-structure-edit: stage it, rebuild later
        if (row < pendingHeights_.size()) pendingHeights_[row] = h;
        return 0.0;
    }
    return layout_.setHeight(row, h);
}

int BlockModel::laneForRow(int row) const {
    return (row >= 0 && row < static_cast<int>(rows_.size())) ? rows_[size_t(row)].cell : -1;
}

int BlockModel::splitRowOf(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return -1;
    if (rows_[size_t(row)].type == Split && rows_[size_t(row)].cell < 0) return row;
    if (rows_[size_t(row)].cell < 0) return -1;
    int i = row;
    while (i > 0 && rows_[size_t(i)].cell >= 0) --i;
    return rows_[size_t(i)].type == Split ? i : -1;
}

int BlockModel::laneCount(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[size_t(row)].type != Split) return 0;
    return layout().cellCount(size_t(row));
}

QVariantList BlockModel::splitRatios(int row) const {
    QVariantList out;
    if (row >= 0 && row < static_cast<int>(rows_.size()))
        for (float f : rows_[size_t(row)].ratios) out.push_back(double(f));
    return out;
}

bool BlockModel::structureValid() const {
    for (const Row& r : rows_) {
        if (r.type == Split && r.cell >= 0) return false;       // I3: no record inside a lane
        if (r.header > 0 && r.type != Split) return false;      // SR-4: only records head tables
    }
    mn::LayoutIndex probe;
    if (!probe.reset(layoutEntries(), std::vector<double>(rows_.size(), 1.0))) return false;   // I1
    const std::vector<int>& heads = tableHeads();
    for (size_t s = 0; s < probe.topCount(); ++s) {
        const size_t rec = probe.flatOfSlot(s);
        if (rows_[rec].type != Split) continue;
        const int lanes = probe.cellCount(rec);
        if (heads[rec] >= 0) { if (lanes < 1) return false; continue; }   // table rows: C ≥ 1, ragged
        if (lanes < 2 || static_cast<int>(rows_[rec].ratios.size()) != lanes) return false;   // I2, I4
    }
    return true;
}

// === Derived tables (SR-4) ================================================
const std::vector<int>& BlockModel::tableHeads() const {
    if (!tablesDirty_ && tableHeads_.size() == rows_.size()) return tableHeads_;
    tableHeads_.assign(rows_.size(), -1);
    int cur = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        if (r.cell < 0) cur = r.type != Split ? -1 : r.header > 0 ? static_cast<int>(i) : cur;
        tableHeads_[i] = cur;
    }
    tablesDirty_ = false;
    tableGeomDirty_ = true;                           // heads may have moved: geometry follows
    return tableHeads_;
}

// The app's auto column-width rule (mirrors exportColWidth): the widest text line in the
// body font, the head row reserving the sort-glyph slot, + padding, clamped [48, 360].
static double tableTextWidth(const QString& text) {
    static const QFontMetricsF fm = [] {
        QFont f(QStringLiteral("Aspekta"));
        f.setPixelSize(14);
        return QFontMetricsF(f);
    }();
    double w = 0.0;
    for (const QString& line : text.split(QLatin1Char('\n'))) w = std::max(w, fm.horizontalAdvance(line));
    return w;
}

BlockModel::TableGeom BlockModel::buildTableGeom(int head) const {
    TableGeom g;
    const int n = static_cast<int>(rows_.size());
    const QJsonArray spec = QJsonDocument::fromJson(rows_[size_t(head)].table.toUtf8())
                                .object().value(QStringLiteral("cols")).toArray();
    int cols = static_cast<int>(spec.size());
    std::vector<double> widest;
    std::vector<char> measurable;
    for (int rec = head; rec < n && rows_[size_t(rec)].type == Split && rows_[size_t(rec)].cell < 0
                         && (rec == head || rows_[size_t(rec)].header == 0); rec = splitRowEnd(rec) + 1) {
        for (int i = rec + 1; i <= splitRowEnd(rec); ++i) {
            const Row& b = rows_[size_t(i)];
            if (b.cell < 0) continue;
            cols = std::max(cols, b.cell + 1);
            if (b.type == Media || b.type == Divider || b.type == Table || content_[size_t(i)].isEmpty()) continue;
            if (b.natW < 0) b.natW = static_cast<int32_t>(std::lround(tableTextWidth(content_[size_t(i)])));
            if (widest.size() < size_t(cols)) { widest.resize(size_t(cols), 0.0); measurable.resize(size_t(cols), 0); }
            widest[size_t(b.cell)] = std::max(widest[size_t(b.cell)], b.natW + (rec == head ? 18.0 : 0.0));
            measurable[size_t(b.cell)] = 1;
        }
    }
    widest.resize(size_t(cols), 0.0);
    measurable.resize(size_t(cols), 0);
    g.x.resize(size_t(cols));
    g.w.resize(size_t(cols));
    for (int c = 0; c < cols; ++c) {
        const int manual = c < spec.size() ? spec[c].toObject().value(QStringLiteral("w")).toInt(0) : 0;
        const double w = manual > 0 ? double(manual)
                       : measurable[size_t(c)] ? std::clamp(std::round(widest[size_t(c)] + 2 * 8 + 6), 48.0, 360.0)
                                               : 160.0;
        g.x[size_t(c)] = g.width;
        g.w[size_t(c)] = w;
        g.width += w;
    }
    return g;
}

const BlockModel::TableGeom* BlockModel::tableGeom(int head) const {
    const std::vector<int>& heads = tableHeads();      // first: a regrouping dirties geometry
    if (tableGeomDirty_ || geomRows_ != rows_.size()) {
        geoms_.clear();
        for (size_t i = 0; i < rows_.size(); ++i)
            if (heads[i] == static_cast<int>(i)) geoms_.insert(static_cast<int>(i), buildTableGeom(static_cast<int>(i)));
        geomRows_ = rows_.size();
        tableGeomDirty_ = false;
    }
    const auto it = geoms_.constFind(head);
    return it == geoms_.constEnd() ? nullptr : &it.value();
}

qreal BlockModel::tablePadTop(int row) const {
    const mn::LayoutIndex& li = layout();
    return row >= 0 && size_t(row) < li.size() ? li.entry(size_t(row)).padTop : 0.0;
}

qreal BlockModel::tablePadBottom(int row) const {
    const mn::LayoutIndex& li = layout();
    return row >= 0 && size_t(row) < li.size() ? li.entry(size_t(row)).padBottom : 0.0;
}

QString BlockModel::gridCellText(int head, int r, int c) const {
    QStringList parts;
    for (const QVariant& v : gridCellRows(head, r, c)) parts << content_[size_t(v.toInt())];
    return parts.join(QLatin1Char('\n'));
}

QVariantMap BlockModel::tableStickyAt(qreal y) const {
    const int top = rowForY(y);
    if (top < 0 || top >= static_cast<int>(rows_.size()) || rows_[size_t(top)].type != Split) return {};
    const int head = tableHeadOf(top);
    if (head < 0) return {};
    const QVariantList recs = tableRecords(head);
    const int hc = std::min(static_cast<int>(rows_[size_t(head)].header), static_cast<int>(recs.size()));
    if (hc <= 0 || recs.size() <= hc) return {};                // no body rows to scroll under it
    const mn::LayoutIndex& li = layout();
    const size_t lastHeader = size_t(recs[hc - 1].toInt()), last = size_t(recs.back().toInt());
    QVariantList headerRows;
    for (int k = 0; k < hc; ++k) headerRows << recs[k];
    return {
        { QStringLiteral("head"), head },
        { QStringLiteral("headerTop"), li.y(size_t(head)) + li.entry(size_t(head)).padTop },
        { QStringLiteral("headerBottom"), li.y(lastHeader) + li.height(lastHeader) },
        { QStringLiteral("tableBottom"), li.y(last) + li.height(last) - li.entry(last).padBottom },
        { QStringLiteral("headerRows"), headerRows },
    };
}

qreal BlockModel::tableColumnWidth(int head, int column) const {
    const TableGeom* g = tableGeom(head);
    return g && column >= 0 && size_t(column) < g->w.size() ? g->w[size_t(column)] : 0.0;
}

qreal BlockModel::tableColumnLeft(int head, int column) const {
    const TableGeom* g = tableGeom(head);
    if (!g || column < 0) return 0.0;
    return size_t(column) < g->x.size() ? g->x[size_t(column)] : g->width;
}

qreal BlockModel::tableWidth(int head) const {
    const TableGeom* g = tableGeom(head);
    return g ? g->width : 0.0;
}

// === Grid addressing and structure (SR-4 S3a) ==============================
int BlockModel::gridRecord(int head, int r) const {
    const QVariantList recs = tableRecords(head);
    return r >= 0 && r < recs.size() ? recs[r].toInt() : -1;
}

std::pair<int,int> BlockModel::tableBand(int head) const {
    const QVariantList recs = tableRecords(head);
    if (recs.isEmpty()) return { -1, -2 };
    return { head, splitRowEnd(recs.back().toInt()) };
}

std::vector<BlockModel::GridRow> BlockModel::gridOf(int head) const {
    std::vector<GridRow> grid;
    for (const QVariant& v : tableRecords(head)) {
        GridRow gr;
        gr.rec = v.toInt();
        for (int i = gr.rec + 1; i <= splitRowEnd(gr.rec); ++i) {
            const int c = rows_[size_t(i)].cell;
            if (c < 0) continue;
            if (static_cast<int>(gr.cells.size()) <= c) gr.cells.resize(size_t(c) + 1);
            gr.cells[size_t(c)].blocks.push_back(i);
        }
        const QJsonObject t = QJsonDocument::fromJson(rows_[size_t(gr.rec)].table.toUtf8()).object();
        const QJsonArray cbg = t.value(QStringLiteral("cbg")).toArray(), cfg = t.value(QStringLiteral("cfg")).toArray();
        for (size_t c = 0; c < gr.cells.size(); ++c) {
            gr.cells[c].bg = cbg.at(qsizetype(c)).toString();
            gr.cells[c].fg = cfg.at(qsizetype(c)).toString();
        }
        grid.push_back(std::move(gr));
    }
    return grid;
}

int BlockModel::gridRowCount(int head) const { return static_cast<int>(tableRecords(head).size()); }

int BlockModel::gridCellCount(int head, int r) const {
    const int rec = gridRecord(head, r);
    if (rec < 0) return 0;
    int cells = 0;
    for (int i = rec + 1; i <= splitRowEnd(rec); ++i) cells = std::max(cells, rows_[size_t(i)].cell + 1);
    return cells;
}

QVariantList BlockModel::gridCellRows(int head, int r, int c) const {
    const int rec = gridRecord(head, r);
    QVariantList out;
    if (rec < 0) return out;
    for (int i = rec + 1; i <= splitRowEnd(rec); ++i)
        if (rows_[size_t(i)].cell == c) out << i;
    return out;
}

int BlockModel::gridCellAt(int head, int r, int c) const {
    const QVariantList rows = gridCellRows(head, r, c);
    return rows.isEmpty() ? -1 : rows.front().toInt();
}

int BlockModel::gridRowOf(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return -1;
    const int rec = rows_[size_t(row)].cell >= 0 ? splitRowOf(row) : row;
    const int head = tableHeadOf(rec);
    if (rec < 0 || head < 0) return -1;
    return static_cast<int>(tableRecords(head).indexOf(QVariant(rec)));
}

int BlockModel::gridColumnOf(int row) const {
    return (row >= 0 && row < static_cast<int>(rows_.size()) && tableHeadOf(row) >= 0) ? rows_[size_t(row)].cell : -1;
}

int BlockModel::refillTableCells(int lo, int hi) {
    int inserted = 0;
    for (int rec = lo; rec <= hi && rec < static_cast<int>(rows_.size());) {
        if (rows_[size_t(rec)].type != Split || rows_[size_t(rec)].cell >= 0) { ++rec; continue; }
        const int end = splitRowEnd(rec);
        if (tableHeadOf(rec) < 0) { rec = end + 1; continue; }
        std::vector<std::pair<int,int>> todo;         // (lane, insert at), in document order
        int expect = 0;
        for (int i = rec + 1; i <= end; ++i) {
            const int lane = rows_[size_t(i)].cell;
            if (i > rec + 1 && lane == rows_[size_t(i - 1)].cell) continue;
            for (int k = expect; k < lane; ++k) todo.push_back({ k, i });   // interior lanes that emptied
            expect = std::max(expect, lane + 1);
        }
        if (end == rec) todo.push_back({ 0, rec + 1 });                   // every cell emptied
        for (auto it = todo.rbegin(); it != todo.rend(); ++it) {         // bottom-up keeps positions valid
            Row p{}; p.type = Paragraph; p.param = 1; p.cell = static_cast<int8_t>(it->first);
            const int at = it->second;
            insertRowRaw(at, p, rankBetween(ranks_[size_t(at - 1)],
                                            at < static_cast<int>(rows_.size()) ? ranks_[size_t(at)] : QString()));
        }
        inserted += static_cast<int>(todo.size());
        hi += static_cast<int>(todo.size());
        rec = splitRowEnd(rec) + 1;
    }
    return inserted;
}

void BlockModel::rebuildTable(int lo, int hi, const std::vector<GridRow>& grid, const QJsonArray& cols, int headerCount) {
    std::vector<Row> nr;
    std::vector<QString> ni, nc;
    auto emptyParagraph = [&](int c) {
        Row p{}; p.type = Paragraph; p.param = 1; p.cell = static_cast<int8_t>(c);
        nr.push_back(p); ni.push_back(makeUlid()); nc.push_back(QString());
    };
    for (size_t k = 0; k < grid.size(); ++k) {
        const GridRow& gr = grid[k];
        Row rec{};
        QString recId;
        if (gr.rec >= 0) { rec = rows_[size_t(gr.rec)]; recId = gr.copy ? makeUlid() : ids_[size_t(gr.rec)]; }
        else { rec.type = Split; rec.param = 1; recId = makeUlid(); }
        QJsonObject t = QJsonDocument::fromJson(rec.table.toUtf8()).object();
        t.remove(QStringLiteral("cols"));
        if (k == 0) t.insert(QStringLiteral("cols"), cols);         // the head: role + column spec
        QJsonArray cbg, cfg;                                         // cell colours ride their cells
        for (const GridCell& cell : gr.cells) { cbg.append(cell.bg); cfg.append(cell.fg); }
        while (!cbg.isEmpty() && cbg.last().toString().isEmpty()) cbg.removeLast();
        while (!cfg.isEmpty() && cfg.last().toString().isEmpty()) cfg.removeLast();
        if (cbg.isEmpty()) t.remove(QStringLiteral("cbg")); else t.insert(QStringLiteral("cbg"), cbg);
        if (cfg.isEmpty()) t.remove(QStringLiteral("cfg")); else t.insert(QStringLiteral("cfg"), cfg);
        rec.header = k == 0 ? static_cast<uint8_t>(std::clamp(headerCount, 1, 255)) : uint8_t(0);
        rec.table = t.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(t).toJson(QJsonDocument::Compact));
        rec.cell = -1;
        const int cells = std::max(1, static_cast<int>(gr.cells.size()));
        rec.ratios.assign(size_t(cells), 1.0f / static_cast<float>(cells));
        nr.push_back(rec); ni.push_back(recId); nc.push_back(QString());
        for (int c = 0; c < cells; ++c) {
            if (c >= static_cast<int>(gr.cells.size()) || gr.cells[size_t(c)].blocks.empty()) { emptyParagraph(c); continue; }
            for (int v : gr.cells[size_t(c)].blocks) {
                const bool copy = v <= -2;
                const int i = copy ? -v - 2 : v;
                Row b = rows_[size_t(i)];
                b.cell = static_cast<int8_t>(c);
                b.natW = -1;
                nr.push_back(b); ni.push_back(copy ? makeUlid() : ids_[size_t(i)]); nc.push_back(content_[size_t(i)]);
            }
        }
    }
    replaceBand(lo, hi, nr, ni, nc);
    tablesDirty_ = true;
    tableGeomDirty_ = true;
}

bool BlockModel::commitGrid(int head, const std::vector<GridRow>& grid, const QJsonArray& cols, int headerCount) {
    if (grid.empty()) return false;
    const auto [lo, hi] = tableBand(head);
    beginTxn(lo, hi);
    rebuildTable(lo, hi, grid, cols, headerCount);
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return true;
}

static QJsonArray tableColsOf(const QString& table) {
    return QJsonDocument::fromJson(table.toUtf8()).object().value(QStringLiteral("cols")).toArray();
}

// A copy of a cell: every block copied (new ids), its colours too.
BlockModel::GridCell BlockModel::copiedCell(const GridCell& cell) {
    GridCell out = cell;
    for (int& v : out.blocks)
        if (v >= 0) v = -v - 2;
    return out;
}

bool BlockModel::gridInsertRow(int head, int at) {
    std::vector<GridRow> grid = gridOf(head);
    if (grid.empty()) return false;
    at = std::clamp(at, 0, static_cast<int>(grid.size()));
    GridRow fresh;
    fresh.cells.resize(grid[size_t(at > 0 ? at - 1 : 0)].cells.size());
    grid.insert(grid.begin() + at, fresh);
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), rows_[size_t(head)].header);
}

bool BlockModel::gridDeleteRow(int head, int r) {
    std::vector<GridRow> grid = gridOf(head);
    const int rows = static_cast<int>(grid.size()), hc = headerCount(head);
    if (r < 0 || r >= rows) return false;
    if (r < hc) return gridDeleteTable(head);                   // "Delete row" on a header row = the table
    if (rows - hc <= 1) return false;                           // the last body row stays
    grid.erase(grid.begin() + r);
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), hc);
}

bool BlockModel::gridMoveRow(int head, int from, int to) {
    std::vector<GridRow> grid = gridOf(head);
    const int rows = static_cast<int>(grid.size());
    if (from < 0 || from >= rows) return false;
    to = std::clamp(to, 0, rows - 1);
    if (to == from) return true;
    GridRow moved = grid[size_t(from)];
    grid.erase(grid.begin() + from);
    grid.insert(grid.begin() + to, moved);
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), rows_[size_t(head)].header);
}

bool BlockModel::gridDuplicateRow(int head, int r) {
    std::vector<GridRow> grid = gridOf(head);
    if (r < 0 || r >= static_cast<int>(grid.size())) return false;
    GridRow copy = grid[size_t(r)];
    copy.copy = true;
    for (auto& cell : copy.cells) cell = copiedCell(cell);
    grid.insert(grid.begin() + r + 1, copy);
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), rows_[size_t(head)].header);
}

bool BlockModel::gridInsertColumn(int head, int at) {
    std::vector<GridRow> grid = gridOf(head);
    if (grid.empty() || at < 0 || tableColumnCount(head) >= 63) return false;
    for (GridRow& gr : grid)
        if (at <= static_cast<int>(gr.cells.size())) gr.cells.insert(gr.cells.begin() + at, GridCell{});
    QJsonArray cols = tableColsOf(rows_[size_t(head)].table);
    while (cols.size() < at) cols.append(QJsonObject());
    cols.insert(at, QJsonObject());
    return commitGrid(head, grid, cols, rows_[size_t(head)].header);
}

bool BlockModel::gridDeleteColumn(int head, int c) {
    std::vector<GridRow> grid = gridOf(head);
    if (grid.empty() || c < 0 || c >= tableColumnCount(head) || tableColumnCount(head) <= 1) return false;
    for (GridRow& gr : grid) {
        if (c < static_cast<int>(gr.cells.size())) gr.cells.erase(gr.cells.begin() + c);
        if (gr.cells.empty()) gr.cells.resize(1);               // a row keeps one (refilled) cell
    }
    QJsonArray cols = tableColsOf(rows_[size_t(head)].table);
    if (c < cols.size()) cols.removeAt(c);
    return commitGrid(head, grid, cols, rows_[size_t(head)].header);
}

bool BlockModel::gridMoveColumn(int head, int from, int to) {
    std::vector<GridRow> grid = gridOf(head);
    const int cols = tableColumnCount(head);
    if (grid.empty() || from < 0 || from >= cols) return false;
    to = std::clamp(to, 0, cols - 1);
    if (to == from) return true;
    const size_t need = size_t(std::max(from, to)) + 1;
    for (GridRow& gr : grid) {
        if (gr.cells.size() <= size_t(from)) continue;          // a ragged row without that cell
        if (gr.cells.size() < need) gr.cells.resize(need);
        GridCell moved = gr.cells[size_t(from)];
        gr.cells.erase(gr.cells.begin() + from);
        gr.cells.insert(gr.cells.begin() + to, moved);
    }
    QJsonArray spec = tableColsOf(rows_[size_t(head)].table);
    while (spec.size() < qsizetype(need)) spec.append(QJsonObject());
    const QJsonValue moved = spec.at(from);
    spec.removeAt(from);
    spec.insert(to, moved);
    return commitGrid(head, grid, spec, rows_[size_t(head)].header);
}

bool BlockModel::gridDuplicateColumn(int head, int c) {
    std::vector<GridRow> grid = gridOf(head);
    if (grid.empty() || c < 0 || c >= tableColumnCount(head) || tableColumnCount(head) >= 63) return false;
    for (GridRow& gr : grid)
        if (c < static_cast<int>(gr.cells.size()))
            gr.cells.insert(gr.cells.begin() + c + 1, copiedCell(gr.cells[size_t(c)]));
    QJsonArray cols = tableColsOf(rows_[size_t(head)].table);
    while (cols.size() <= c) cols.append(QJsonObject());
    cols.insert(c + 1, cols.at(c));
    return commitGrid(head, grid, cols, rows_[size_t(head)].header);
}

bool BlockModel::gridDeleteTable(int head) {
    const auto [lo, hi] = tableBand(head);
    if (lo < 0) return false;
    removeBlocks(lo, hi);
    return true;
}

bool BlockModel::gridClearCells(int head, int r0, int c0, int r1, int c1) {
    std::vector<GridRow> grid = gridOf(head);
    if (grid.empty()) return false;
    if (r0 > r1) std::swap(r0, r1);
    if (c0 > c1) std::swap(c0, c1);
    for (int r = std::max(0, r0); r <= r1 && r < static_cast<int>(grid.size()); ++r)
        for (int c = std::max(0, c0); c <= c1 && c < static_cast<int>(grid[size_t(r)].cells.size()); ++c)
            grid[size_t(r)].cells[size_t(c)].blocks.clear();           // contents only: colours stay
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), rows_[size_t(head)].header);
}

bool BlockModel::gridFillDown(int head, int r0, int c0, int r1, int c1) {
    std::vector<GridRow> grid = gridOf(head);
    const int rows = static_cast<int>(grid.size());
    if (rows == 0) return false;
    r0 = std::clamp(r0, 0, rows - 1); r1 = std::clamp(r1, 0, rows - 1);
    if (r0 > r1) std::swap(r0, r1);
    if (c0 > c1) std::swap(c0, c1);
    for (int c = std::max(0, c0); c <= c1; ++c) {
        const GridCell src = c < static_cast<int>(grid[size_t(r0)].cells.size()) ? grid[size_t(r0)].cells[size_t(c)]
                                                                               : GridCell{};
        for (int r = r0 + 1; r <= r1; ++r) {
            auto& cells = grid[size_t(r)].cells;
            if (static_cast<int>(cells.size()) <= c) cells.resize(size_t(c) + 1);
            cells[size_t(c)] = copiedCell(src);
        }
    }
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), rows_[size_t(head)].header);
}

bool BlockModel::gridFillRight(int head, int r0, int c0, int r1, int c1) {
    std::vector<GridRow> grid = gridOf(head);
    const int rows = static_cast<int>(grid.size());
    if (rows == 0) return false;
    if (r0 > r1) std::swap(r0, r1);
    if (c0 > c1) std::swap(c0, c1);
    c0 = std::max(0, c0);
    for (int r = std::max(0, r0); r <= r1 && r < rows; ++r) {
        auto& cells = grid[size_t(r)].cells;
        const GridCell src = c0 < static_cast<int>(cells.size()) ? cells[size_t(c0)] : GridCell{};
        if (static_cast<int>(cells.size()) <= c1) cells.resize(size_t(c1) + 1);
        for (int c = c0 + 1; c <= c1; ++c) cells[size_t(c)] = copiedCell(src);
    }
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), rows_[size_t(head)].header);
}

// === Colours, alignment, bulk ops, sort, label join (SR-4 S3b) =============
static std::vector<int> gridIndexSet(const QVariantList& list, int bound) {
    std::vector<int> out;
    for (const QVariant& v : list) {
        bool ok = false;
        const int i = v.toInt(&ok);
        if (ok && i >= 0 && i < bound) out.push_back(i);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static void setAttrString(QJsonObject& o, const QString& key, const QString& value) {
    if (value.isEmpty()) o.remove(key);
    else o.insert(key, value);
}

static void setLaneString(QJsonObject& t, const QString& key, int c, const QString& value) {
    QJsonArray a = t.value(key).toArray();
    while (a.size() <= c) a.append(QString());
    a[c] = value;
    while (!a.isEmpty() && a.last().toString().isEmpty()) a.removeLast();
    if (a.isEmpty()) t.remove(key);
    else t.insert(key, a);
}

bool BlockModel::editTableAttrs(int head, const std::function<void(int r, int rec, QJsonObject& t)>& fn) {
    const QVariantList recs = tableRecords(head);
    if (recs.isEmpty()) return false;
    const auto [lo, hi] = tableBand(head);
    beginTxn(lo, hi);
    for (int r = 0; r < recs.size(); ++r) {
        const int rec = recs[r].toInt();
        QJsonObject t = QJsonDocument::fromJson(rows_[size_t(rec)].table.toUtf8()).object();
        const QJsonObject was = t;
        fn(r, rec, t);
        if (t == was) continue;
        rows_[size_t(rec)].table = t.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(t).toJson(QJsonDocument::Compact));
        persistMeta(rec);
    }
    ++contentRevision_;
    emit dataChanged(index(lo), index(hi));
    emit contentChangedSpike();
    endTxn();
    return true;
}

QString BlockModel::gridColour(int head, int r, int c, bool fg) const {
    const int rec = gridRecord(head, r);
    if (rec < 0 || c < 0) return {};
    const QJsonObject t = QJsonDocument::fromJson(rows_[size_t(rec)].table.toUtf8()).object();
    QString v = t.value(fg ? QStringLiteral("cfg") : QStringLiteral("cbg")).toArray().at(c).toString();
    if (v.isEmpty()) v = t.value(fg ? QStringLiteral("fg") : QStringLiteral("bg")).toString();
    if (v.isEmpty())
        v = tableColsOf(rows_[size_t(head)].table).at(c).toObject().value(fg ? QStringLiteral("fg") : QStringLiteral("bg")).toString();
    return v;
}

QString BlockModel::gridCellBg(int head, int r, int c) const { return gridColour(head, r, c, false); }
QString BlockModel::gridCellFg(int head, int r, int c) const { return gridColour(head, r, c, true); }

QString BlockModel::gridRowBg(int head, int r) const {
    const int rec = gridRecord(head, r);
    return rec < 0 ? QString()
                   : QJsonDocument::fromJson(rows_[size_t(rec)].table.toUtf8()).object().value(QStringLiteral("bg")).toString();
}

int BlockModel::gridColAlign(int head, int c) const {
    if (headerCount(head) == 0 || c < 0) return 0;
    return tableColsOf(rows_[size_t(head)].table).at(c).toObject().value(QStringLiteral("a")).toInt(0);
}

bool BlockModel::gridSetCellColor(int head, int r0, int c0, int r1, int c1, bool fg, const QString& color) {
    if (r0 > r1) std::swap(r0, r1);
    if (c0 > c1) std::swap(c0, c1);
    if (c1 < 0 || c0 >= 63) return false;
    return editTableAttrs(head, [&](int r, int, QJsonObject& t) {
        if (r < r0 || r > r1) return;
        for (int c = std::max(0, c0); c <= std::min(c1, 62); ++c)
            setLaneString(t, fg ? QStringLiteral("cfg") : QStringLiteral("cbg"), c, color);
    });
}

bool BlockModel::gridSetRowsColor(int head, const QVariantList& rows, bool fg, const QString& color) {
    const std::vector<int> set = gridIndexSet(rows, gridRowCount(head));
    if (set.empty()) return false;
    return editTableAttrs(head, [&](int r, int, QJsonObject& t) {
        if (std::binary_search(set.begin(), set.end(), r)) setAttrString(t, fg ? QStringLiteral("fg") : QStringLiteral("bg"), color);
    });
}

bool BlockModel::gridSetColsColor(int head, const QVariantList& cols, bool fg, const QString& color) {
    const std::vector<int> set = gridIndexSet(cols, 63);
    if (set.empty()) return false;
    return editTableAttrs(head, [&](int r, int, QJsonObject& t) {
        if (r != 0) return;
        QJsonArray spec = t.value(QStringLiteral("cols")).toArray();
        for (int c : set) {
            while (spec.size() <= c) spec.append(QJsonObject());
            QJsonObject col = spec[c].toObject();
            setAttrString(col, fg ? QStringLiteral("fg") : QStringLiteral("bg"), color);
            spec[c] = col;
        }
        t.insert(QStringLiteral("cols"), spec);
    });
}

bool BlockModel::gridSetColsAlign(int head, const QVariantList& cols, int align) {
    const std::vector<int> set = gridIndexSet(cols, 63);
    if (set.empty()) return false;
    align = std::clamp(align, 0, 2);
    return editTableAttrs(head, [&](int r, int, QJsonObject& t) {
        if (r != 0) return;
        QJsonArray spec = t.value(QStringLiteral("cols")).toArray();
        for (int c : set) {
            while (spec.size() <= c) spec.append(QJsonObject());
            QJsonObject col = spec[c].toObject();
            if (align == 0) col.remove(QStringLiteral("a"));
            else col.insert(QStringLiteral("a"), align);
            spec[c] = col;
        }
        t.insert(QStringLiteral("cols"), spec);
    });
}

bool BlockModel::gridSetRowColor(int head, int r, bool fg, const QString& color) { return gridSetRowsColor(head, { r }, fg, color); }
bool BlockModel::gridSetColColor(int head, int c, bool fg, const QString& color) { return gridSetColsColor(head, { c }, fg, color); }
bool BlockModel::gridSetColAlign(int head, int c, int align) { return gridSetColsAlign(head, { c }, align); }

bool BlockModel::gridDeleteRows(int head, const QVariantList& rows) {
    std::vector<GridRow> grid = gridOf(head);
    const std::vector<int> set = gridIndexSet(rows, static_cast<int>(grid.size()));
    if (set.empty()) return false;
    const int hc = headerCount(head);
    if (set.front() < hc) return gridDeleteTable(head);         // no orphaned table elements
    if (static_cast<int>(grid.size()) - hc <= static_cast<int>(set.size())) return false;
    for (auto it = set.rbegin(); it != set.rend(); ++it) grid.erase(grid.begin() + *it);
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), hc);
}

bool BlockModel::gridDeleteColumns(int head, const QVariantList& cols) {
    std::vector<GridRow> grid = gridOf(head);
    const int count = tableColumnCount(head);
    const std::vector<int> set = gridIndexSet(cols, count);
    if (grid.empty() || set.empty() || static_cast<int>(set.size()) >= count) return false;
    for (GridRow& gr : grid) {
        for (auto it = set.rbegin(); it != set.rend(); ++it)
            if (*it < static_cast<int>(gr.cells.size())) gr.cells.erase(gr.cells.begin() + *it);
        if (gr.cells.empty()) gr.cells.resize(1);
    }
    QJsonArray spec = tableColsOf(rows_[size_t(head)].table);
    for (auto it = set.rbegin(); it != set.rend(); ++it)
        if (*it < spec.size()) spec.removeAt(*it);
    return commitGrid(head, grid, spec, rows_[size_t(head)].header);
}

bool BlockModel::gridClearRows(int head, const QVariantList& rows) {
    std::vector<GridRow> grid = gridOf(head);
    const std::vector<int> set = gridIndexSet(rows, static_cast<int>(grid.size()));
    if (set.empty()) return false;
    for (int r : set)
        for (GridCell& cell : grid[size_t(r)].cells) cell.blocks.clear();
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), rows_[size_t(head)].header);
}

bool BlockModel::gridClearColumns(int head, const QVariantList& cols) {
    std::vector<GridRow> grid = gridOf(head);
    const std::vector<int> set = gridIndexSet(cols, tableColumnCount(head));
    if (grid.empty() || set.empty()) return false;
    for (GridRow& gr : grid)
        for (int c : set)
            if (c < static_cast<int>(gr.cells.size())) gr.cells[size_t(c)].blocks.clear();
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), rows_[size_t(head)].header);
}

bool BlockModel::gridSortByColumn(int head, int c, bool asc) {
    std::vector<GridRow> grid = gridOf(head);
    const int hc = std::min(headerCount(head), static_cast<int>(grid.size()));
    if (c < 0 || static_cast<int>(grid.size()) - hc < 2) return false;
    auto text = [&](const GridRow& gr) {
        if (c >= static_cast<int>(gr.cells.size())) return QString();
        QStringList parts;
        for (int v : gr.cells[size_t(c)].blocks) parts << content_[size_t(v)];
        return parts.join(QLatin1Char('\n'));
    };
    auto less = [](const QString& x, const QString& y) {
        bool okx = false, oky = false;
        const double nx = x.toDouble(&okx), ny = y.toDouble(&oky);
        if (okx && oky) return nx < ny;                          // numeric when both sides parse
        if (okx != oky) return okx;                              // numbers before text
        return QString::compare(x, y, Qt::CaseInsensitive) < 0;
    };
    // Typed columns sort by option order (unset after every option) or by check state.
    const QJsonObject col = tableColsOf(rows_[size_t(head)].table).at(c).toObject();
    const int kind = col.value(QStringLiteral("k")).toInt(0);
    const QJsonArray opts = col.value(QStringLiteral("o")).toArray();
    auto typedKey = [&](const GridRow& gr) {
        const int b = c < static_cast<int>(gr.cells.size()) && !gr.cells[size_t(c)].blocks.empty()
                    ? gr.cells[size_t(c)].blocks.front() : -1;
        const QString v = chipPayloadOf(b).value(QStringLiteral("v")).toString();
        if (kind == 2) return v.toInt();
        for (int i = 0; i < opts.size(); ++i)
            if (opts[i].toObject().value(QStringLiteral("id")).toString() == v) return i;
        return static_cast<int>(opts.size());
    };
    struct Item { QString text; int key; GridRow row; };
    std::vector<Item> body;
    for (size_t i = size_t(hc); i < grid.size(); ++i) body.push_back({ text(grid[i]), kind ? typedKey(grid[i]) : 0, grid[i] });
    std::stable_sort(body.begin(), body.end(), [&](const Item& a, const Item& b) {
        if (kind) return asc ? a.key < b.key : b.key < a.key;
        return asc ? less(a.text, b.text) : less(b.text, a.text);
    });
    bool changed = false;
    for (size_t k = 0; k < body.size(); ++k) changed = changed || body[k].row.rec != grid[size_t(hc) + k].rec;
    if (!changed) return true;
    for (size_t k = 0; k < body.size(); ++k) grid[size_t(hc) + k] = std::move(body[k].row);
    return commitGrid(head, grid, tableColsOf(rows_[size_t(head)].table), rows_[size_t(head)].header);
}

bool BlockModel::joinTableByLabel(int target, int source) {
    const std::vector<GridRow> tg = gridOf(target);
    std::vector<GridRow> sg = gridOf(source);
    if (tg.empty() || sg.empty()) return false;
    auto label = [&](const GridRow& gr, int c) {
        if (c >= static_cast<int>(gr.cells.size())) return QString();
        QStringList parts;
        for (int v : gr.cells[size_t(c)].blocks) parts << content_[size_t(v)];
        return parts.join(QLatin1Char('\n')).trimmed().toCaseFolded();
    };
    const int targetCols = tableColumnCount(target), sourceCols = tableColumnCount(source);
    std::vector<char> used(size_t(targetCols), 0);
    std::vector<int> map(size_t(sourceCols), -1);
    for (int s = 0; s < sourceCols; ++s) {                       // labels, duplicates in order
        const QString l = label(sg[0], s);
        if (l.isEmpty()) continue;
        for (int t = 0; t < targetCols; ++t)
            if (!used[size_t(t)] && label(tg[0], t) == l) { map[size_t(s)] = t; used[size_t(t)] = 1; break; }
    }
    for (int s = 0; s < sourceCols; ++s)                         // unlabeled columns: by position
        if (map[size_t(s)] < 0 && label(sg[0], s).isEmpty() && s < targetCols && !used[size_t(s)]) {
            map[size_t(s)] = s;
            used[size_t(s)] = 1;
        }
    QJsonArray cols = tableColsOf(rows_[size_t(target)].table);
    while (cols.size() < targetCols) cols.append(QJsonObject());
    const QJsonArray sourceSpec = tableColsOf(rows_[size_t(source)].table);
    int next = targetCols;
    for (int s = 0; s < sourceCols; ++s)                         // unmatched source columns append
        if (map[size_t(s)] < 0) {
            map[size_t(s)] = next++;
            cols.append(s < sourceSpec.size() ? sourceSpec.at(s) : QJsonValue(QJsonObject()));
        }
    std::vector<GridRow> grid = tg;
    for (const GridRow& gr : sg) {
        GridRow joined;
        joined.rec = gr.rec;
        for (int s = 0; s < static_cast<int>(gr.cells.size()); ++s) {
            const int t = map[size_t(s)];
            if (static_cast<int>(joined.cells.size()) <= t) joined.cells.resize(size_t(t) + 1);
            joined.cells[size_t(t)] = gr.cells[size_t(s)];
        }
        grid.push_back(std::move(joined));
    }
    const int lo = target, hi = tableBand(source).second;
    beginTxn(lo, hi);
    rebuildTable(lo, hi, grid, cols, rows_[size_t(target)].header);
    adaptJoinedChips(target, static_cast<int>(tg.size()));     // chips adapt by label (A9)
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return true;
}

// === Table keys (SR-4 S6a) ==================================================
bool BlockModel::gridRowIsEmpty(int head, int r) const {
    const int rec = gridRecord(head, r);
    if (rec < 0) return false;
    for (int i = rec + 1; i <= splitRowEnd(rec); ++i) {
        const uint8_t t = rows_[size_t(i)].type;
        if (!content_[size_t(i)].isEmpty() || (t != Paragraph && t != Heading && t != Quote)) return false;
    }
    return true;
}

int BlockModel::gridExitRow(int head) {
    const int rows = gridRowCount(head), r = rows - 1;
    if (rows < 1 || r < headerCount(head) || !gridRowIsEmpty(head, r)) return -1;   // header rows never exit
    const auto [lo, hi] = tableBand(head);
    const int rec = gridRecord(head, r);
    beginTxn(lo, hi);
    removeBlocks(rec, splitRowEnd(rec));
    spliceSpecsAt(rec, { BlockSpec{} }, /*allowReuseAnchorAbove=*/false, /*lane=*/-1);   // a paragraph below the table
    endTxn();
    return rec;
}

// === Ways out of split rows (2026-09-14 walk) ===============================
int BlockModel::insertParagraphAbove(int row) {
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return -1;
    row = std::clamp(row, 0, n - 1);
    const int rec = splitRowOf(row);
    const int at = rec >= 0 ? rec : row;
    beginTxn(at, at - 1);
    spliceSpecsAt(at, { BlockSpec{} }, /*allowReuseAnchorAbove=*/false, /*lane=*/-1);
    endTxn();
    return at;
}

int BlockModel::exitLane(int row) {
    const int n = static_cast<int>(rows_.size());
    if (row < 0 || row >= n || rows_[size_t(row)].cell < 0 || tableHeadOf(row) >= 0) return -1;
    if (rows_[size_t(row)].type != Paragraph || !content_[size_t(row)].isEmpty()) return -1;
    const int rec = splitRowOf(row), lane = rows_[size_t(row)].cell;
    if (rec < 0 || laneLast(rec, lane) != row) return -1;
    const bool only = laneFirst(rec, lane) == row;
    const auto band = wholeSplitRows(rec, rec);
    beginTxn(band.first, band.second);
    if (!only) removeBlock(row);                     // the lane keeps its other blocks
    const int below = splitRowEnd(rec) + 1;
    const bool reuse = below < static_cast<int>(rows_.size()) && rows_[size_t(below)].cell < 0
                       && rows_[size_t(below)].type == Paragraph && content_[size_t(below)].isEmpty();
    if (!reuse) spliceSpecsAt(below, { BlockSpec{} }, /*allowReuseAnchorAbove=*/false, /*lane=*/-1);
    endTxn();
    return below;
}

// === Typed columns (SR-4 S4) ================================================
static const QJsonArray& checkOptions() {
    static const QJsonArray opts = [] {
        QJsonArray a;
        const char* defs[3][2] = { { "To do", "#8A8A8A" }, { "Doing", "#0189F1" }, { "Done", "#58A65C" } };
        for (int i = 0; i < 3; ++i)
            a.append(QJsonObject{ { QStringLiteral("id"), QString::number(i) },
                                  { QStringLiteral("l"), QLatin1String(defs[i][0]) },
                                  { QStringLiteral("c"), QLatin1String(defs[i][1]) } });
        return a;
    }();
    return opts;
}

static QString optionIdByLabel(const QJsonArray& opts, const QString& label) {
    const QString want = label.trimmed().toCaseFolded();
    if (want.isEmpty()) return {};
    for (const QJsonValue& v : opts)
        if (v.toObject().value(QStringLiteral("l")).toString().trimmed().toCaseFolded() == want)
            return v.toObject().value(QStringLiteral("id")).toString();
    return {};
}

static bool optionsHave(const QJsonArray& opts, const QString& id) {
    for (const QJsonValue& v : opts)
        if (v.toObject().value(QStringLiteral("id")).toString() == id) return true;
    return false;
}

QJsonObject BlockModel::chipPayloadOf(int block) const {
    if (block < 0 || block >= static_cast<int>(rows_.size())) return {};
    for (const Span& s : rows_[size_t(block)].spans)
        if (s.kind == SpanChoice) return QJsonDocument::fromJson(s.href.toUtf8()).object();
    return {};
}

void BlockModel::writeCellValue(int block, const QJsonArray& options, const QString& id) {
    Row& r = rows_[size_t(block)];
    r.type = Paragraph; r.level = 0; r.taskState = 0; r.depth = 0; r.lang.clear(); r.param = 1;
    QString label;
    std::vector<Span> spans;
    if (!id.isEmpty() && optionsHave(options, id)) {
        const QJsonObject payload{ { QStringLiteral("o"), options }, { QStringLiteral("v"), id } };
        label = mn::inl::sanitizeChoiceLabel(mn::inl::choiceLabelFor(payload, id));
        if (!label.isEmpty()) spans.push_back({ 0, static_cast<int>(label.size()), SpanChoice, mn::inl::encodeChoicePayload(payload) });
    }
    if (content_[size_t(block)] == label && r.spans.size() == spans.size()
        && (spans.empty() || r.spans.front().href == spans.front().href)) return;   // unchanged
    content_[size_t(block)] = label;
    r.spans = std::move(spans);
    persistContent(block);
    persistMeta(block);
    emit dataChanged(index(block), index(block), { ContentRole });
}

void BlockModel::writePlainValue(int block, const QString& text) {
    Row& r = rows_[size_t(block)];
    r.type = Paragraph; r.level = 0; r.taskState = 0; r.depth = 0; r.lang.clear(); r.param = 1;
    r.spans.clear();
    content_[size_t(block)] = text;
    persistContent(block);
    persistMeta(block);
    emit dataChanged(index(block), index(block), { ContentRole });
}

int BlockModel::gridColumnKind(int head, int c) const {
    if (headerCount(head) == 0 || c < 0) return 0;
    return tableColsOf(rows_[size_t(head)].table).at(c).toObject().value(QStringLiteral("k")).toInt(0);
}

QVariantList BlockModel::gridColumnOptions(int head, int c) const {
    const int kind = gridColumnKind(head, c);
    const QJsonArray opts = kind == 2 ? checkOptions()
                          : tableColsOf(rows_[size_t(head)].table).at(c).toObject().value(QStringLiteral("o")).toArray();
    QVariantList out;
    if (kind == 0) return out;
    for (const QJsonValue& v : opts) {
        const QJsonObject o = v.toObject();
        out << QVariantMap{ { QStringLiteral("id"), o.value(QStringLiteral("id")).toString() },
                            { QStringLiteral("label"), o.value(QStringLiteral("l")).toString() },
                            { QStringLiteral("color"), o.value(QStringLiteral("c")).toString() } };
    }
    return out;
}

bool BlockModel::gridSetColumnKind(int head, int c, int kind) {
    if (headerCount(head) == 0 || c < 0 || c >= tableColumnCount(head)) return false;
    kind = std::clamp(kind, 0, 2);
    QJsonArray spec = tableColsOf(rows_[size_t(head)].table);
    while (spec.size() <= c) spec.append(QJsonObject());
    QJsonObject col = spec[c].toObject();
    if (col.value(QStringLiteral("k")).toInt(0) == kind) return true;
    std::vector<GridRow> grid = gridOf(head);
    const int hc = std::min(headerCount(head), static_cast<int>(grid.size()));
    auto valueOf = [&](const GridCell& cell) {
        if (cell.blocks.empty()) return QString();
        QStringList parts;
        for (int b : cell.blocks) parts << content_[size_t(b)];
        return parts.join(QLatin1Char(' ')).simplified();
    };
    QJsonArray options;
    if (kind == 1) {                                        // T1: harvest the distinct values
        static const char* palette[] = { "#8A8A8A", "#0189F1", "#58A65C", "#E5A33B",
                                         "#D9534F", "#9B6BD6", "#2BB3A6", "#C6689D" };
        for (size_t r = size_t(hc); r < grid.size(); ++r) {
            if (c >= static_cast<int>(grid[r].cells.size())) continue;
            const QString raw = valueOf(grid[r].cells[size_t(c)]);
            if (raw.isEmpty()) continue;                     // an empty cell is no value (sanitize would name it "Option")
            const QString v = mn::inl::sanitizeChoiceLabel(raw);
            if (v.isEmpty() || !optionIdByLabel(options, v).isEmpty()) continue;
            options.append(QJsonObject{ { QStringLiteral("id"), makeUlid() }, { QStringLiteral("l"), v },
                                        { QStringLiteral("c"), QLatin1String(palette[options.size() % 8]) } });
        }
    }
    const QJsonArray& valueOptions = kind == 2 ? checkOptions() : options;
    if (kind == 0) { col.remove(QStringLiteral("k")); col.remove(QStringLiteral("o")); }
    else { col.insert(QStringLiteral("k"), kind); if (kind == 1) col.insert(QStringLiteral("o"), options); else col.remove(QStringLiteral("o")); }
    spec[c] = col;
    const auto [lo, hi] = tableBand(head);
    beginTxn(lo, hi);
    for (size_t r = size_t(hc); r < grid.size(); ++r) {
        if (c >= static_cast<int>(grid[r].cells.size()) || grid[r].cells[size_t(c)].blocks.empty()) continue;
        GridCell& cell = grid[r].cells[size_t(c)];
        const QString v = valueOf(cell);
        const int keep = cell.blocks.front();               // the value block; the rest go (one value per cell)
        if (kind == 0) writePlainValue(keep, v);
        else writeCellValue(keep, valueOptions, optionIdByLabel(valueOptions, v));
        if (kind == 2 && optionIdByLabel(valueOptions, v) == QStringLiteral("0")) writeCellValue(keep, valueOptions, QString());
        cell.blocks = { keep };
    }
    rebuildTable(lo, hi, grid, spec, headerCount(head));
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return true;
}

bool BlockModel::gridSetColumnsKind(int head, const QVariantList& cols, int kind) {
    const std::vector<int> set = gridIndexSet(cols, tableColumnCount(head));
    if (set.empty()) return false;
    const auto [lo, hi] = tableBand(head);
    beginTxn(lo, hi);                                        // one undo step for the set
    bool ok = true;
    for (int c : set) ok = gridSetColumnKind(head, c, kind) && ok;
    endTxn();
    return ok;
}

bool BlockModel::sweepColumnOptions(int head, int c, const QJsonArray& options,
                                    const std::function<QString(const QString&)>& remap) {
    if (gridColumnKind(head, c) != 1) return false;
    QJsonArray spec = tableColsOf(rows_[size_t(head)].table);
    QJsonObject col = spec[c].toObject();
    col.insert(QStringLiteral("o"), options);
    spec[c] = col;
    const auto [lo, hi] = tableBand(head);
    beginTxn(lo, hi);
    QJsonObject t = QJsonDocument::fromJson(rows_[size_t(head)].table.toUtf8()).object();
    t.insert(QStringLiteral("cols"), spec);
    rows_[size_t(head)].table = QString::fromUtf8(QJsonDocument(t).toJson(QJsonDocument::Compact));
    persistMeta(head);
    const int hc = headerCount(head), rows = gridRowCount(head);
    for (int r = hc; r < rows; ++r) {
        const int b = gridCellAt(head, r, c);
        const QJsonObject p = chipPayloadOf(b);
        if (b < 0 || p.isEmpty()) continue;
        writeCellValue(b, options, remap(p.value(QStringLiteral("v")).toString()));
    }
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return true;
}

static QJsonArray columnOptionsOf(const QString& table, int c) {
    return tableColsOf(table).at(c).toObject().value(QStringLiteral("o")).toArray();
}

QString BlockModel::gridAddOption(int head, int c, const QString& label, const QString& color) {
    if (gridColumnKind(head, c) != 1) return {};
    const QString clean = mn::inl::sanitizeChoiceLabel(label);
    if (clean.isEmpty()) return {};
    QJsonArray opts = columnOptionsOf(rows_[size_t(head)].table, c);
    const QString id = makeUlid();
    opts.append(QJsonObject{ { QStringLiteral("id"), id }, { QStringLiteral("l"), clean }, { QStringLiteral("c"), color } });
    return sweepColumnOptions(head, c, opts, [](const QString& v) { return v; }) ? id : QString();
}

bool BlockModel::gridRenameOption(int head, int c, const QString& id, const QString& label) {
    const QString clean = mn::inl::sanitizeChoiceLabel(label);
    QJsonArray opts = columnOptionsOf(rows_[size_t(head)].table, c);
    if (clean.isEmpty() || !optionsHave(opts, id)) return false;
    for (qsizetype i = 0; i < opts.size(); ++i) {
        QJsonObject o = opts[i].toObject();
        if (o.value(QStringLiteral("id")).toString() == id) { o.insert(QStringLiteral("l"), clean); opts[i] = o; }
    }
    return sweepColumnOptions(head, c, opts, [](const QString& v) { return v; });
}

bool BlockModel::gridRecolorOption(int head, int c, const QString& id, const QString& color) {
    QJsonArray opts = columnOptionsOf(rows_[size_t(head)].table, c);
    if (!optionsHave(opts, id)) return false;
    for (qsizetype i = 0; i < opts.size(); ++i) {
        QJsonObject o = opts[i].toObject();
        if (o.value(QStringLiteral("id")).toString() == id) { o.insert(QStringLiteral("c"), color); opts[i] = o; }
    }
    return sweepColumnOptions(head, c, opts, [](const QString& v) { return v; });
}

bool BlockModel::gridMoveOption(int head, int c, const QString& id, int toIndex) {
    QJsonArray opts = columnOptionsOf(rows_[size_t(head)].table, c);
    qsizetype from = -1;
    for (qsizetype i = 0; i < opts.size(); ++i)
        if (opts[i].toObject().value(QStringLiteral("id")).toString() == id) from = i;
    if (from < 0) return false;
    const qsizetype to = std::clamp<qsizetype>(toIndex, 0, opts.size() - 1);
    if (to == from) return true;
    const QJsonValue moved = opts.at(from);
    opts.removeAt(from);
    opts.insert(to, moved);
    return sweepColumnOptions(head, c, opts, [](const QString& v) { return v; });
}

bool BlockModel::gridRemoveOption(int head, int c, const QString& id) {
    QJsonArray opts = columnOptionsOf(rows_[size_t(head)].table, c);
    if (!optionsHave(opts, id)) return false;
    for (qsizetype i = opts.size() - 1; i >= 0; --i)
        if (opts[i].toObject().value(QStringLiteral("id")).toString() == id) opts.removeAt(i);
    return sweepColumnOptions(head, c, opts, [&](const QString& v) { return v == id ? QString() : v; });
}

bool BlockModel::gridSetColumnOptions(int head, int c, const QVariantList& options) {
    if (gridColumnKind(head, c) != 1) return false;
    QJsonArray opts;
    for (const QVariant& v : options) {
        const QVariantMap m = v.toMap();
        const QString label = mn::inl::sanitizeChoiceLabel(m.value(QStringLiteral("label")).toString());
        if (label.isEmpty()) continue;
        QString id = m.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) id = makeUlid();
        opts.append(QJsonObject{ { QStringLiteral("id"), id }, { QStringLiteral("l"), label },
                                 { QStringLiteral("c"), m.value(QStringLiteral("color")).toString() } });
    }
    return sweepColumnOptions(head, c, opts, [&](const QString& v) { return optionsHave(opts, v) ? v : QString(); });
}

QString BlockModel::gridCellChoice(int head, int r, int c) const {
    return chipPayloadOf(gridCellAt(head, r, c)).value(QStringLiteral("v")).toString();
}

QString BlockModel::gridCellChoiceLabel(int head, int r, int c) const {
    const QJsonObject p = chipPayloadOf(gridCellAt(head, r, c));
    return p.isEmpty() ? QString() : mn::inl::choiceLabelFor(p, p.value(QStringLiteral("v")).toString());
}

QString BlockModel::gridCellChoiceColor(int head, int r, int c) const {
    const QJsonObject p = chipPayloadOf(gridCellAt(head, r, c));
    return p.isEmpty() ? QString() : mn::inl::choiceColorFor(p, p.value(QStringLiteral("v")).toString());
}

int BlockModel::ensureGridCell(int head, int r, int c) {
    int b = gridCellAt(head, r, c);
    if (b >= 0) return b;
    std::vector<GridRow> grid = gridOf(head);
    if (r < 0 || r >= static_cast<int>(grid.size()) || c < 0 || c >= 63) return -1;
    if (static_cast<int>(grid[size_t(r)].cells.size()) <= c) grid[size_t(r)].cells.resize(size_t(c) + 1);
    const auto [lo, hi] = tableBand(head);
    rebuildTable(lo, hi, grid, tableColsOf(rows_[size_t(head)].table), headerCount(head));
    return gridCellAt(head, r, c);
}

bool BlockModel::gridSetCellChoice(int head, int r, int c, const QString& id) {
    if (gridColumnKind(head, c) != 1 || r < headerCount(head)) return false;
    const QJsonArray opts = columnOptionsOf(rows_[size_t(head)].table, c);
    if (!id.isEmpty() && !optionsHave(opts, id)) return false;
    const auto [lo, hi] = tableBand(head);
    beginTxn(lo, hi);
    const int b = ensureGridCell(head, r, c);
    if (b >= 0) writeCellValue(b, opts, id);
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return b >= 0;
}

int BlockModel::gridCellCheck(int head, int r, int c) const {
    return std::clamp(chipPayloadOf(gridCellAt(head, r, c)).value(QStringLiteral("v")).toString().toInt(), 0, 2);
}

bool BlockModel::gridSetCellCheck(int head, int r, int c, int state) {
    if (gridColumnKind(head, c) != 2 || r < headerCount(head)) return false;
    state = std::clamp(state, 0, 2);
    const auto [lo, hi] = tableBand(head);
    beginTxn(lo, hi);
    const int b = ensureGridCell(head, r, c);
    if (b >= 0) writeCellValue(b, checkOptions(), state == 0 ? QString() : QString::number(state));
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return b >= 0;
}

bool BlockModel::gridCycleCellCheck(int head, int r, int c) {
    return gridSetCellCheck(head, r, c, (gridCellCheck(head, r, c) + 1) % 3);
}

std::vector<BlockModel::BlockSpec> BlockModel::gridSpecsFromTable(const QString& tableJson) {
    const TableGrid g = TableGrid::fromJson(tableJson);
    std::vector<BlockSpec> out;
    const int rows = g.rows(), cols = std::min(g.cols(), 63);
    if (rows < 1 || cols < 1) return out;
    auto compact = [](const QJsonObject& o) {
        return o.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    };
    QJsonArray spec;
    std::vector<QJsonArray> choiceOptions(static_cast<size_t>(cols));
    for (int c = 0; c < cols; ++c) {
        QJsonObject col;
        if (g.colWidth(c) > 0) col.insert(QStringLiteral("w"), g.colWidth(c));
        if (g.colAlign(c) != 0) col.insert(QStringLiteral("a"), g.colAlign(c));
        if (!g.colBg(c).isEmpty()) col.insert(QStringLiteral("bg"), g.colBg(c));
        if (!g.colFg(c).isEmpty()) col.insert(QStringLiteral("fg"), g.colFg(c));
        if (g.colKind(c) == TableGrid::ColChoice) {
            for (const TableGrid::Option& o : g.colOptions(c))
                choiceOptions[size_t(c)].append(QJsonObject{ { QStringLiteral("id"), o.id }, { QStringLiteral("l"), o.label },
                                                             { QStringLiteral("c"), o.color } });
            col.insert(QStringLiteral("k"), 1);
            col.insert(QStringLiteral("o"), choiceOptions[size_t(c)]);
        } else if (g.colKind(c) == TableGrid::ColCheck) {
            col.insert(QStringLiteral("k"), 2);
        }
        spec.append(col);
    }
    const int hc = std::clamp(g.headerRows(), 1, rows);     // a table always has a header
    const std::vector<float> equal(static_cast<size_t>(cols), 1.0f / static_cast<float>(cols));
    auto chip = [](BlockSpec& p, const QJsonArray& options, const QString& id) {
        const QJsonObject payload{ { QStringLiteral("o"), options }, { QStringLiteral("v"), id } };
        const QString label = mn::inl::sanitizeChoiceLabel(mn::inl::choiceLabelFor(payload, id));
        p.text = label;
        p.spans = { { 0, static_cast<int>(label.size()), SpanChoice, mn::inl::encodeChoicePayload(payload) } };
    };
    for (int r = 0; r < rows; ++r) {
        BlockSpec rec;
        rec.type = Split;
        rec.ratios = equal;
        rec.header = r == 0 ? static_cast<uint8_t>(hc) : uint8_t(0);
        QJsonObject t;
        if (r == 0) t.insert(QStringLiteral("cols"), spec);
        if (!g.rowBg(r).isEmpty()) t.insert(QStringLiteral("bg"), g.rowBg(r));
        if (!g.rowFg(r).isEmpty()) t.insert(QStringLiteral("fg"), g.rowFg(r));
        QJsonArray cbg, cfg;
        for (int c = 0; c < cols; ++c) { cbg.append(g.cellBg(r, c)); cfg.append(g.cellFg(r, c)); }
        while (!cbg.isEmpty() && cbg.last().toString().isEmpty()) cbg.removeLast();
        while (!cfg.isEmpty() && cfg.last().toString().isEmpty()) cfg.removeLast();
        if (!cbg.isEmpty()) t.insert(QStringLiteral("cbg"), cbg);
        if (!cfg.isEmpty()) t.insert(QStringLiteral("cfg"), cfg);
        rec.table = compact(t);
        out.push_back(rec);
        for (int c = 0; c < cols; ++c) {
            const int kind = r < hc ? TableGrid::ColText : g.colKind(c);
            const QString media = g.cellMedia(r, c);
            if (!media.isEmpty()) {
                BlockSpec m;
                m.type = Media;
                m.mediaJson = media;
                m.cell = static_cast<int8_t>(c);
                out.push_back(m);
            }
            BlockSpec p;
            p.cell = static_cast<int8_t>(c);
            if (kind == TableGrid::ColChoice) {
                const QString id = g.cellChoice(r, c);
                if (!id.isEmpty() && !g.optionLabel(c, id).isEmpty()) chip(p, choiceOptions[size_t(c)], id);
            } else if (kind == TableGrid::ColCheck) {
                if (g.cellCheck(r, c) > 0) chip(p, checkOptions(), QString::number(g.cellCheck(r, c)));
            } else {
                p.text = g.cellText(r, c);
                p.spans = cellSpansFromJson(g.cellSpans(r, c));
            }
            if (!media.isEmpty() && p.text.isEmpty()) continue;   // the image fills the cell
            out.push_back(p);
        }
    }
    return out;
}

std::vector<int> BlockModel::expandTableSpecs(std::vector<BlockSpec>& specs) {
    std::vector<int> at(specs.size());
    bool any = false;
    for (const BlockSpec& sp : specs) any = any || (sp.type == Table && sp.mediaJson.isEmpty());
    if (!any) {
        for (size_t k = 0; k < at.size(); ++k) at[k] = static_cast<int>(k);
        return at;
    }
    std::vector<BlockSpec> out;
    for (size_t k = 0; k < specs.size(); ++k) {
        BlockSpec& sp = specs[k];
        if (sp.type != Table || !sp.mediaJson.isEmpty()) {
            at[k] = static_cast<int>(out.size());
            out.push_back(std::move(sp));
            continue;
        }
        std::vector<BlockSpec> grid = gridSpecsFromTable(sp.tableJson);
        at[k] = grid.empty() ? -1 : static_cast<int>(out.size());
        std::move(grid.begin(), grid.end(), std::back_inserter(out));
    }
    specs.swap(out);
    return at;
}

int BlockModel::gridPasteTSV(int head, int r0, int c0, const QString& text) {
    if (headerCount(head) == 0 || r0 < 0 || c0 < 0 || c0 >= 63) return -1;
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    while (t.endsWith(QLatin1Char('\n'))) t.chop(1);
    const TableGrid src = TableGrid::fromTSV(t);
    const int rows = src.rows(), cols = std::min(src.cols(), 63 - c0);
    std::vector<GridRow> grid = gridOf(head);
    if (rows <= 0 || cols <= 0 || grid.empty() || r0 >= static_cast<int>(grid.size())) return -1;
    const size_t widest = size_t(c0 + cols);
    while (grid.size() < size_t(r0 + rows)) {                  // grow rows, copying the last row's divisions
        GridRow fresh;
        fresh.cells.resize(grid.back().cells.size());
        grid.push_back(std::move(fresh));
    }
    for (int i = 0; i < rows; ++i) {                            // grow columns; one value block per target
        auto& cells = grid[size_t(r0 + i)].cells;
        if (cells.size() < widest) cells.resize(widest);
        for (int j = 0; j < cols; ++j)
            if (cells[size_t(c0 + j)].blocks.size() > 1) cells[size_t(c0 + j)].blocks.resize(1);
    }
    QJsonArray spec = tableColsOf(rows_[size_t(head)].table);
    while (spec.size() < qsizetype(widest)) spec.append(QJsonObject());
    const auto [lo, hi] = tableBand(head);
    const int hc = headerCount(head);
    beginTxn(lo, hi);
    rebuildTable(lo, hi, grid, spec, hc);
    int land = -1;
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j) {
            const int r = r0 + i, c = c0 + j;
            if (gridCellAt(head, r, c) < 0) continue;
            const QString v = src.cellText(i, j);
            const int kind = r < hc ? 0 : gridColumnKind(head, c);
            if (kind == 1) {                                    // adopt a matching option, else add one
                QString id = optionIdByLabel(columnOptionsOf(rows_[size_t(head)].table, c), v);
                if (id.isEmpty() && !v.trimmed().isEmpty()) id = gridAddOption(head, c, v, QStringLiteral("#8A8A8A"));
                writeCellValue(gridCellAt(head, r, c), columnOptionsOf(rows_[size_t(head)].table, c), id);
            } else if (kind == 2) {
                const QString id = optionIdByLabel(checkOptions(), v);
                writeCellValue(gridCellAt(head, r, c), checkOptions(), id == QStringLiteral("0") ? QString() : id);
            } else {
                writePlainValue(gridCellAt(head, r, c), v);
            }
            land = gridCellAt(head, r, c);
        }
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return land;
}

int BlockModel::insertGridFromTSV(int afterRow, const QString& tsv) {
    QString t = tsv;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    while (t.endsWith(QLatin1Char('\n'))) t.chop(1);
    if (t.trimmed().isEmpty()) return -1;                     // fromTSV("") is a 1×1 grid, not nothing
    const TableGrid g = TableGrid::fromTSV(t);
    if (g.rows() < 1 || g.cols() < 1) return -1;
    const int n = static_cast<int>(rows_.size());
    int gap = n == 0 ? 0 : std::clamp(afterRow, -1, n - 1) + 1;   // insertTableRows' own gap
    while (gap < n && rows_[size_t(gap)].cell >= 0) ++gap;
    beginTxn(gap, gap - 1);
    const int first = insertTableRows(afterRow, g.rows(), std::min(g.cols(), 63));
    const int head = first - 1;
    gridPasteTSV(head, 0, 0, t);
    endTxn();
    return gridCellAt(head, 0, 0);
}

int BlockModel::gridInsertMedia(int head, int r, int c, const QVariantList& fileUrls) {
    if (headerCount(head) <= 0 || r < 0 || r >= gridRowCount(head) || c < 0 || c >= 63 || fileUrls.isEmpty()) return -1;
    if (r >= headerCount(head) && gridColumnKind(head, c) != 0) return -1;   // a typed body cell holds one chip
    const auto [lo, hi] = tableBand(head);
    beginTxn(lo, hi);
    const bool ragged = gridCellAt(head, r, c) < 0;
    int last = -1;
    if (ensureGridCell(head, r, c) >= 0) {
        for (const QVariant& u : fileUrls) {
            const QVariantList blocks = gridCellRows(head, r, c);
            if (blocks.isEmpty()) break;
            const int nr = insertMediaFromUrl(blocks.back().toInt(), u.toString());   // joins the cell's lane
            if (nr >= 0) last = nr;
        }
    }
    if (ragged && last < 0) { bumpLayout(); ++contentRevision_; emit contentChangedSpike(); }
    endTxn();
    return last;
}

void BlockModel::adaptJoinedChips(int head, int firstJoined) {
    QJsonArray spec = tableColsOf(rows_[size_t(head)].table);
    const int rows = gridRowCount(head), hc = headerCount(head);
    bool specChanged = false;
    for (int c = 0; c < spec.size(); ++c) {
        QJsonObject col = spec[c].toObject();
        const int kind = col.value(QStringLiteral("k")).toInt(0);
        if (kind == 0) continue;
        QJsonArray opts = kind == 2 ? checkOptions() : col.value(QStringLiteral("o")).toArray();
        const int optionsBefore = static_cast<int>(opts.size());
        std::vector<std::pair<int, QString>> writes;
        for (int r = std::max(firstJoined, hc); r < rows; ++r) {
            const int b = gridCellAt(head, r, c);
            const QJsonObject p = chipPayloadOf(b);
            if (b < 0 || p.isEmpty()) continue;
            const QString v = p.value(QStringLiteral("v")).toString();
            const QString label = mn::inl::choiceLabelFor(p, v);
            QString id = optionIdByLabel(opts, label);
            if (id.isEmpty() && kind == 1 && !label.isEmpty()) {
                id = makeUlid();
                opts.append(QJsonObject{ { QStringLiteral("id"), id }, { QStringLiteral("l"), label },
                                         { QStringLiteral("c"), mn::inl::choiceColorFor(p, v) } });
            }
            writes.push_back({ b, id == QStringLiteral("0") && kind == 2 ? QString() : id });
        }
        for (const auto& [b, id] : writes) writeCellValue(b, opts, id);
        if (kind == 1 && opts.size() != optionsBefore) {
            col.insert(QStringLiteral("o"), opts);
            spec[c] = col;
            specChanged = true;
            for (int r = hc; r < std::max(firstJoined, hc); ++r) {    // the table's own chips: the grown set
                const int b = gridCellAt(head, r, c);
                const QJsonObject p = chipPayloadOf(b);
                if (b >= 0 && !p.isEmpty()) writeCellValue(b, opts, p.value(QStringLiteral("v")).toString());
            }
        }
    }
    if (!specChanged) return;
    QJsonObject t = QJsonDocument::fromJson(rows_[size_t(head)].table.toUtf8()).object();
    t.insert(QStringLiteral("cols"), spec);
    rows_[size_t(head)].table = QString::fromUtf8(QJsonDocument(t).toJson(QJsonDocument::Compact));
    persistMeta(head);
}

void BlockModel::applyPermutation(int lo, const std::vector<std::pair<QString, QString>>& order) {
    const int m = static_cast<int>(order.size());
    if (lo < 0 || lo + m > static_cast<int>(rows_.size())) return;
    QHash<QString, int> at;
    for (int i = lo; i < lo + m; ++i) at.insert(ids_[size_t(i)], i);
    const std::vector<double> oldHeights = layout().heights();
    std::vector<Row> nr;
    std::vector<QString> nc;
    std::vector<double> heights = oldHeights;
    for (int k = 0; k < m; ++k) {
        const auto it = at.constFind(order[size_t(k)].first);
        if (it == at.constEnd()) return;                         // never expected: leave the doc alone
        nr.push_back(rows_[size_t(it.value())]);
        nc.push_back(content_[size_t(it.value())]);
        if (size_t(lo + k) < heights.size() && size_t(it.value()) < oldHeights.size())
            heights[size_t(lo + k)] = oldHeights[size_t(it.value())];
    }
    applying_ = true;
    beginResetModel();
    for (int k = 0; k < m; ++k) {
        const size_t i = size_t(lo + k);
        rows_[i] = nr[size_t(k)];
        content_[i] = nc[size_t(k)];
        ids_[i] = order[size_t(k)].first;
        ranks_[i] = order[size_t(k)].second;
        if (doc_.isOpen()) doc_.updateRank(ids_[i], ranks_[i]);
    }
    reindex(std::move(heights));
    endResetModel();
    ++layoutRevision_; ++contentRevision_;
    emit modelReset(); emit layoutChangedSpike(); emit contentChangedSpike();
    applying_ = false;
}

bool BlockModel::setTableColumnWidth(int head, int column, qreal width) {
    if (headerCount(head) == 0 || column < 0 || column >= 63) return false;
    const int w = width <= 0 ? 0 : static_cast<int>(std::lround(std::clamp<qreal>(width, 48.0, 4000.0)));
    QJsonObject t = QJsonDocument::fromJson(rows_[size_t(head)].table.toUtf8()).object();
    QJsonArray cols = t.value(QStringLiteral("cols")).toArray();
    while (cols.size() <= column) cols.append(QJsonObject());
    QJsonObject col = cols[column].toObject();
    if (col.value(QStringLiteral("w")).toInt(0) == w) return true;
    if (w > 0) col.insert(QStringLiteral("w"), w);
    else col.remove(QStringLiteral("w"));
    cols[column] = col;
    t.insert(QStringLiteral("cols"), cols);
    const QVariantList recs = tableRecords(head);
    const int end = splitRowEnd(recs.back().toInt());
    beginTxn(head, head);
    rows_[size_t(head)].table = QString::fromUtf8(QJsonDocument(t).toJson(QJsonDocument::Compact));
    persistMeta(head);
    rederiveMedia(head, end);                          // cell media re-fit their column
    bumpLayout();
    ++contentRevision_;
    emit dataChanged(index(head), index(end));
    emit contentChangedSpike();
    endTxn();
    return true;
}

std::pair<int,int> BlockModel::splitRunBand(int record) const {
    const int n = static_cast<int>(rows_.size());
    int lo = record;
    for (int b = record - 1; b >= 0; --b) {
        if (rows_[size_t(b)].cell >= 0) continue;
        if (rows_[size_t(b)].type != Split) break;
        lo = b;
    }
    int hi = splitRowEnd(record);
    while (hi + 1 < n && rows_[size_t(hi + 1)].type == Split && rows_[size_t(hi + 1)].cell < 0) hi = splitRowEnd(hi + 1);
    return { lo, hi };
}

int BlockModel::tableHeadOf(int row) const {
    return (row >= 0 && row < static_cast<int>(rows_.size())) ? tableHeads()[size_t(row)] : -1;
}

int BlockModel::headerCount(int row) const {
    return (row >= 0 && row < static_cast<int>(rows_.size()) && rows_[size_t(row)].type == Split
            && rows_[size_t(row)].cell < 0) ? rows_[size_t(row)].header : 0;
}

bool BlockModel::isHeaderRow(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    const int rec = rows_[size_t(row)].cell >= 0 ? splitRowOf(row) : row;
    const int head = tableHeadOf(rec);
    if (rec < 0 || head < 0) return false;
    int k = 0;
    for (int r = head; r < rec; r = splitRowEnd(r) + 1) ++k;
    return k < rows_[size_t(head)].header;
}

QVariantList BlockModel::tableRecords(int head) const {
    if (headerCount(head) == 0) return {};
    const int n = static_cast<int>(rows_.size());
    QVariantList out;
    for (int r = head; r < n && rows_[size_t(r)].type == Split && rows_[size_t(r)].cell < 0
                       && (r == head || rows_[size_t(r)].header == 0); r = splitRowEnd(r) + 1)
        out << r;
    return out;
}

int BlockModel::tableColumnCount(int head) const {
    if (headerCount(head) == 0) return 0;
    int cols = static_cast<int>(QJsonDocument::fromJson(rows_[size_t(head)].table.toUtf8())
                                    .object().value(QStringLiteral("cols")).toArray().size());
    for (const QVariant& v : tableRecords(head)) {
        const int rec = v.toInt();
        for (int i = rec + 1; i <= splitRowEnd(rec); ++i) cols = std::max(cols, rows_[size_t(i)].cell + 1);
    }
    return cols;
}

bool BlockModel::setHeaderRole(int record, int count) {
    const int n = static_cast<int>(rows_.size());
    if (record < 0 || record >= n || rows_[size_t(record)].type != Split || rows_[size_t(record)].cell >= 0) return false;
    count = std::clamp(count, 0, 255);
    if (rows_[size_t(record)].header == count) return true;
    // A9: unassigning a head right below another table joins it explicitly — by label.
    if (count == 0) {
        int prev = record - 1;
        while (prev >= 0 && rows_[size_t(prev)].cell >= 0) --prev;
        const int target = prev >= 0 && rows_[size_t(prev)].type == Split ? tableHeadOf(prev) : -1;
        if (target >= 0) return joinTableByLabel(target, record);
    }
    // The tables above and below regroup around it: the band is the whole run of split rows.
    const auto [lo, hi] = splitRunBand(record);
    beginTxn(lo, hi);
    Row& r = rows_[size_t(record)];
    QJsonObject t = QJsonDocument::fromJson(r.table.toUtf8()).object();
    if (count > 0 && !t.contains(QStringLiteral("cols"))) {
        int lanes = 0;
        for (int i = record + 1; i <= splitRowEnd(record); ++i) lanes = std::max(lanes, rows_[size_t(i)].cell + 1);
        QJsonArray cols;
        for (int c = 0; c < lanes; ++c) cols.append(QJsonObject());   // auto-width text columns
        t.insert(QStringLiteral("cols"), cols);
    } else if (count == 0) {
        t.remove(QStringLiteral("cols"));
    }
    r.header = static_cast<uint8_t>(count);
    r.table = t.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(t).toJson(QJsonDocument::Compact));
    tablesDirty_ = true;
    persistMeta(record);
    normalizeStructure(lo, hi);                       // rows leaving a table follow layout rules
    tablesDirty_ = true;
    reindex(std::vector<double>(layout().heights())); // the table pockets move with the role
    bumpLayout();
    ++contentRevision_;
    const int last = std::min(hi, static_cast<int>(rows_.size()) - 1);
    if (lo <= last) emit dataChanged(index(lo), index(last));
    emit contentChangedSpike();
    endTxn();
    return true;
}

int BlockModel::insertTableRows(int afterRow, int nRows, int nCols) {
    const int n = static_cast<int>(rows_.size());
    nRows = std::clamp(nRows, 1, 10000);
    nCols = std::clamp(nCols, 1, 63);
    int gap = n == 0 ? 0 : std::clamp(afterRow, -1, n - 1) + 1;
    while (gap < n && rows_[size_t(gap)].cell >= 0) ++gap;   // never inside a split row
    beginTxn(gap, gap - 1);
    QString prev = gap > 0 ? ranks_[size_t(gap - 1)] : QString();
    const QString next = gap < n ? ranks_[size_t(gap)] : QString();
    const std::vector<float> equal(static_cast<size_t>(nCols), 1.0f / static_cast<float>(nCols));
    QJsonArray cols;
    for (int c = 0; c < nCols; ++c) cols.append(QJsonObject());
    const QString headAttrs = QString::fromUtf8(
        QJsonDocument(QJsonObject{ { QStringLiteral("cols"), cols } }).toJson(QJsonDocument::Compact));
    int at = gap;
    auto put = [&](const Row& r) {
        const QString rk = rankBetween(prev, next);
        insertRowRaw(at++, r, rk);
        prev = rk;
    };
    for (int k = 0; k < nRows; ++k) {
        Row rec{}; rec.type = Split; rec.param = 1; rec.ratios = equal;
        if (k == 0) { rec.header = 1; rec.table = headAttrs; }
        put(rec);
        for (int c = 0; c < nCols; ++c) {
            Row p{}; p.type = Paragraph; p.param = 1; p.cell = static_cast<int8_t>(c);
            put(p);
        }
    }
    tablesDirty_ = true;
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return gap + 1;
}

// Surviving lanes' ratios. runLane = each surviving lane's ORIGINAL lane number,
// in order. An untouched row keeps its exact ratios; a lane that vanished gives
// its width to the nearest surviving lane on its left (else the first); ratios
// that can't be mapped (a revisited lane, a count that doesn't fit) become equal.
static std::vector<float> survivingLaneRatios(const std::vector<float>& old,
                                              const std::vector<int>& runLane, int lanes) {
    const std::vector<float> equal(static_cast<size_t>(lanes), 1.0f / static_cast<float>(lanes));
    int maxLane = -1;
    for (int a = 0; a < lanes; ++a) {
        maxLane = std::max(maxLane, runLane[size_t(a)]);
        for (int b = 0; b < a; ++b)
            if (runLane[size_t(a)] == runLane[size_t(b)]) return equal;
    }
    if (old.empty() || static_cast<int>(old.size()) <= maxLane) return equal;
    double sum = 0.0;
    for (float f : old) { if (!(f > 0.0f)) return equal; sum += f; }
    if (std::abs(sum - 1.0) > 0.01) return equal;
    bool identity = static_cast<int>(old.size()) == lanes;
    for (int a = 0; a < lanes && identity; ++a) identity = runLane[size_t(a)] == a;
    if (identity) return old;
    std::vector<float> out(static_cast<size_t>(lanes));
    for (int a = 0; a < lanes; ++a) out[size_t(a)] = old[size_t(runLane[size_t(a)])];
    for (int o = 0; o < static_cast<int>(old.size()); ++o) {
        if (std::find(runLane.begin(), runLane.begin() + lanes, o) != runLane.begin() + lanes) continue;
        int target = 0;
        for (int a = lanes - 1; a >= 0; --a)
            if (runLane[size_t(a)] < o) { target = a; break; }
        out[size_t(target)] += old[size_t(o)];
    }
    double total = 0.0;
    for (float f : out) total += f;
    for (float& f : out) f = static_cast<float>(f / total);
    return out;
}

// PLAN-SR3 D1 over rows [lo, hi], which must start at a top-level row and end at the
// end of a split row (or a top-level row). A block outside any record is top level;
// a record inside a lane starts its own row; lanes renumber in order of appearance
// (a gap or a revisited lane becomes the next lane); a record with no blocks is
// dropped and a one-lane record unwraps.
BlockModel::StructurePlan BlockModel::planStructure(const std::vector<Row>& rows, std::size_t lo, std::size_t hi) {
    StructurePlan p;
    const size_t m = (hi >= lo && hi < rows.size()) ? hi - lo + 1 : 0;
    p.remove.assign(m, 0);
    p.cell.resize(m);
    p.ratios.resize(m);
    for (size_t k = 0; k < m; ++k) {
        p.cell[k] = -1;
        if (rows[lo + k].type == Split) p.ratios[k] = rows[lo + k].ratios;
    }
    // SR-4: table rows (a head, or adjacent below one) may have a single lane and keep
    // their ratios as they are. Is the window's first record already inside a table?
    bool inTable = false;
    for (size_t b = lo; b-- > 0;) {
        if (rows[b].cell >= 0) continue;
        if (rows[b].type != Split) break;
        if (rows[b].header > 0) { inTable = true; break; }
    }
    for (size_t i = lo; i < lo + m;) {
        if (rows[i].type != Split) { if (rows[i].cell < 0) inTable = false; ++i; continue; }
        size_t j = i + 1;
        while (j < lo + m && rows[j].cell >= 0 && rows[j].type != Split) ++j;
        std::vector<int> runLane;
        int prev = -2;
        for (size_t c = i + 1; c < j; ++c) {
            if (rows[c].cell != prev) { prev = rows[c].cell; runLane.push_back(prev); }
            p.cell[c - lo] = static_cast<int8_t>(std::min<int>(static_cast<int>(runLane.size()) - 1, 63));
        }
        const int lanes = std::min<int>(static_cast<int>(runLane.size()), 64);
        const bool tableRow = inTable || rows[i].header > 0;
        if (lanes < (tableRow ? 1 : 2)) {   // no blocks, or a layout row with one lane: the record goes
            p.remove[i - lo] = 1;
            for (size_t c = i + 1; c < j; ++c) p.cell[c - lo] = -1;
        } else {
            inTable = tableRow;
            p.ratios[i - lo] = tableRow ? rows[i].ratios : survivingLaneRatios(rows[i].ratios, runLane, lanes);
        }
        i = j;
    }
    return p;
}

// Load-time repair: the plan over the whole document, applied without signals.
void BlockModel::repairStructure(QSet<QString>& changedIds, QStringList& removedIds) {
    const size_t n = rows_.size();
    if (n == 0) return;
    const StructurePlan p = planStructure(rows_, 0, n - 1);
    std::vector<Row> nr;
    std::vector<QString> ni, nk, nc;
    nr.reserve(n); ni.reserve(n); nk.reserve(n); nc.reserve(n);
    for (size_t k = 0; k < n; ++k) {
        if (p.remove[k]) { removedIds.push_back(ids_[k]); continue; }
        Row r = rows_[k];
        if (r.cell != p.cell[k] || r.ratios != p.ratios[k]) {
            r.cell = p.cell[k];
            r.ratios = p.ratios[k];
            changedIds.insert(ids_[k]);
        }
        nr.push_back(r); ni.push_back(ids_[k]); nk.push_back(ranks_[k]); nc.push_back(content_[k]);
    }
    rows_.swap(nr); ids_.swap(ni); ranks_.swap(nk); content_.swap(nc);
}

std::pair<int,int> BlockModel::wholeSplitRows(int lo, int hi) const {
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return {lo, hi};
    lo = std::clamp(lo, 0, n - 1);
    hi = std::clamp(hi, lo, n - 1);
    while (lo > 0 && rows_[size_t(lo)].cell >= 0) --lo;
    while (hi + 1 < n && rows_[size_t(hi + 1)].cell >= 0) ++hi;
    return {lo, hi};
}

int BlockModel::splitRowEnd(int record) const {
    int j = record;
    while (j + 1 < static_cast<int>(rows_.size()) && rows_[size_t(j + 1)].cell >= 0) ++j;
    return j;
}

int8_t BlockModel::laneAt(int at) const {
    const int prev = at - 1;
    if (prev < 0 || prev >= static_cast<int>(rows_.size())) return -1;
    if (rows_[size_t(prev)].cell >= 0) return rows_[size_t(prev)].cell;   // after a lane block: its lane
    if (rows_[size_t(prev)].type == Split) return 0;                      // right under a record: lane 0
    return -1;
}

void BlockModel::removeRowRaw(int row) {
    dropBlockInk(ids_[size_t(row)]);   // DB side cascades with the block row (FK)
    if (doc_.isOpen()) doc_.deleteBlock(ids_[size_t(row)]);
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    content_.erase(content_.begin() + row);
    ids_.erase(ids_.begin() + row);
    ranks_.erase(ranks_.begin() + row);
    indexErase(static_cast<size_t>(row));
    endRemoveRows();
}

void BlockModel::normalizeStructure(int lo, int hi) {
    if (rows_.empty()) return;
    std::tie(lo, hi) = wholeSplitRows(lo, std::max(lo, hi));
    hi += refillTableCells(lo, hi);                   // a table cell never empties (A4, SR-4)
    const StructurePlan p = planStructure(rows_, size_t(lo), size_t(hi));
    bool changed = false;
    for (int i = lo; i <= hi; ++i) {
        const size_t k = size_t(i - lo);
        if (p.remove[k]) continue;
        Row& r = rows_[size_t(i)];
        if (r.cell == p.cell[k] && r.ratios == p.ratios[k]) continue;
        r.cell = p.cell[k];
        r.ratios = p.ratios[k];
        persistMeta(i);
        changed = true;
    }
    for (int i = hi; i >= lo; --i)
        if (p.remove[size_t(i - lo)]) { removeRowRaw(i); changed = true; }
    if (!changed) return;
    if (!indexDirty_) reindex(std::vector<double>(layout_.heights()));   // lanes changed in place
    const int last = std::min(hi, static_cast<int>(rows_.size()) - 1);
    rederiveMedia(lo, last);                          // lane widths may have changed
    if (lo <= last) emit dataChanged(index(lo), index(last));
}

int BlockModel::splitIntoColumns(int row, int side, qreal ratio) {
    const int n = static_cast<int>(rows_.size());
    if (row < 0 || row >= n) return -1;
    const Row src = rows_[size_t(row)];
    if (src.type == Split || src.type == Table || tableHeadOf(row) >= 0) return -1;   // tables add columns (SR-4)
    const float keep = static_cast<float>(std::clamp<qreal>(ratio, 0.05, 0.95));
    const bool newRight = side == 0;

    Row fresh{}; fresh.type = Paragraph; fresh.param = 1;
    int freshRow = -1;

    if (src.cell < 0) {
        // [block | new] or [new | block]: a record goes in above the block.
        beginTxn(row, row);
        const QString above = row > 0 ? ranks_[size_t(row - 1)] : QString();
        const QString below = row + 1 < n ? ranks_[size_t(row + 1)] : QString();
        Row rec{}; rec.type = Split; rec.param = 1;
        rec.ratios = newRight ? std::vector<float>{keep, 1.0f - keep} : std::vector<float>{1.0f - keep, keep};
        const QString recRank = rankBetween(above, ranks_[size_t(row)]);
        insertRowRaw(row, rec, recRank);                              // the block is now at row + 1
        rows_[size_t(row + 1)].cell = newRight ? 0 : 1;
        persistMeta(row + 1);
        fresh.cell = newRight ? 1 : 0;
        if (newRight) {
            freshRow = row + 2;
            insertRowRaw(freshRow, fresh, rankBetween(ranks_[size_t(row + 1)], below));
        } else {
            freshRow = row + 1;
            // Between the record and the block (now at row + 1) — never the row after the block:
            // that read past the end on the document's last block and misordered lanes on reload.
            insertRowRaw(freshRow, fresh, rankBetween(recRank, ranks_[size_t(row + 1)]));
        }
        rederiveMedia(row, row + 2);                  // the block now lays out at a lane's width
        emit dataChanged(index(row), index(row + 2));
    } else {
        // A new lane beside the block's lane, splitting that lane's width.
        const int rec = splitRowOf(row);
        const int lane = src.cell;
        if (rec < 0 || rows_[size_t(rec)].ratios.size() >= 63
            || lane >= static_cast<int>(rows_[size_t(rec)].ratios.size())) return -1;
        const auto band = wholeSplitRows(rec, rec);
        beginTxn(band.first, band.second);
        const int newLane = newRight ? lane + 1 : lane;
        int first = row, last = row;
        while (first - 1 > rec && rows_[size_t(first - 1)].cell == lane) --first;
        while (last + 1 <= band.second && rows_[size_t(last + 1)].cell == lane) ++last;
        for (int i = rec + 1; i <= band.second; ++i)
            if (rows_[size_t(i)].cell >= newLane) {
                ++rows_[size_t(i)].cell;
                persistMeta(i);
            }
        std::vector<float>& ratios = rows_[size_t(rec)].ratios;
        const float width = ratios[size_t(lane)];
        ratios[size_t(lane)] = width * keep;
        ratios.insert(ratios.begin() + newLane, width * (1.0f - keep));
        persistMeta(rec);
        fresh.cell = static_cast<int8_t>(newLane);
        if (newRight) {
            freshRow = last + 1;
            insertRowRaw(freshRow, fresh, rankBetween(ranks_[size_t(last)],
                      last + 1 < static_cast<int>(rows_.size()) ? ranks_[size_t(last + 1)] : QString()));
        } else {
            freshRow = first;
            insertRowRaw(freshRow, fresh, rankBetween(ranks_[size_t(first - 1)], ranks_[size_t(first)]));
        }
        rederiveMedia(rec, band.second + 1);          // the split lane narrowed
        emit dataChanged(index(rec), index(band.second + 1));
    }
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return freshRow;
}

// The 1.0 clean break (PLAN-split-rows-interchange R-I1): only files stamped
// with this build's format open. Anything else is refused with a reason —
// never migrated. The working copy is discarded; the original is untouched.
bool BlockModel::acceptOpenedFormat() {
    if (doc_.format() == QLatin1String(Document::kFormat)) return true;
    lastOpenError_ = doc_.schemaVersion() > 0
        ? tr("This document was made with an earlier minNotes and can't be opened.")
        : tr("This file isn't a minNotes document.");
    doc_.close();
    cleanupScratch();
    return false;
}

void BlockModel::seedEmptyDoc() {
    doc_.begin();
    // content is NOT NULL — bind a non-null empty string (a null QString would
    // violate the constraint and silently drop the block, leaving an empty doc).
    doc_.appendBlock(makeUlid(), rankBetween(QString(), QString()), 0,
                     QStringLiteral("paragraph"), QString(), QStringLiteral(""));
    doc_.commit();
}

void BlockModel::newDocument() {
    // Identity path for the untitled doc — the media anchor + Save-As source dir.
    // The bytes live in the working copy (scratchPath_), not at this path.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/untitled-") + makeUlid() + QStringLiteral(".mnd");
    if (!loadDocument(path, /*untitled*/true)) return;
    seedEmptyDoc();
    loadFromStore();
    dirty_ = false;                  // a pristine empty doc isn't dirty (still untitled)
    setSaveState(SaveClean);
    emit documentChanged();
    emit dirtyChanged();
}

bool BlockModel::openDocument(const QString& pathOrUrl) {
    const QString path = pathOrUrl.startsWith(QLatin1String("file:"))
                       ? QUrl(pathOrUrl).toLocalFile() : pathOrUrl;
    if (path.isEmpty() || !loadDocument(path, /*untitled*/false)) return false;
    loadFromStore();
    emit documentChanged();
    return true;
}

// Write the working copy back to the original. checkpoint → VACUUM INTO a sibling
// temp (a clean, compacted, rollback-stamped image — also normalises away any WAL
// stamp) → atomic replace over the original. The working copy keeps the live data
// if any step fails, so nothing is lost.
bool BlockModel::writeBackToOriginal() {
    setSaveState(SaveSaving);
    doc_.stampMeta();     // saved files record schema_version/app_version/modified
    doc_.checkpoint();
    const QString dir = QFileInfo(docPath_).absolutePath();
    QDir().mkpath(dir);
    const QString tmp = dir + QStringLiteral("/.mn-save-") + makeUlid() + QStringLiteral(".tmp");
    QFile::remove(tmp);
    if (!doc_.vacuumInto(tmp)) {
        qWarning() << "BlockModel: save VACUUM INTO failed" << tmp;
        QFile::remove(tmp);
        setSaveState(SaveFailed);
        return false;
    }
    // A legacy WAL-stamped original could have an orphaned -wal/-shm that would
    // shadow our fresh rollback file on the next open — drop them.
    QFile::remove(docPath_ + QStringLiteral("-wal"));
    QFile::remove(docPath_ + QStringLiteral("-shm"));
    if (!atomicReplace(tmp, docPath_)) {
        qWarning() << "BlockModel: save atomic replace failed" << docPath_;
        QFile::remove(tmp);
        setSaveState(SaveFailed);
        return false;
    }
    recordOriginalStat();
    dirty_ = false;
    emit dirtyChanged();
    setSaveState(SaveClean);
    return true;
}

bool BlockModel::save() {
    if (untitled_) return false;                       // no chosen path → caller invokes Save As
    if (externalChangeDetected()) {                    // changed on disk since we opened it
        setSaveState(SaveConflict);
        return false;                                  // caller resolves (Overwrite / Save As copy)
    }
    return writeBackToOriginal();
}

bool BlockModel::overwriteSave() {
    if (untitled_) return false;
    return writeBackToOriginal();                      // bypass the conflict check ("Overwrite")
}

bool BlockModel::saveAs(const QString& pathOrUrl) {
    QString path = pathOrUrl.startsWith(QLatin1String("file:"))
                 ? QUrl(pathOrUrl).toLocalFile() : pathOrUrl;
    if (path.isEmpty()) return false;
    if (!path.endsWith(QLatin1String(".mnd"), Qt::CaseInsensitive)) path += QStringLiteral(".mnd");
    const QString srcMediaDir = mediaAnchorDir();   // package docs: the extraction dir
    const QString dstDir = QFileInfo(path).absolutePath();
    QDir().mkpath(dstDir);
    setSaveState(SaveSaving);
    // Flush the WAL, then write a clean copy to the chosen location (temp + atomic
    // replace, matching the in-place save path).
    doc_.stampMeta();     // saved files record schema_version/app_version/modified
    doc_.checkpoint();
    const QString tmp = dstDir + QStringLiteral("/.mn-save-") + makeUlid() + QStringLiteral(".tmp");
    QFile::remove(tmp);
    if (!doc_.vacuumInto(tmp)) {
        qWarning() << "BlockModel: Save As VACUUM INTO failed" << tmp;
        QFile::remove(tmp);
        setSaveState(SaveFailed);
        return false;
    }
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
    if (!atomicReplace(tmp, path)) {
        QFile::remove(tmp);
        setSaveState(SaveFailed);
        return false;
    }
    // Critical: bring the pasted-media sidecar along to the new location.
    // A lazily-opened package first materializes everything still in the
    // archive (skip-if-exists: files already extracted — possibly edited
    // sidecar notes — win), or the folder copy would silently drop media
    // nothing had touched yet.
    if (mnpkg::isPackagePath(docPath_) && QFileInfo::exists(docPath_))
        mnpkg::extractMatching(docPath_, QStringLiteral("media/"),
                               QStringLiteral("media/"),
                               pkgDir_ + QStringLiteral("/.minnotes"));
    copyMediaSidecar(srcMediaDir, dstDir);
    // Re-home identity + media anchor to the new file; keep editing the SAME
    // working copy (no reload — it already holds the content).
    docPath_ = path;
    untitled_ = false;
    mediaStore_ = std::make_unique<MediaStore>(path);
    recordOriginalStat();
    dirty_ = false;
    setSaveState(SaveClean);
    emit documentChanged();
    emit dirtyChanged();
    return true;
}

bool BlockModel::snapshotTo(const QString& path) {
    if (!doc_.isOpen()) return false;
    doc_.stampMeta();
    doc_.checkpoint();
    QFile::remove(path);
    return doc_.vacuumInto(path);
}

BlockModel::BlockType BlockModel::typeFromString(const QString& s) {
    if (s == QLatin1String("heading"))   return Heading;
    if (s == QLatin1String("code"))      return Code;
    if (s == QLatin1String("media"))     return Media;
    if (s == QLatin1String("quote"))     return Quote;
    if (s == QLatin1String("list_item")) return ListItem;
    if (s == QLatin1String("task_item")) return TaskListItem;
    if (s == QLatin1String("ordered_item")) return OrderedListItem;
    if (s == QLatin1String("divider"))   return Divider;
    if (s == QLatin1String("table"))     return Table;
    if (s == QLatin1String("split"))     return Split;
    return Paragraph;
}

const char* BlockModel::typeToString(uint8_t t) {
    switch (t) {
    case Heading:  return "heading";
    case Code:     return "code";
    case Media:    return "media";
    case Quote:    return "quote";
    case ListItem: return "list_item";
    case TaskListItem: return "task_item";
    case OrderedListItem: return "ordered_item";
    case Divider:  return "divider";
    case Table:    return "table";
    case Split:    return "split";
    default:       return "paragraph";
    }
}

QString BlockModel::rankBetween(const QString& a, const QString& b) {
    const QString& kAlpha = kRankAlpha;
    const int B = kAlpha.size();
    auto val = [&](QChar c) { return kAlpha.indexOf(c); };

    QString r;
    int i = 0;
    bool bInf = b.isEmpty();   // b exhausted → treat as +infinity
    while (true) {
        const int ca = (i < a.size()) ? val(a[i]) : 0;           // a padded with min digit
        const int cb = (bInf || i >= b.size()) ? B : val(b[i]);  // b padded with one-past-max
        if (ca + 1 < cb) {                 // room for a digit strictly between
            r += kAlpha[(ca + cb) / 2];
            return r;
        }
        r += kAlpha[ca];                   // ca == cb, or adjacent (ca+1 == cb)
        if (ca < cb) bInf = true;          // placed a digit below b → b no longer bounds us
        ++i;
    }
}

void BlockModel::persistContent(int row) {
    if (doc_.isOpen() && row >= 0 && row < static_cast<int>(ids_.size()))
        doc_.updateContent(ids_[row], content_[row]);
    // SR-4: a table cell's text can move its column's auto width — and every column right of it.
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[size_t(row)].cell < 0) return;
    const int head = tableHeadOf(row);
    if (head < 0) return;
    const int column = rows_[size_t(row)].cell;
    const qreal before = tableColumnWidth(head, column);
    rows_[size_t(row)].natW = -1;
    tableGeomDirty_ = true;
    if (tableColumnWidth(head, column) == before) return;
    const QVariantList recs = tableRecords(head);
    const int end = splitRowEnd(recs.back().toInt());
    rederiveMedia(head, end);
    bumpLayout();
    emit dataChanged(index(head), index(end));
}

void BlockModel::seedSyntheticStore(int n) {
    // One-time fill of an empty doc so there's something to edit. Evenly-spaced
    // width-4 base-62 ranks (NOT zero-padded — those are all-minimum and leave
    // no room to insert before block 0); rankBetween subdivides for inserts.
    const quint64 span = 62ull * 62 * 62 * 62;
    const quint64 step = std::max<quint64>(1, span / static_cast<quint64>(n + 1));
    doc_.begin();
    for (int i = 0; i < n; ++i) {
        Row r{}; r.type = Paragraph; r.param = 1;
        const QString rank = encode62(static_cast<quint64>(i + 1) * step, 4);
        doc_.appendBlock(makeUlid(), rank, 0, QString::fromLatin1(typeToString(r.type)),
                         QString(), genBase(i, r));
    }
    doc_.commit();
}

void BlockModel::loadFromStore() {
    beginResetModel();
    rows_.clear();
    ids_.clear();
    ranks_.clear();
    content_.clear();

    const std::vector<Document::BlockMeta> metas = doc_.skinnyScan();
    rows_.reserve(metas.size());
    ids_.reserve(metas.size());
    ranks_.reserve(metas.size());
    content_.reserve(metas.size());
    std::vector<double> heights;
    heights.reserve(metas.size());
    QSet<QString> canonicalized;      // ids whose markdown markers were consumed

    for (const Document::BlockMeta& m : metas) {
        QString text = doc_.contentFor(m.id);   // Phase 1a: load eagerly
        Row r{};
        r.type = typeFromString(m.type);
        r.param = static_cast<uint16_t>(std::max<int>(1, text.count(QLatin1Char('\n')) + 1));
        const QJsonObject o = QJsonDocument::fromJson(m.attrs.toUtf8()).object();
        if (r.type == Heading)
            r.level = static_cast<uint8_t>(std::clamp(o.value(QStringLiteral("level")).toInt(1), 1, 6));
        if (r.type == TaskListItem)
            r.taskState = static_cast<uint8_t>(std::clamp(o.value(QStringLiteral("state")).toInt(0), 0, int(TaskStateCount) - 1));
        if (isListType(r.type))
            r.depth = static_cast<uint8_t>(std::clamp(m.depth, 0, kMaxListDepth));
        if (r.type == Code)
            r.lang = o.value(QStringLiteral("lang")).toString();
        // Split rows (SR-3): a lane block records its lane, a record its ratios.
        // Structure is validated as a whole after the scan (repairStructure).
        if (const QJsonValue cv = o.value(QStringLiteral("cell")); cv.isDouble())
            r.cell = static_cast<int8_t>(std::clamp(cv.toInt(), 0, 63));
        if (r.type == Split) {
            for (const QJsonValue& rv : o.value(QStringLiteral("ratios")).toArray())
                r.ratios.push_back(static_cast<float>(rv.toDouble()));
            // SR-4: a head's header count and a record's table attrs.
            r.header = static_cast<uint8_t>(std::clamp(o.value(QStringLiteral("header")).toInt(0), 0, 255));
            if (const QJsonObject to = o.value(QStringLiteral("table")).toObject(); !to.isEmpty())
                r.table = QString::fromUtf8(QJsonDocument(to).toJson(QJsonDocument::Compact));
        }
        if (r.type == Table)   // content is the grid JSON; param = row count for height estimate
            r.param = static_cast<uint16_t>(std::max(1, TableGrid::fromJson(text).rows()));
        fillMediaMeta(r, text);   // media: dims/video/aspect-param from the descriptor JSON
        for (const QJsonValue& sv : o.value(QStringLiteral("spans")).toArray()) {
            const QJsonObject so = sv.toObject();
            const uint8_t k = spanKindFromString(so.value(QStringLiteral("k")).toString());
            const int s = so.value(QStringLiteral("s")).toInt(), e = so.value(QStringLiteral("e")).toInt();
            if (k && e > s) r.spans.push_back({s, e, k, so.value(QStringLiteral("u")).toString()});
        }
        // Markdown is an input method, not storage: consume any inline markers
        // into spans on load so non-active blocks render clean (markers only ever
        // appear while you're typing them). Converted rows are persisted back to
        // the working copy below, so clean-text+spans is the ONE on-disk text
        // format — not "markers until the block happens to be edited", which
        // left two formats per document and pinned every future reader (export,
        // tooling) to this exact conversion pass. Heading/Code stay literal by
        // design.
        if (r.type == Paragraph || r.type == Quote || r.type == ListItem || r.type == TaskListItem) {
            QString clean; std::vector<Span> spans;
            if (convertMarkdown(text, r.spans, clean, spans)) {
                text = clean; r.spans = spans;
                canonicalized.insert(m.id);
            }
        }
        // Defensive: drop/clamp spans that exceed the content (stale data must
        // never push the highlighter / positionToRectangle out of range).
        {
            const int len = text.size();
            std::vector<Span> ok;
            for (Span sp : r.spans) {
                sp.s = std::clamp(sp.s, 0, len);
                sp.e = std::clamp(sp.e, 0, len);
                if (sp.e > sp.s) ok.push_back(sp);
            }
            r.spans.swap(ok);
        }
        rows_.push_back(r);
        ids_.push_back(m.id);
        ranks_.push_back(m.rank);
        content_.push_back(text);
    }
    // A malformed split row is repaired, never refused — the way the editor's
    // own rules would leave it (PLAN-SR3 D1).
    QSet<QString> restructured;
    QStringList removedRecords;
    repairStructure(restructured, removedRecords);
    {
        const std::vector<double> lw = laneWidths();
        for (size_t i = 0; i < rows_.size(); ++i) heights.push_back(estimatedHeight(rows_[i], lw[i]));
    }
    // Persist the conversions and repairs into the WORKING COPY in one
    // transaction (the original file updates on the next explicit save, as
    // always). This is a normalization, not a user edit: no undo entries (the
    // txn chokepoint is bypassed deliberately), no dirty flag.
    if ((!canonicalized.isEmpty() || !restructured.isEmpty() || !removedRecords.isEmpty())
        && doc_.isOpen()) {
        doc_.begin();
        for (const QString& id : removedRecords) doc_.deleteBlock(id);
        for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
            const bool canon = canonicalized.contains(ids_[row]);
            if (canon) persistContent(row);
            if (canon || restructured.contains(ids_[row])) persistMeta(row);
        }
        doc_.commit();
    }
    // Margin ink: one SELECT for the whole doc. Guard against orphans (an ink
    // row whose block is gone — shouldn't happen with the FK cascade, but a
    // doc edited by a build predating v2 could in principle leave one).
    inkByBlock_ = doc_.isOpen() ? doc_.allInk() : QHash<QString, QString>{};
    if (!inkByBlock_.isEmpty()) {
        const QSet<QString> live(ids_.begin(), ids_.end());
        for (auto it = inkByBlock_.begin(); it != inkByBlock_.end();) {
            if (!live.contains(it.key())) { doc_.deleteInk(it.key()); it = inkByBlock_.erase(it); }
            else ++it;
        }
    }
    ++inkRevision_;
    emit inkChanged();
    reindex(std::move(heights));
    endResetModel();
    ++layoutRevision_;
    clearUndo();
    // The document's page measure (v3; 760 for pre-v3 docs).
    const qreal docW = doc_.pageWidth();
    if (!qFuzzyCompare(pageWidth_, docW)) { pageWidth_ = docW; emit pageWidthChanged(); }
    emit modelReset();
    emit layoutChangedSpike();
}

void BlockModel::rebuild(int n, int distribution) {
    beginResetModel();
    rows_.clear();
    content_.clear();
    ids_.clear();
    ranks_.clear();
    rows_.reserve(static_cast<size_t>(std::max(0, n)));
    content_.reserve(static_cast<size_t>(std::max(0, n)));
    std::vector<double> heights;
    heights.reserve(static_cast<size_t>(std::max(0, n)));

    for (int i = 0; i < n; ++i) {
        const uint32_t h = rowHash(i);
        Row r{};
        if (distribution == Uniform) {
            r.type = Paragraph; r.param = 1;
        } else {
            const uint32_t bucket = h % 100;
            if (distribution == Adversarial && (h % 50) == 0) {
                r.type = (h & 1) ? Code : Paragraph;        // heavy tail
                r.param = static_cast<uint16_t>(200 + (h % 120));
            } else if (bucket < 70) {
                r.type = Paragraph; r.param = static_cast<uint16_t>(1 + (h % 3));
            } else if (bucket < 85) {
                r.type = Heading;   r.param = 0;
            } else if (bucket < 95) {
                r.type = Code;      r.param = static_cast<uint16_t>(5 + (h % 36));
            } else {
                r.type = Media;     r.param = static_cast<uint16_t>(40 + (h % 120)); // aspect h/w *100
            }
        }
        rows_.push_back(r);
        content_.push_back(genBase(i, r));   // generate text ONCE, here — not per scroll frame
        ids_.push_back(makeUlid());
        ranks_.push_back(encode62(static_cast<quint64>(i + 1)
                         * std::max<quint64>(1, (62ull*62*62*62) / static_cast<quint64>(n + 1)), 4));
        heights.push_back(estimatedHeight(r, contentWidth_));
    }
    reindex(std::move(heights));
    endResetModel();
    ++layoutRevision_;
    clearUndo();
    emit modelReset();
    emit layoutChangedSpike();
}

void BlockModel::fillMediaMeta(Row& r, const QString& content) const {
    if (r.type != Media) return;
    const QJsonObject mo = QJsonDocument::fromJson(content.toUtf8()).object();
    const int mw = mo.value(QStringLiteral("w")).toInt();
    const int mh = mo.value(QStringLiteral("h")).toInt();
    r.mediaW = static_cast<uint16_t>(std::clamp(mw, 0, 65535));
    r.mediaH = static_cast<uint16_t>(std::clamp(mh, 0, 65535));
    if (mw > 0 && mh > 0)
        r.param = static_cast<uint16_t>(std::clamp(int(100.0 * mh / mw + 0.5), 1, 1000));
    const QString kind = mo.value(QStringLiteral("kind")).toString();
    r.isVideo  = kind == QLatin1String("video");
    r.isFile   = kind == QLatin1String("file");
    r.isPdf    = kind == QLatin1String("pdf");
    r.isSketch = kind == QLatin1String("sketch");
    r.dispW   = static_cast<uint16_t>(std::clamp(mo.value(QStringLiteral("dw")).toInt(), 0, 65535));
    // A user-widened image joins the width cache (the tables' contract) so the
    // page grows to hold it; ≤-page values are harmless under the max() scan.
    // This is the ONE chokepoint every Row reconstruction funnels through
    // (rule 2a), so load/insert/undo all publish it.
    r.measuredW = r.dispW;
}

// Effective displayed width of a media block: the per-block override HONOURED
// VERBATIM if set (user ruling: the drag may exceed the page — the screen is
// the practical cap; the page scrolls like it does for wide tables/code), else
// the default — PDFs fit the page, raster media is intrinsic-capped (never
// upscaled by default).
double BlockModel::mediaDisplayWidth(const Row& r, double laneW) const {
    if (r.dispW > 0) return r.dispW;
    if (r.isPdf) return laneW;   // fit the page (or the lane)
    // Sketches FILL the page and TRACK page-width changes (user ruling
    // 2026-08-21 — illustrations are page furniture, not fixed-px images).
    // Strokes are normalized, so display scale is exact; dw still overrides.
    if (r.isSketch) return laneW;
    if (r.mediaW > 0) return std::min<double>(laneW, r.mediaW);
    return laneW;
}

// === Lane geometry (SR-3, PLAN-SR3 D3) =====================================
// Lanes share the page measure less a kLaneGap between neighbours.

double BlockModel::laneWidthFrom(const std::vector<float>& ratios, int lane) const {
    if (lane < 0 || lane >= static_cast<int>(ratios.size())) return contentWidth_;
    const double avail = std::max(0.0, contentWidth_ - kLaneGap * double(ratios.size() - 1));
    return avail * double(ratios[size_t(lane)]);
}

double BlockModel::laneLeftFrom(const std::vector<float>& ratios, int lane) const {
    double x = 0.0;
    for (int k = 0; k < lane && k < static_cast<int>(ratios.size()); ++k)
        x += laneWidthFrom(ratios, k) + kLaneGap;
    return x;
}

double BlockModel::laneWidthOfRow(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[size_t(row)].cell < 0) return contentWidth_;
    int i = row;
    while (i > 0 && rows_[size_t(i)].cell >= 0) --i;
    if (const TableGeom* tg = tableGeom(tableHeadOf(i))) return tableLaneWidth(*tg, rows_[size_t(row)].cell);
    return rows_[size_t(i)].type == Split ? laneWidthFrom(rows_[size_t(i)].ratios, rows_[size_t(row)].cell)
                                          : contentWidth_;
}

double BlockModel::laneWidthForInsert(int at, int8_t cell) const {
    if (cell < 0) return contentWidth_;
    int i = std::min(at, static_cast<int>(rows_.size())) - 1;
    while (i >= 0 && rows_[size_t(i)].cell >= 0) --i;
    if (i >= 0)
        if (const TableGeom* tg = tableGeom(tableHeadOf(i))) return tableLaneWidth(*tg, cell);
    return (i >= 0 && rows_[size_t(i)].type == Split) ? laneWidthFrom(rows_[size_t(i)].ratios, cell)
                                                       : contentWidth_;
}

std::vector<double> BlockModel::laneWidths() const {
    std::vector<double> w(rows_.size(), contentWidth_);
    const std::vector<float>* ratios = nullptr;
    const TableGeom* tg = nullptr;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        if (r.cell < 0) {
            ratios = r.type == Split ? &r.ratios : nullptr;
            tg = r.type == Split ? tableGeom(tableHeadOf(static_cast<int>(i))) : nullptr;
            continue;
        }
        if (tg) w[i] = tableLaneWidth(*tg, r.cell);
        else if (ratios) w[i] = laneWidthFrom(*ratios, r.cell);
    }
    return w;
}

void BlockModel::rederiveMedia(int lo, int hi) {
    hi = std::min(hi, static_cast<int>(rows_.size()) - 1);
    for (int i = std::max(0, lo); i <= hi; ++i)
        if (rows_[size_t(i)].type == Media)
            setIndexHeight(size_t(i), estimatedHeight(rows_[size_t(i)], laneWidthOfRow(i)));
}

qreal BlockModel::xForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[size_t(row)].cell < 0) return 0.0;
    int i = row;
    while (i > 0 && rows_[size_t(i)].cell >= 0) --i;
    if (const TableGeom* tg = tableGeom(tableHeadOf(i))) {
        const size_t c = size_t(rows_[size_t(row)].cell);
        return c < tg->x.size() ? tg->x[c] : tg->width;
    }
    return rows_[size_t(i)].type == Split ? laneLeftFrom(rows_[size_t(i)].ratios, rows_[size_t(row)].cell) : 0.0;
}

qreal BlockModel::widthForRow(int row) const { return laneWidthOfRow(row); }

int BlockModel::blockAt(qreal x, qreal y) const {
    const int top = rowForY(y);
    if (top < 0 || top >= static_cast<int>(rows_.size()) || rows_[size_t(top)].type != Split) return top;
    const mn::LayoutIndex& li = layout();
    if (size_t(top) >= li.size() || !li.entry(size_t(top)).split) return top;
    const int lanes = tableHeadOf(top) >= 0 ? li.cellCount(size_t(top))
                                            : std::min(li.cellCount(size_t(top)), static_cast<int>(rows_[size_t(top)].ratios.size()));
    if (lanes < 1) return top;
    const int lane = std::min(laneAtX(top, x), lanes - 1);
    return static_cast<int>(li.blockInCellAt(size_t(top), lane, y - li.y(size_t(top))));
}

int BlockModel::laneAtX(int record, qreal pageX) const {
    if (const TableGeom* tg = tableGeom(tableHeadOf(record))) {   // px columns, past the page too
        const int cols = static_cast<int>(tg->w.size());
        for (int k = 0; k < cols; ++k)
            if (pageX < tg->x[size_t(k)] + tg->w[size_t(k)]) return k;
        return std::max(0, cols - 1);
    }
    const std::vector<float>& ratios = rows_[size_t(record)].ratios;
    const int lanes = static_cast<int>(ratios.size());
    for (int k = 0; k < lanes; ++k)   // the gap belongs half to each neighbour
        if (pageX < laneLeftFrom(ratios, k) + laneWidthFrom(ratios, k) + kLaneGap / 2.0) return k;
    return std::max(0, lanes - 1);
}

int BlockModel::laneFirst(int record, int lane) const {
    for (int i = record + 1; i < static_cast<int>(rows_.size()) && rows_[size_t(i)].cell >= 0; ++i)
        if (rows_[size_t(i)].cell == lane) return i;
    return -1;
}

int BlockModel::laneLast(int record, int lane) const {
    int last = -1;
    for (int i = record + 1; i < static_cast<int>(rows_.size()) && rows_[size_t(i)].cell >= 0; ++i) {
        if (rows_[size_t(i)].cell == lane) last = i;
        else if (last >= 0) break;
    }
    return last;
}

int BlockModel::splitRowLast(int row) const {
    const int rec = splitRowOf(row);
    return rec >= 0 ? splitRowEnd(rec) : -1;
}

int BlockModel::nextLeaf(int row) const {
    int r = row + 1;
    while (r < static_cast<int>(rows_.size()) && rows_[size_t(r)].type == Split) ++r;
    return (row >= -1 && r < static_cast<int>(rows_.size())) ? r : -1;
}

int BlockModel::prevLeaf(int row) const {
    int r = std::min(row, static_cast<int>(rows_.size())) - 1;
    while (r >= 0 && rows_[size_t(r)].type == Split) --r;
    return r;
}

int BlockModel::entryLeaf(int top, qreal pageX, bool fromAbove) const {
    if (top < 0 || top >= static_cast<int>(rows_.size())) return -1;
    if (rows_[size_t(top)].type != Split) return top;
    const int lane = laneAtX(top, pageX);
    return fromAbove ? laneFirst(top, lane) : laneLast(top, lane);
}

int BlockModel::leafBelow(int row, qreal pageX) const {
    const int n = static_cast<int>(rows_.size());
    if (row < 0 || row >= n) return -1;
    const int8_t lane = rows_[size_t(row)].cell;
    if (lane >= 0 && row + 1 < n && rows_[size_t(row + 1)].cell == lane) return row + 1;   // the next block in the lane
    const int rec = splitRowOf(row);
    return entryLeaf((rec >= 0 ? splitRowEnd(rec) : row) + 1, pageX, true);   // the row below, at goal-x
}

int BlockModel::leafAbove(int row, qreal pageX) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return -1;
    const int8_t lane = rows_[size_t(row)].cell;
    if (lane >= 0 && row > 0 && rows_[size_t(row - 1)].cell == lane) return row - 1;    // the previous block in the lane
    const int rec = splitRowOf(row);
    const int before = (rec >= 0 ? rec : row) - 1;
    if (before < 0) return -1;
    return entryLeaf(rows_[size_t(before)].cell >= 0 ? splitRowOf(before) : before, pageX, false);
}

// === Lane gestures and commands (SR-3 S7) ===================================

void BlockModel::insertRowRaw(int at, const Row& r, const QString& rank) {
    const QString id = makeUlid();
    beginInsertRows({}, at, at);
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, QString());
    ids_.insert(ids_.begin() + at, id);
    ranks_.insert(ranks_.begin() + at, rank);
    indexInsert(static_cast<size_t>(at), r.type == Split ? 0.0 : estimatedHeight(r, laneWidthForInsert(at, r.cell)));
    endInsertRows();
    if (doc_.isOpen())
        doc_.appendBlock(id, rank, r.depth, QString::fromLatin1(typeToString(r.type)),
                         attrsJson(r.type, r.level, r.lang, r.spans, r.taskState, r.cell, r.ratios, r.header, r.table), QString());
}

std::vector<float> BlockModel::clampedRatios(std::vector<float> ratios) const {
    const int lanes = static_cast<int>(ratios.size());
    if (lanes == 0) return ratios;
    const std::vector<float> equal(static_cast<size_t>(lanes), 1.0f / static_cast<float>(lanes));
    const double avail = std::max(0.0, contentWidth_ - kLaneGap * double(lanes - 1));
    const double minShare = avail > 0.0 ? kMinLaneWidth / avail : 1.0;
    if (minShare * lanes >= 1.0) return equal;             // too narrow for the minimum: equal lanes
    double sum = 0.0;
    for (float& f : ratios) { if (!(f > 0.0f)) f = 0.0f; sum += f; }
    if (sum <= 0.0) return equal;
    double deficit = 0.0, spare = 0.0;
    for (float& f : ratios) {
        f = static_cast<float>(f / sum);
        if (f < minShare) deficit += minShare - f; else spare += f - minShare;
    }
    if (deficit > 1e-9)                                     // lift narrow lanes; the wide ones pay pro rata
        for (float& f : ratios)
            f = f < minShare ? static_cast<float>(minShare)
                             : static_cast<float>(f - (f - minShare) / spare * deficit);
    return ratios;
}

bool BlockModel::setSplitRatios(int record, const QVariantList& ratios) {
    if (record < 0 || record >= static_cast<int>(rows_.size()) || rows_[size_t(record)].type != Split
        || tableHeadOf(record) >= 0) return false;
    if (ratios.size() !=static_cast<qsizetype>(rows_[size_t(record)].ratios.size())) return false;
    std::vector<float> r;
    for (const QVariant& v : ratios) r.push_back(static_cast<float>(v.toDouble()));
    r = clampedRatios(std::move(r));
    if (r == rows_[size_t(record)].ratios) return true;
    const int end = splitRowEnd(record);
    beginTxn(record, record);
    rows_[size_t(record)].ratios = r;
    persistMeta(record);
    rederiveMedia(record, end);
    emit dataChanged(index(record), index(end));
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return true;
}

qreal BlockModel::dividerX(int record, int divider) const {
    if (record < 0 || record >= static_cast<int>(rows_.size()) || rows_[size_t(record)].type != Split) return 0.0;
    const std::vector<float>& ratios = rows_[size_t(record)].ratios;
    if (divider < 0 || divider + 1 >= static_cast<int>(ratios.size())) return 0.0;
    return laneLeftFrom(ratios, divider) + laneWidthFrom(ratios, divider) + kLaneGap / 2.0;
}

QVariantList BlockModel::dividerChain(int record, int divider) const {
    const int n = static_cast<int>(rows_.size());
    if (record < 0 || record >= n || rows_[size_t(record)].type != Split
        || divider < 0 || divider + 1 >= static_cast<int>(rows_[size_t(record)].ratios.size())
        || tableHeadOf(record) >= 0) return {};
    const double x = dividerX(record, divider);
    auto dividerAt = [&](int rec) {
        const int lanes = static_cast<int>(rows_[size_t(rec)].ratios.size());
        for (int k = 0; k + 1 < lanes; ++k)
            if (std::abs(dividerX(rec, k) - x) <= 1.0) return k;
        return -1;
    };
    QVariantList out{ record, divider };
    for (int above = record - 1; above >= 0;) {                   // the adjacent layout rows above …
        const int rec = rows_[size_t(above)].cell >= 0 ? splitRowOf(above) : -1;
        if (rec < 0 || tableHeadOf(rec) >= 0) break;                // the chain stops at a table
        const int k = dividerAt(rec);
        if (k < 0) break;
        out.prepend(k);
        out.prepend(rec);
        above = rec - 1;
    }
    for (int below = splitRowEnd(record) + 1; below < n && rows_[size_t(below)].type == Split;) {   // … and below
        if (tableHeadOf(below) >= 0) break;
        const int k = dividerAt(below);
        if (k < 0) break;
        out << below << k;
        below = splitRowEnd(below) + 1;
    }
    return out;
}

bool BlockModel::moveDivider(int record, int divider, qreal pageX, bool alone) {
    const QVariantList chain = alone ? QVariantList{ record, divider } : dividerChain(record, divider);
    if (chain.size() < 2 || record < 0 || record >= static_cast<int>(rows_.size())
        || rows_[size_t(record)].type != Split || tableHeadOf(record) >= 0) return false;
    struct Change { int rec; std::vector<float> ratios; };
    std::vector<Change> changes;
    for (qsizetype i = 0; i + 1 < chain.size(); i += 2) {
        const int rec = chain[i].toInt(), k = chain[i + 1].toInt();
        std::vector<float> ratios = rows_[size_t(rec)].ratios;
        if (k < 0 || k + 1 >= static_cast<int>(ratios.size())) continue;
        const double avail = std::max(1.0, contentWidth_ - kLaneGap * double(ratios.size() - 1));
        const double wl = ratios[size_t(k)] * avail, wr = ratios[size_t(k + 1)] * avail;
        const double lo = kMinLaneWidth - wl, hi = wr - kMinLaneWidth;   // neither lane under the minimum
        const double delta = lo <= hi ? std::clamp(pageX - dividerX(rec, k), lo, hi) : 0.0;
        ratios[size_t(k)] = static_cast<float>((wl + delta) / avail);
        ratios[size_t(k + 1)] = static_cast<float>((wr - delta) / avail);
        if (ratios != rows_[size_t(rec)].ratios) changes.push_back({ rec, std::move(ratios) });
    }
    if (changes.empty()) return false;
    const int lo = changes.front().rec, hi = splitRowEnd(changes.back().rec);
    beginTxn(lo, hi);
    for (Change& c : changes) {
        rows_[size_t(c.rec)].ratios = std::move(c.ratios);
        persistMeta(c.rec);
        rederiveMedia(c.rec, splitRowEnd(c.rec));
    }
    emit dataChanged(index(lo), index(hi));
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return true;
}

int BlockModel::wrapRun(int lo, int hi, int side, qreal ratio) {
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return -1;
    lo = std::clamp(lo, 0, n - 1);
    hi = std::clamp(hi, 0, n - 1);
    if (lo > hi) std::swap(lo, hi);
    if (lo == hi) return splitIntoColumns(lo, side, ratio);
    for (int i = lo; i <= hi; ++i)
        if (rows_[size_t(i)].cell >= 0 || rows_[size_t(i)].type == Split || rows_[size_t(i)].type == Table) return -1;
    const float keep = static_cast<float>(std::clamp<qreal>(ratio, 0.05, 0.95));
    const bool newRight = side == 0;
    beginTxn(lo, hi);
    Row rec{}; rec.type = Split; rec.param = 1;
    rec.ratios = clampedRatios(newRight ? std::vector<float>{keep, 1.0f - keep} : std::vector<float>{1.0f - keep, keep});
    const QString recRank = rankBetween(lo > 0 ? ranks_[size_t(lo - 1)] : QString(), ranks_[size_t(lo)]);
    insertRowRaw(lo, rec, recRank);                           // the run is now [lo+1, hi+1]
    for (int i = lo + 1; i <= hi + 1; ++i) {
        rows_[size_t(i)].cell = newRight ? 0 : 1;
        persistMeta(i);
    }
    Row fresh{}; fresh.type = Paragraph; fresh.param = 1; fresh.cell = newRight ? 1 : 0;
    int freshRow;
    if (newRight) {
        freshRow = hi + 2;
        insertRowRaw(freshRow, fresh, rankBetween(ranks_[size_t(hi + 1)],
                     hi + 2 < static_cast<int>(rows_.size()) ? ranks_[size_t(hi + 2)] : QString()));
    } else {
        freshRow = lo + 1;
        insertRowRaw(freshRow, fresh, rankBetween(recRank, ranks_[size_t(lo + 1)]));
    }
    rederiveMedia(lo, hi + 2);
    emit dataChanged(index(lo), index(hi + 2));
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return freshRow;
}

void BlockModel::replaceBand(int lo, int hi, const std::vector<Row>& nr,
                             const std::vector<QString>& ni, const std::vector<QString>& nc) {
    const int n = static_cast<int>(rows_.size());
    const std::vector<double> oldHeights = layout().heights();
    QHash<QString, double> heightById;
    QSet<QString> oldIds;
    for (int i = lo; i <= hi; ++i) {
        oldIds.insert(ids_[size_t(i)]);
        if (size_t(i) < oldHeights.size()) heightById.insert(ids_[size_t(i)], oldHeights[size_t(i)]);
    }
    const QSet<QString> newIds(ni.begin(), ni.end());
    for (int i = lo; i <= hi; ++i)
        if (!newIds.contains(ids_[size_t(i)])) {
            dropBlockInk(ids_[size_t(i)]);
            if (doc_.isOpen()) doc_.deleteBlock(ids_[size_t(i)]);
        }
    const QString nextRank = hi + 1 < n ? ranks_[size_t(hi + 1)] : QString();
    std::vector<QString> nk;
    QString prevRank = lo > 0 ? ranks_[size_t(lo - 1)] : QString();
    for (size_t k = 0; k < nr.size(); ++k) { prevRank = rankBetween(prevRank, nextRank); nk.push_back(prevRank); }

    beginResetModel();
    rows_.erase(rows_.begin() + lo, rows_.begin() + hi + 1);
    ids_.erase(ids_.begin() + lo, ids_.begin() + hi + 1);
    ranks_.erase(ranks_.begin() + lo, ranks_.begin() + hi + 1);
    content_.erase(content_.begin() + lo, content_.begin() + hi + 1);
    rows_.insert(rows_.begin() + lo, nr.begin(), nr.end());
    ids_.insert(ids_.begin() + lo, ni.begin(), ni.end());
    ranks_.insert(ranks_.begin() + lo, nk.begin(), nk.end());
    content_.insert(content_.begin() + lo, nc.begin(), nc.end());
    const std::vector<double> lw = laneWidths();
    std::vector<double> heights;
    heights.reserve(rows_.size());
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        if (i < lo) { heights.push_back(i < static_cast<int>(oldHeights.size()) ? oldHeights[size_t(i)] : 0.0); continue; }
        const int k = i - lo;
        if (k < static_cast<int>(nr.size())) {
            const Row& r = rows_[size_t(i)];
            heights.push_back(r.type == Media ? estimatedHeight(r, lw[size_t(i)])   // media re-derive for its lane
                                              : heightById.value(ids_[size_t(i)], estimatedHeight(r, lw[size_t(i)])));
        } else {
            const int old = i - static_cast<int>(nr.size()) + (hi - lo + 1);
            heights.push_back(old < static_cast<int>(oldHeights.size()) ? oldHeights[size_t(old)] : 0.0);
        }
    }
    reindex(std::move(heights));
    endResetModel();

    if (doc_.isOpen())
        for (size_t k = 0; k < nr.size(); ++k) {
            const int i = lo + static_cast<int>(k);
            const Row& r = rows_[size_t(i)];
            if (oldIds.contains(ni[k])) {
                doc_.updateRank(ni[k], nk[k]);
                persistMeta(i);
            } else {
                doc_.appendBlock(ni[k], nk[k], r.depth, QString::fromLatin1(typeToString(r.type)),
                                 attrsJson(r.type, r.level, r.lang, r.spans, r.taskState, r.cell, r.ratios, r.header, r.table), nc[k]);
            }
        }
    ++layoutRevision_;
    ++contentRevision_;
    emit modelReset();
    emit layoutChangedSpike();
    emit contentChangedSpike();
}

int BlockModel::alignLanes(int record) {
    if (record < 0 || record >= static_cast<int>(rows_.size()) || rows_[size_t(record)].type != Split
        || tableHeadOf(record) >= 0) return -1;
    const int end = splitRowEnd(record);
    const int lanes = static_cast<int>(rows_[size_t(record)].ratios.size());
    std::vector<std::vector<int>> laneRows(static_cast<size_t>(lanes));
    for (int i = record + 1; i <= end; ++i)
        if (rows_[size_t(i)].cell >= 0 && rows_[size_t(i)].cell < lanes) laneRows[size_t(rows_[size_t(i)].cell)].push_back(i);
    size_t pairs = 0;
    for (const auto& l : laneRows) pairs = std::max(pairs, l.size());
    if (pairs <= 1) return record;                            // already one block per lane
    std::vector<Row> nr;
    std::vector<QString> ni, nc;
    for (size_t j = 0; j < pairs; ++j) {
        Row rec = rows_[size_t(record)];
        nr.push_back(rec);
        ni.push_back(j == 0 ? ids_[size_t(record)] : makeUlid());
        nc.push_back(QString());
        for (int c = 0; c < lanes; ++c) {
            if (j < laneRows[size_t(c)].size()) {
                const int i = laneRows[size_t(c)][j];
                nr.push_back(rows_[size_t(i)]);
                ni.push_back(ids_[size_t(i)]);
                nc.push_back(content_[size_t(i)]);
            } else {                                          // a shorter lane: an empty paragraph keeps the pair
                Row p{}; p.type = Paragraph; p.param = 1; p.cell = static_cast<int8_t>(c);
                nr.push_back(p);
                ni.push_back(makeUlid());
                nc.push_back(QString());
            }
        }
    }
    beginTxn(record, end);
    replaceBand(record, end, nr, ni, nc);
    endTxn();
    return record;
}

int BlockModel::mergeRowsIntoLanes(int loRow, int hiRow) {
    const int n = static_cast<int>(rows_.size());
    if (loRow < 0 || hiRow < 0 || loRow >= n || hiRow >= n) return -1;
    if (loRow > hiRow) std::swap(loRow, hiRow);
    const int first = splitRowOf(loRow), lastRec = splitRowOf(hiRow);
    if (first < 0 || lastRec <= first) return -1;
    const int lanes = static_cast<int>(rows_[size_t(first)].ratios.size());
    std::vector<int> recs;
    for (int r = first; r <= lastRec; r = splitRowEnd(r) + 1) {   // adjacent split rows only
        if (r >= n || rows_[size_t(r)].type != Split || static_cast<int>(rows_[size_t(r)].ratios.size()) != lanes
            || tableHeadOf(r) >= 0) return -1;
        recs.push_back(r);
    }
    if (recs.back() != lastRec) return -1;
    const int end = splitRowEnd(lastRec);
    std::vector<Row> nr{ rows_[size_t(first)] };
    std::vector<QString> ni{ ids_[size_t(first)] }, nc{ QString() };
    for (int c = 0; c < lanes; ++c)
        for (int rec : recs)
            for (int i = rec + 1; i <= splitRowEnd(rec); ++i)
                if (rows_[size_t(i)].cell == c) {
                    nr.push_back(rows_[size_t(i)]);
                    ni.push_back(ids_[size_t(i)]);
                    nc.push_back(content_[size_t(i)]);
                }
    beginTxn(first, end);
    replaceBand(first, end, nr, ni, nc);
    endTxn();
    return first;
}

QVariantList BlockModel::collapseEmptyLane(int row, bool forward) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
    const Row& r = rows_[size_t(row)];
    if (r.cell < 0 || r.type != Paragraph || !content_[size_t(row)].isEmpty()) return {};
    const int rec = splitRowOf(row);
    const int lane = r.cell;
    if (rec < 0 || laneFirst(rec, lane) != row || laneLast(rec, lane) != row) return {};
    if (tableHeadOf(rec) >= 0) return {};              // a table cell refills: net no-op (A4)
    const int lanes = static_cast<int>(rows_[size_t(rec)].ratios.size());
    const int prevEnd = lane > 0 ? laneLast(rec, lane - 1) : -1;
    const int nextStart = lane + 1 < lanes ? laneFirst(rec, lane + 1) : -1;
    const int target = forward ? (nextStart >= 0 ? nextStart : prevEnd) : (prevEnd >= 0 ? prevEnd : nextStart);
    const bool atEnd = target == prevEnd;
    const QString targetId = target >= 0 ? ids_[size_t(target)] : QString();
    removeBlock(row);                                    // A4 inside the same undo step
    for (int i = 0; i < static_cast<int>(ids_.size()); ++i)   // rows shifted; find the target by identity
        if (ids_[size_t(i)] == targetId)
            return { i, atEnd ? static_cast<int>(content_[size_t(i)].size()) : 0 };
    const int land = std::clamp(row - 1, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
    return { land, 0 };
}

QVariantList BlockModel::clearLanes(int record, int laneLo, int laneHi) {
    if (laneLo > laneHi) std::swap(laneLo, laneHi);
    beginTxn(record, splitRowEnd(record));
    for (int lane = laneHi; lane >= laneLo; --lane) {
        const int first = laneFirst(record, lane), last = laneLast(record, lane);
        if (first < 0) continue;
        for (int i = last; i > first; --i) removeRowRaw(i);   // the lane keeps one block …
        Row& r = rows_[size_t(first)];
        if (r.type == Paragraph && content_[size_t(first)].isEmpty() && r.spans.empty()) continue;
        Row fresh{};                                          // … an empty paragraph, in its lane
        fresh.type = Paragraph;
        fresh.param = 1;
        fresh.cell = r.cell;
        r = fresh;
        content_[size_t(first)].clear();
        dropBlockInk(ids_[size_t(first)]);
        persistContent(first);
        persistMeta(first);
        setIndexHeight(size_t(first), estimatedHeight(r, laneWidthOfRow(first)));
    }
    emit dataChanged(index(record), index(splitRowEnd(record)));
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return { laneFirst(record, laneLo), 0 };
}

QVariantList BlockModel::deleteRowRange(int loRow, int loCol, int hiRow, int hiCol) {
    const int recLo = splitRowOf(loRow), recHi = splitRowOf(hiRow);
    const int wlo = recLo >= 0 ? recLo : loRow;
    const int whi = recHi >= 0 ? splitRowEnd(recHi) : hiRow;
    const bool loText = recLo < 0 && !isOpaqueRow(loRow);
    beginTxn(wlo, whi);
    // The high end first, so the low end's row number holds while we work.
    int removeHi = whi;
    if (recHi < 0 && !isOpaqueRow(hiRow)) {       // a top-level text end keeps its tail
        deleteRange(hiRow, 0, hiRow, hiCol);
        removeHi = hiRow - 1;
    }
    const int removeLo = loText ? loRow + 1 : wlo;   // a top-level text end keeps its head
    if (removeLo <= removeHi) removeBlocks(removeLo, removeHi);
    if (loText) deleteRange(loRow, loCol, loRow, static_cast<int>(content_[size_t(loRow)].size()));
    endTxn();
    int land = std::min(loText ? loRow : wlo, static_cast<int>(rows_.size()) - 1);
    if (land >= 0 && rows_[size_t(land)].type == Split) land = nextLeaf(land - 1);
    return { std::max(0, land), loText ? loCol : 0 };
}

int BlockModel::insertParagraphBelow(int row) {
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return -1;
    row = std::clamp(row, 0, n - 1);
    const int last = splitRowOf(row) >= 0 ? splitRowLast(row) : row;
    const int at = last + 1;
    beginTxn(at, at - 1);
    insertParagraphRaw(at);                          // joins the lane above …
    if (rows_[size_t(at)].cell >= 0) {               // … so lift it out to the top level
        rows_[size_t(at)].cell = -1;
        persistMeta(at);
    }
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return at;
}

QVariantList BlockModel::moveTarget(int lo, int hi, int dir) const {
    const int n = static_cast<int>(rows_.size());
    if (n == 0 || dir == 0) return {};
    lo = std::clamp(lo, 0, n - 1);
    hi = std::clamp(hi, 0, n - 1);
    if (lo > hi) std::swap(lo, hi);
    const int recLo = splitRowOf(lo), recHi = splitRowOf(hi);
    const int8_t lane = rows_[size_t(lo)].cell;
    if (recLo >= 0 && recLo == recHi && lane >= 0 && rows_[size_t(hi)].cell == lane) {   // within one lane
        const int count = hi - lo + 1;
        if (dir < 0)
            return (lo > 0 && rows_[size_t(lo - 1)].cell == lane) ? QVariantList{ lo, count, lo - 1, int(lane) }
                                                                  : QVariantList{};
        return (hi + 1 < n && rows_[size_t(hi + 1)].cell == lane) ? QVariantList{ lo, count, lo + 1, int(lane) }
                                                                 : QVariantList{};
    }
    // Among top-level rows. A split-row end must be the whole row (Escape's row selection).
    if (recLo >= 0 && lo != nextLeaf(recLo)) return {};
    if (recHi >= 0 && hi != splitRowEnd(recHi)) return {};
    const int from = recLo >= 0 ? recLo : lo;
    const int last = recHi >= 0 ? splitRowEnd(recHi) : hi;
    const int count = last - from + 1;
    if (dir < 0) {
        if (from == 0) return {};
        const int above = from - 1;                                  // step over a whole split row above
        return { from, count, rows_[size_t(above)].cell >= 0 ? splitRowOf(above) : above, -1 };
    }
    const int next = last + 1;
    if (next >= n) return {};
    const int nextEnd = rows_[size_t(next)].type == Split ? splitRowEnd(next) : next;   // … or below
    return { from, count, nextEnd - count + 1, -1 };
}

int BlockModel::moveBeside(int from, int count, int target, int side) {
    const int n = static_cast<int>(rows_.size());
    if (count < 1 || from < 0 || from + count > n || target < 0 || target >= n) return -1;
    if (target >= from && target < from + count) return -1;
    for (int k = from; k < from + count; ++k)
        if (rows_[size_t(k)].type == Split) return -1;
    const uint8_t tt = rows_[size_t(target)].type;
    if (tt == Split || tt == Table || tableHeadOf(target) >= 0) return -1;   // not offered inside tables
    const QString firstId = ids_[size_t(from)];
    const auto band = wholeSplitRows(std::min(from, target), std::max(from + count - 1, target));
    beginTxn(band.first, band.second);
    int land = -1;
    const int fresh = splitIntoColumns(target, side, 0.5);   // [target | new lane] — the lane the run will fill
    if (fresh >= 0) {
        const QString freshId = ids_[size_t(fresh)];
        const int lane = rows_[size_t(fresh)].cell;
        const int f = rowForId(firstId);
        moveBlocks(f, count, fresh > f ? fresh - count : fresh, lane);   // just above the placeholder …
        removeBlock(rowForId(freshId));                                   // … which then goes
        land = rowForId(firstId);
    }
    endTxn();
    return land;
}

int BlockModel::insertMediaAt(int gap, int lane, const QString& fileUrl) {
    const int n = static_cast<int>(rows_.size());
    gap = std::clamp(gap, 0, n);
    const auto band = n > 0 ? wholeSplitRows(std::max(0, gap - 1), std::max(0, gap - 1)) : std::pair<int, int>{0, -1};
    beginTxn(band.first, band.second);
    const int r = insertMediaFromUrl(gap - 1, fileUrl);          // lands in the lane above the gap …
    if (r >= 0 && rows_[size_t(r)].cell != lane) {
        const int8_t was = rows_[size_t(r)].cell;
        rows_[size_t(r)].cell = static_cast<int8_t>(lane);      // … so move it to the lane it was dropped in
        if (structureValid()) {
            persistMeta(r);
            if (!indexDirty_) reindex(std::vector<double>(layout_.heights()));
            rederiveMedia(r, r);
            emit dataChanged(index(r), index(r));
            bumpLayout();
        } else {
            rows_[size_t(r)].cell = was;                        // not a place that lane can be
        }
    }
    endTxn();
    return r;
}

int BlockModel::insertMediaBeside(int target, int side, const QString& fileUrl) {
    if (target < 0 || target >= static_cast<int>(rows_.size())) return -1;
    const auto band = wholeSplitRows(target, target);
    beginTxn(band.first, band.second);
    int r = -1;
    const int fresh = splitIntoColumns(target, side, 0.5);
    if (fresh >= 0) {
        r = insertMediaFromUrl(fresh, fileUrl);                  // consumes the new lane's empty paragraph
        if (r < 0) removeBlock(fresh);                           // nothing landed (e.g. an import): no lane
    }
    endTxn();
    return r;
}

int BlockModel::tabTarget(int row, bool back) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[size_t(row)].cell < 0) return -1;
    const int rec = splitRowOf(row);
    if (rec < 0) return -1;
    const int lane = rows_[size_t(row)].cell;
    if (const int head = tableHeadOf(rec); head >= 0) {   // SR-4: cells in reading order; past the last, append
        const int n = static_cast<int>(rows_.size());
        const int cells = layout().cellCount(size_t(rec));
        if (back) return lane > 0 ? laneLast(rec, lane - 1) : prevLeaf(laneFirst(rec, 0));
        if (lane + 1 < cells) return laneLast(rec, lane + 1);
        const int next = splitRowEnd(rec) + 1;
        const bool nextRow = next < n && rows_[size_t(next)].type == Split && rows_[size_t(next)].cell < 0
                             && tableHeadOf(next) == head;
        return nextRow ? laneLast(next, 0) : kTabAppendsRow;
    }
    const int lanes = static_cast<int>(rows_[size_t(rec)].ratios.size());
    if (!back)
        return lane + 1 < lanes ? laneLast(rec, lane + 1) : nextLeaf(splitRowEnd(rec));
    return lane > 0 ? laneLast(rec, lane - 1) : prevLeaf(laneFirst(rec, 0));
}

QList<int> BlockModel::visibleBlocks(qreal y0, qreal y1) const {
    QList<int> out;
    for (size_t f : layout().visible(y0, y1)) out.push_back(static_cast<int>(f));
    return out;
}

// Displayed media frame height: dispW = min(contentWidth, w) (never upscaled),
// height = round(dispW * h/w). Pure function of the probed dims + the layout
// width — recomputed when setContentWidth changes (resize). Falls back to the
// aspect param if intrinsic dims are missing.
double BlockModel::mediaFrameHeight(const Row& r, double laneW) const {
    if (r.isFile) return kFileChip;            // fixed-height attachment chip
    const double w = mediaDisplayWidth(r, laneW);
    if (r.mediaW > 0 && r.mediaH > 0)
        return std::floor(w * r.mediaH / r.mediaW + 0.5);
    return laneW * (r.param / 100.0);
}

double BlockModel::estimatedHeight(const Row& r, double laneW) const {
    switch (r.type) {
    case Heading: return kHeading + kPadV;
    case Media:   // 12px vertical pad + the transport toolbar for video. Matches
                  // the Editor cell delegate exactly, and media never measures
                  // back, so this IS the authoritative height (no scroll-in jump).
        return 12.0 + mediaFrameHeight(r, laneW)
             + (r.isVideo ? kVideoBar : r.isPdf ? kPdfNav : 0.0);
    case Divider: return 24.0;
    case Table:   return r.param * 34.0 + 58.0;     // param = row count; + 6 top/20 bottom pad (+row button) + header/strip
    case Code:
    case Paragraph:
    default:      return r.param * kLine + kPadV;  // quote/list ≈ paragraph
    }
}

int BlockModel::clampRow(int row) const {
    if (rows_.empty()) return 0;
    return std::clamp(row, 0, static_cast<int>(rows_.size()) - 1);
}

const BlockModel::Row& BlockModel::rowAt(int row) const {
    static const Row kEmpty{};   // type 0 = Paragraph, no spans — harmless defaults
    if (rows_.empty()) return kEmpty;
    return rows_[std::clamp(row, 0, static_cast<int>(rows_.size()) - 1)];
}

int BlockModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int BlockModel::typeForRow(int row) const {
    if (rows_.empty()) return Paragraph;
    return rowAt(row).type;
}

int BlockModel::levelForRow(int row) const {
    if (rows_.empty()) return 0;
    return rowAt(row).level;
}

int BlockModel::taskStateForRow(int row) const {
    if (rows_.empty()) return 0;
    const Row& r = rowAt(row);
    return (r.type == TaskListItem) ? r.taskState : 0;
}

void BlockModel::toggleTask(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (rows_[row].type != TaskListItem) return;
    beginTxn(row, row);
    rows_[row].taskState = static_cast<uint8_t>((rows_[row].taskState + 1) % TaskStateCount);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole});
    ++contentRevision_;            // glyph + (future) strikethrough re-render
    emit contentChangedSpike();
    endTxn();
}

int BlockModel::depthForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return 0;
    return rows_[row].depth;
}

void BlockModel::indentBlocks(int loRow, int hiRow, int delta) {
    if (rows_.empty() || delta == 0) return;
    int lo = std::clamp(loRow, 0, static_cast<int>(rows_.size()) - 1);
    int hi = std::clamp(hiRow, 0, static_cast<int>(rows_.size()) - 1);
    if (lo > hi) std::swap(lo, hi);
    bool any = false;              // a range with no movable list rows is a no-op
    for (int i = lo; i <= hi; ++i) {
        if (!isListType(rows_[i].type)) continue;
        const int d = std::clamp(rows_[i].depth + delta, 0, kMaxListDepth);
        if (d != rows_[i].depth) { any = true; break; }
    }
    if (!any) return;
    beginTxn(lo, hi);
    for (int i = lo; i <= hi; ++i) {
        if (!isListType(rows_[i].type)) continue;
        const uint8_t d = static_cast<uint8_t>(std::clamp(rows_[i].depth + delta, 0, kMaxListDepth));
        if (d == rows_[i].depth) continue;
        rows_[i].depth = d;
        persistMeta(i);
    }
    emit dataChanged(index(lo), index(hi), {TypeRole});
    bumpLayout();                  // indent narrows the text column → re-wrap/re-measure
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

// --- Block-pinned margin ink (tier 2 annotations) -----------------------
QString BlockModel::inkForBlock(const QString& blockId) const {
    return inkByBlock_.value(blockId);
}

QString BlockModel::inkForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(ids_.size())) return {};
    return inkByBlock_.value(ids_[row]);
}

QStringList BlockModel::inkBlockIds() const {
    return QStringList(inkByBlock_.keyBegin(), inkByBlock_.keyEnd());
}

void BlockModel::setBlockInk(int row, const QString& inkJson) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (!doc_.isOpen()) return;
    const QString& id = ids_[row];
    if (inkByBlock_.value(id) == inkJson) return;   // no-op (also skips undo entry)
    beginTxn(row, row);   // one undo step per stroke; gestures group callers
    if (inkJson.isEmpty()) {
        inkByBlock_.remove(id);
        doc_.deleteInk(id);
    } else {
        inkByBlock_.insert(id, inkJson);
        doc_.upsertInk(id, inkJson);
    }
    ++inkRevision_;
    emit inkChanged();
    endTxn();
}

void BlockModel::dropBlockInk(const QString& blockId) {
    if (inkByBlock_.remove(blockId)) { ++inkRevision_; emit inkChanged(); }
}

int BlockModel::orderedNumberForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return 0;
    if (rows_[row].type != OrderedListItem) return 0;
    const uint8_t d = rows_[row].depth;
    int n = 1;
    for (int i = row - 1; i >= 0; --i) {
        const Row& x = rows_[i];
        if (isListType(x.type) && x.depth > d) continue;    // children don't break the run
        if (x.type == OrderedListItem && x.depth == d) { ++n; continue; }
        break;                                              // anything else ends the run
    }
    return n;
}

bool BlockModel::matchMarkdownPrefix(const QString& content, BlockType& type, int& level, int& strip) {
    level = 0;
    // Headings: 1–6 leading '#' then a space → heading of that level.
    int h = 0;
    while (h < content.size() && h < 6 && content[h] == QLatin1Char('#')) ++h;
    if (h > 0 && h < content.size() && content[h] == QLatin1Char(' ')) {
        type = Heading; level = h; strip = h + 1; return true;
    }
    // Quote: "> "
    if (content.startsWith(QLatin1String("> "))) { type = Quote; strip = 2; return true; }
    // Task list: "- [ ] " todo, "- [/] " in-progress, "- [x] " done. More specific
    // than the plain "- " rule, so it must be tested first. `level` carries the
    // initial state out to applyMarkdownTrigger (0/1/2).
    if (content.startsWith(QLatin1String("- [ ] "))) { type = TaskListItem; level = TaskTodo;  strip = 6; return true; }
    if (content.startsWith(QLatin1String("- [/] "))) { type = TaskListItem; level = TaskDoing; strip = 6; return true; }
    if (content.startsWith(QLatin1String("- [x] ")) || content.startsWith(QLatin1String("- [X] ")))
        { type = TaskListItem; level = TaskDone; strip = 6; return true; }
    // Unordered list: "- ", "* ", or "+ "
    if (content.startsWith(QLatin1String("- ")) || content.startsWith(QLatin1String("* "))
        || content.startsWith(QLatin1String("+ "))) { type = ListItem; strip = 2; return true; }
    // Ordered list: 1–3 digits + ". " (markdown numbering; the stored item
    // carries no number — it's computed at render time).
    {
        int d = 0;
        while (d < content.size() && d < 3 && content[d].isDigit()) ++d;
        if (d > 0 && d + 1 < content.size()
            && content[d] == QLatin1Char('.') && content[d + 1] == QLatin1Char(' ')) {
            type = OrderedListItem; strip = d + 2; return true;
        }
    }
    // Divider: "--- " (markers + content consumed entirely)
    if (content.startsWith(QLatin1String("--- "))) { type = Divider; strip = 4; return true; }
    return false;
}

QString BlockModel::attrsJson(uint8_t type, uint8_t level, const QString& lang,
                              const std::vector<Span>& spans, uint8_t taskState,
                              int cell, const std::vector<float>& ratios,
                              uint8_t header, const QString& table) const {
    QJsonObject o;
    if (type == Heading && level > 0) o.insert(QStringLiteral("level"), level);
    if (type == TaskListItem) o.insert(QStringLiteral("state"), taskState);
    if (type == Code && !lang.isEmpty()) o.insert(QStringLiteral("lang"), lang);
    if (cell >= 0) o.insert(QStringLiteral("cell"), cell);
    if (type == Split && !ratios.empty()) {
        QJsonArray ra;
        for (float f : ratios) ra.append(std::round(double(f) * 10000.0) / 10000.0);
        o.insert(QStringLiteral("ratios"), ra);
    }
    if (type == Split && header > 0) o.insert(QStringLiteral("header"), header);
    if (type == Split && !table.isEmpty())
        o.insert(QStringLiteral("table"), QJsonDocument::fromJson(table.toUtf8()).object());
    if (!spans.empty()) {
        QJsonArray arr;
        for (const Span& sp : spans) {
            QJsonObject so;
            so.insert(QStringLiteral("s"), sp.s);
            so.insert(QStringLiteral("e"), sp.e);
            so.insert(QStringLiteral("k"), QString::fromLatin1(spanKindToString(sp.kind)));
            if (spanHasPayload(sp.kind) && !sp.href.isEmpty()) so.insert(QStringLiteral("u"), sp.href);
            arr.append(so);
        }
        o.insert(QStringLiteral("spans"), arr);
    }
    return o.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void BlockModel::persistMeta(int row) {
    if (row >= 0 && row < static_cast<int>(rows_.size()) && rows_[size_t(row)].type == Split)
        tableGeomDirty_ = true;                        // a head's column spec may have changed
    if (!doc_.isOpen() || row < 0 || row >= static_cast<int>(ids_.size())) return;
    const Row& r = rows_[row];
    doc_.updateMeta(ids_[row], QString::fromLatin1(typeToString(r.type)),
                    attrsJson(r.type, r.level, r.lang, r.spans, r.taskState, r.cell, r.ratios, r.header, r.table), r.depth);
}

// === Undo / redo: region-snapshot transactions ===========================
BlockModel::BlockSnap BlockModel::snapAt(int row) const {
    BlockSnap s;
    s.id = ids_[row]; s.rank = ranks_[row]; s.content = content_[row];
    s.type = rows_[row].type; s.level = rows_[row].level; s.spans = rows_[row].spans;
    s.taskState = rows_[row].taskState;
    s.depth = rows_[row].depth;
    s.lang = rows_[row].lang;
    s.cell = rows_[row].cell;
    s.ratios = rows_[row].ratios;
    s.header = rows_[row].header;
    s.table = rows_[row].table;
    s.ink = inkByBlock_.value(s.id);   // "" when uninked (the common case)
    return s;
}

std::vector<BlockModel::BlockSnap> BlockModel::snapshotRange(int lo, int hi) const {
    std::vector<BlockSnap> out;
    lo = std::max(0, lo);
    hi = std::min(hi, static_cast<int>(rows_.size()) - 1);
    for (int i = lo; i <= hi; ++i) out.push_back(snapAt(i));
    return out;
}

void BlockModel::applySnapshot(int lo, int oldCount, const std::vector<BlockSnap>& snaps) {
    applying_ = true;
    beginResetModel();
    // Preserve heights across the reset — for EVERY row, keyed by id (was
    // measured-only until 2026-08-20). Untouched rows keep their real
    // heights as before; RESTORED rows now SEED from their pre-reset height
    // too, instead of dropping to estimatedHeight: an estimate can't see a
    // table's cell media, and the delegate correction only fires on an
    // ACTUAL height change — so a history jump landing on a state that
    // renders at the height already shown left the stale estimate standing
    // (the "rows don't resize to fit the pasted image" bug). Seeded rows
    // still clear `measured`, so any real height difference re-reports
    // through the normal chain.
    QHash<QString, double> heightById;
    heightById.reserve(static_cast<int>(rows_.size()));
    QHash<QString, double> measuredById;   // untouched rows: keep flag semantics
    measuredById.reserve(static_cast<int>(rows_.size()));
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        heightById.insert(ids_[i], layout().height(i));
        if (rows_[i].measured) measuredById.insert(ids_[i], layout().height(i));
    }
    const int last = std::min(lo + oldCount, static_cast<int>(rows_.size()));
    QSet<QString> newIds, oldIds;
    bool inkTouched = false;
    for (const BlockSnap& s : snaps) newIds.insert(s.id);
    for (int i = lo; i < last; ++i) {
        oldIds.insert(ids_[i]);
        if (!newIds.contains(ids_[i])) {                     // left the region
            if (inkByBlock_.remove(ids_[i])) inkTouched = true;   // DB cascades
            if (doc_.isOpen()) doc_.deleteBlock(ids_[i]);
        }
    }
    rows_.erase(rows_.begin() + lo, rows_.begin() + last);
    content_.erase(content_.begin() + lo, content_.begin() + last);
    ids_.erase(ids_.begin() + lo, ids_.begin() + last);
    ranks_.erase(ranks_.begin() + lo, ranks_.begin() + last);
    int at = lo;
    for (const BlockSnap& s : snaps) {
        Row r{}; r.type = s.type; r.level = s.level; r.spans = s.spans; r.lang = s.lang;
        r.cell = s.cell; r.ratios = s.ratios;
        r.header = s.header; r.table = s.table;
        tablesDirty_ = true;
        r.taskState = s.taskState;
        r.depth = s.depth;
        r.param = static_cast<uint16_t>(std::max<int>(1, s.content.count(QLatin1Char('\n')) + 1));
        fillMediaMeta(r, s.content);   // media: dims/video/param from descriptor (else height ~= 0)
        rows_.insert(rows_.begin() + at, r);
        content_.insert(content_.begin() + at, s.content);
        ids_.insert(ids_.begin() + at, s.id);
        ranks_.insert(ranks_.begin() + at, s.rank);
        if (doc_.isOpen()) {
            const QString attrs = attrsJson(s.type, s.level, s.lang, s.spans, s.taskState, s.cell, s.ratios, s.header, s.table);
            const QString type = QString::fromLatin1(typeToString(s.type));
            if (oldIds.contains(s.id)) {   // survived: content/meta/rank may all have changed (incl. reorder)
                doc_.updateContent(s.id, s.content);
                doc_.updateMeta(s.id, type, attrs, s.depth);
                doc_.updateRank(s.id, s.rank);
            } else {
                doc_.appendBlock(s.id, s.rank, s.depth, type, attrs, s.content);   // re-born (undo of a delete)
            }
        }
        // Restore the snap's ink (after appendBlock — the FK needs the block
        // row to exist). This is how undoing a block deletion brings its
        // margin ink back.
        if (s.ink != inkByBlock_.value(s.id)) {
            inkTouched = true;
            if (s.ink.isEmpty()) { inkByBlock_.remove(s.id); if (doc_.isOpen()) doc_.deleteInk(s.id); }
            else { inkByBlock_.insert(s.id, s.ink); if (doc_.isOpen()) doc_.upsertInk(s.id, s.ink); }
        }
        ++at;
    }
    const int regionEnd = lo + static_cast<int>(snaps.size());   // [lo,regionEnd) = restored snaps
    std::vector<double> heights; heights.reserve(rows_.size());
    const std::vector<double> lw = laneWidths();   // media estimates use the lane's width
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const bool replaced = (i >= lo && i < regionEnd);        // a restored snap → re-measure
        auto it = measuredById.constFind(ids_[i]);
        if (!replaced && rows_[i].measured && it != measuredById.constEnd()) {
            heights.push_back(it.value());                       // untouched → keep its real height
        } else if (rows_[i].type == Media) {
            // Media heights are ESTIMATE-authoritative — the delegate never
            // measures them back, so seeding the pre-reset height would
            // freeze a stale size after an undone resize (dw reverted, row
            // still at the resized height; user-caught 2026-08-21). The
            // estimate reflects the restored descriptor via fillMediaMeta.
            rows_[i].measured = false;
            heights.push_back(estimatedHeight(rows_[i], lw[size_t(i)]));
        } else {
            rows_[i].measured = false;                           // changed/new → re-measure
            auto hit = heightById.constFind(ids_[i]);
            heights.push_back(hit != heightById.constEnd()
                                  ? hit.value()                  // seed: pre-reset height beats a
                                  : estimatedHeight(rows_[i], lw[size_t(i)]));  // media-blind estimate; re-born → estimate
        }
    }
    reindex(std::move(heights));
    endResetModel();
    ++layoutRevision_; ++contentRevision_;
    if (inkTouched) { ++inkRevision_; emit inkChanged(); }
    emit modelReset(); emit layoutChangedSpike(); emit contentChangedSpike();
    applying_ = false;
}

void BlockModel::applyPatches(const std::vector<UndoPatch>& ps, bool beforeSide) {
    applying_ = true;
    bool inkTouched = false;
    bool restructured = false;   // a patch moved a block between lanes (or retyped a record)
    for (const UndoPatch& p : ps) {
        const BlockSnap& s = beforeSide ? p.before : p.after;
        if (p.row < 0 || p.row >= static_cast<int>(rows_.size())
            || ids_[p.row] != s.id) continue;   // safety net — never expected
        Row& r = rows_[p.row];
        if (r.type != s.type || r.cell != s.cell || r.header != s.header) restructured = true;   // header: the pocket moves
        r.cell = s.cell; r.ratios = s.ratios;
        r.header = s.header; r.table = s.table;
        tablesDirty_ = true;
        r.type = s.type; r.level = s.level; r.lang = s.lang;
        r.taskState = s.taskState; r.depth = s.depth; r.spans = s.spans;
        r.param = static_cast<uint16_t>(std::max<int>(1, s.content.count(QLatin1Char('\n')) + 1));
        fillMediaMeta(r, s.content);   // media: dims/video/param from descriptor
        r.measured = false;            // estimate now, delegate re-reports on render
        content_[p.row] = s.content;
        ranks_[p.row] = s.rank;
        // Keep the CURRENT fenwick height (2026-08-20): same id, in-place
        // content swap — the standing height is a far better guess than a
        // media-blind estimate, and `measured=false` above lets any real
        // height change re-report through the normal chain. EXCEPT media
        // rows (2026-08-21): they never measure back — the estimate is
        // authoritative and must re-derive from the restored descriptor.
        if (r.type == Media)
            setIndexHeight(static_cast<size_t>(p.row), estimatedHeight(r, laneWidthOfRow(p.row)));
        if (doc_.isOpen()) {
            doc_.updateContent(s.id, s.content);
            doc_.updateMeta(s.id, QString::fromLatin1(typeToString(s.type)),
                            attrsJson(s.type, s.level, s.lang, s.spans, s.taskState, s.cell, s.ratios, s.header, s.table),
                            s.depth);
            doc_.updateRank(s.id, s.rank);
        }
        if (s.ink != inkByBlock_.value(s.id)) {
            inkTouched = true;
            if (s.ink.isEmpty()) { inkByBlock_.remove(s.id); if (doc_.isOpen()) doc_.deleteInk(s.id); }
            else { inkByBlock_.insert(s.id, s.ink); if (doc_.isOpen()) doc_.upsertInk(s.id, s.ink); }
        }
        emit dataChanged(index(p.row), index(p.row));   // all roles — type/spans/content may differ
    }
    tableCacheRow_ = -1;               // a patched row may be a table — drop the parse cache
    if (restructured) reindex(std::vector<double>(layout().heights()));
    ++contentRevision_;
    if (inkTouched) { ++inkRevision_; emit inkChanged(); }
    bumpLayout();
    emit contentChangedSpike();
    applying_ = false;
}

void BlockModel::beginTxn(int lo, int hi) {
    if (applying_) return;                      // no recording during undo/redo apply
    if (txnDepth_ == 0) {                        // outermost: capture `before`
        txnLo_ = std::max(0, lo);
        txnHi_ = hi;
        txnSize_ = rows_.size();
        txnBefore_ = snapshotRange(txnLo_, txnHi_);
    }
    ++txnDepth_;                                 // inner mutations just nest in
}

void BlockModel::endTxn(const QString& coalesce) {
    if (applying_) return;
    if (txnDepth_ == 0) return;
    if (--txnDepth_ > 0) return;                 // wait for the outermost to commit
    const int delta = static_cast<int>(rows_.size()) - static_cast<int>(txnSize_);
    std::vector<BlockSnap> after = snapshotRange(txnLo_, txnHi_ + delta);

    // No-op group (e.g. "clear" on an already-plain paragraph) → no undo entry.
    // Every persisted field counts: a change to only a span's payload (a link URL, a
    // chip payload) or only a code block's language is a real edit — it must reach
    // undo and mark the document dirty, or it is never saved.
    auto snapEq = [](const BlockSnap& x, const BlockSnap& y) {
        if (x.id != y.id || x.rank != y.rank || x.type != y.type
            || x.level != y.level || x.taskState != y.taskState || x.depth != y.depth
            || x.content != y.content || x.lang != y.lang
            || x.cell != y.cell || x.ratios != y.ratios
            || x.header != y.header || x.table != y.table
            || x.spans.size() != y.spans.size()
            || x.ink != y.ink) return false;   // last: usually shared → O(1) equal
        for (size_t j = 0; j < x.spans.size(); ++j)
            if (x.spans[j].s != y.spans[j].s || x.spans[j].e != y.spans[j].e
                || x.spans[j].kind != y.spans[j].kind || x.spans[j].href != y.spans[j].href) return false;
        return true;
    };
    auto sameSnaps = [&](const std::vector<BlockSnap>& a, const std::vector<BlockSnap>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!snapEq(a[i], b[i])) return false;
        return true;
    };
    if (sameSnaps(txnBefore_, after)) return;
    markDirty();                                 // a real edit reached the chokepoint

    // Coalesce a run of typing into the previous entry (same key, same single
    // block, contiguous caret) so undo removes the whole run at once.
    // Only a LEAF may absorb an edit (!canRedo() ⇔ undoCur_ has no children):
    // after an undo, undoCur_ is the just-undone entry's PARENT — overwriting
    // its `after` in place would orphan that child's `before`, and a later redo
    // would replay the child onto a state it never followed (resurrecting the
    // pre-undo content). Same-id guards the single block actually matching,
    // not just the row index.
    if (!coalesce.isEmpty() && undoCur_ >= 0 && !canRedo()) {
        UndoEntry& prev = undo_[undoCur_];
        if (prev.coalesce == coalesce && prev.lo == txnLo_
            && prev.after.size() == 1 && txnBefore_.size() == 1 && after.size() == 1
            && prev.after[0].id == txnBefore_[0].id
            && prev.cRowA == cRow_ && prev.cColA == cCol_) {
            prev.after = std::move(after);
            awaitingAfter_ = true;     // next noteCaret stamps prev's caret-after
            ++undoRev_; emit undoStackChanged();
            return;
        }
    }
    UndoEntry e;
    e.lo = txnLo_;
    // Sparse compression (2026-08-20): a multi-row band whose ids line up on
    // both sides is a NON-STRUCTURAL gesture (cross-anchor ink commit,
    // collect rewrite, formatting sweep) — store only the rows that actually
    // changed, as in-place patches. A group move touching rows 2 and 80
    // stops storing the 77 untouched blocks between them, and its apply
    // skips the model reset. Structural bands (counts differ) and
    // single-row entries (the coalesce path) keep the band form.
    bool sparse = txnBefore_.size() == after.size() && txnBefore_.size() > 1;
    if (sparse)
        for (size_t i = 0; i < after.size(); ++i)
            if (txnBefore_[i].id != after[i].id) { sparse = false; break; }
    // T2 (SR-4): a band whose blocks only changed order (and so rank) — a sort, a row
    // move — stores the two (id, rank) orders instead of two snapshots of every block.
    std::vector<std::pair<QString, QString>> permB, permA;
    if (!sparse && txnBefore_.size() == after.size() && after.size() > 1) {
        QHash<QString, int> beforeAt;
        beforeAt.reserve(static_cast<int>(txnBefore_.size()));
        for (size_t i = 0; i < txnBefore_.size(); ++i) beforeAt.insert(txnBefore_[i].id, static_cast<int>(i));
        bool perm = beforeAt.size() == static_cast<int>(after.size());
        for (size_t i = 0; perm && i < after.size(); ++i) {
            const auto it = beforeAt.constFind(after[i].id);
            if (it == beforeAt.constEnd()) { perm = false; break; }
            BlockSnap x = txnBefore_[size_t(it.value())];
            x.rank = after[i].rank;
            perm = snapEq(x, after[i]);
        }
        if (perm)
            for (size_t i = 0; i < after.size(); ++i) {
                permB.push_back({ txnBefore_[i].id, txnBefore_[i].rank });
                permA.push_back({ after[i].id, after[i].rank });
            }
    }
    if (!permB.empty()) {
        e.permBefore = std::move(permB);
        e.permAfter = std::move(permA);
    } else if (sparse) {
        for (size_t i = 0; i < after.size(); ++i) {
            if (snapEq(txnBefore_[i], after[i])) continue;
            e.patches.push_back({txnLo_ + static_cast<int>(i),
                                 std::move(txnBefore_[i]), std::move(after[i])});
        }
    } else {
        e.before = std::move(txnBefore_);
        e.after = std::move(after);
    }
    e.cRowB = cRow_; e.cColB = cCol_; e.aRowB = aRow_; e.aColB = aCol_;
    e.cRowA = cRow_; e.cColA = cCol_; e.aRowA = aRow_; e.aColA = aCol_;   // until noteCaret stamps
    e.parent = undoCur_;
    e.coalesce = coalesce;
    e.ts = QDateTime::currentMSecsSinceEpoch();
    undo_.push_back(std::move(e));
    undoCur_ = static_cast<int>(undo_.size()) - 1;
    awaitingAfter_ = true;
    ++undoRev_; emit undoStackChanged();
}

void BlockModel::clearUndo() {
    undo_.clear(); undoCur_ = -1; txnDepth_ = 0; awaitingAfter_ = false;
    ++undoRev_; emit undoStackChanged();
}

void BlockModel::beginGroup(int loRow, int hiRow) { beginTxn(loRow, hiRow); }
void BlockModel::endGroup() { endTxn(); }

bool BlockModel::hasFormat(int row, int start, int end, const QString& kind) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    return mn::inl::hasFormat(content_[row], rows_[row].spans, start, end, spanKindFromString(kind));
}

bool BlockModel::payloadSpanCovers(int row, int start, int end,
                                   int kind, const QString& value) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    return mn::inl::payloadCovers(content_[row], rows_[row].spans, start, end,
                                  static_cast<uint8_t>(kind), value);
}

// The row format ops edit a copy through the engine, so a no-op never opens a txn.
void BlockModel::commitRowSpans(int row, std::vector<Span>&& spans, const QString& coalesce) {
    beginTxn(row, row);
    rows_[row].spans = std::move(spans);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn(coalesce);
}

void BlockModel::setFormat(int row, int start, int end, const QString& kind, bool on) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || isOpaqueRow(row)) return;
    std::vector<Span> spans = rows_[row].spans;
    if (!mn::inl::setFormat(content_[row], spans, start, end, spanKindFromString(kind), on)) return;
    commitRowSpans(row, std::move(spans));
}

bool BlockModel::canUndo() const { return undoCur_ >= 0; }
bool BlockModel::canRedo() const {
    for (int i = static_cast<int>(undo_.size()) - 1; i >= 0; --i)
        if (undo_[i].parent == undoCur_) return true;
    return false;
}

void BlockModel::undo() {
    if (undoCur_ < 0) return;
    awaitingAfter_ = false;
    const UndoEntry e = undo_[undoCur_];          // copy (applySnapshot won't touch undo_, but be safe)
    // Pure width entries carry no snaps — skip the (view-resetting) apply.
    if (!e.permBefore.empty())
        applyPermutation(e.lo, e.permBefore);
    else if (!e.before.empty() || !e.after.empty())
        applySnapshot(e.lo, static_cast<int>(e.after.size()), e.before);
    else if (!e.patches.empty())
        applyPatches(e.patches, /*beforeSide=*/true);
    applyUndoWidth(e.widthBefore);
    undoCur_ = e.parent;
    markDirty();                                  // undo mutates the doc → unsaved
    emit caretRestoreRequested(e.cRowB, e.cColB, e.aRowB, e.aColB);
    ++undoRev_; emit undoStackChanged();
}

void BlockModel::redo() {
    awaitingAfter_ = false;
    int child = -1;                               // newest child of the current node
    for (int i = static_cast<int>(undo_.size()) - 1; i >= 0; --i)
        if (undo_[i].parent == undoCur_) { child = i; break; }
    if (child < 0) return;
    const UndoEntry e = undo_[child];
    if (!e.permAfter.empty())
        applyPermutation(e.lo, e.permAfter);
    else if (!e.before.empty() || !e.after.empty())
        applySnapshot(e.lo, static_cast<int>(e.before.size()), e.after);
    else if (!e.patches.empty())
        applyPatches(e.patches, /*beforeSide=*/false);
    applyUndoWidth(e.widthAfter);
    undoCur_ = child;
    markDirty();                                  // redo mutates the doc → unsaved
    emit caretRestoreRequested(e.cRowA, e.cColA, e.aRowA, e.aColA);
    ++undoRev_; emit undoStackChanged();
}

// Restore the page width an undo/redo entry recorded (0 = the entry carried
// no width change). Persists to doc_meta so save-after-undo keeps the file
// consistent with what's on screen.
void BlockModel::applyUndoWidth(qreal w) {
    if (w <= 0 || qFuzzyCompare(w, pageWidth_)) return;
    pageWidth_ = w;
    doc_.setPageWidth(int(std::lround(w)));
    emit pageWidthChanged();
}

// Edge-affinity migration (PLAN-page-width, affinity DERIVED not stored):
// a px-space element fully in a margin keeps its CONTENT position — the
// left page edge never moves, so left-margin X (center-relative) shifts
// by -Δw/2 and right-margin by +Δw/2; anything overlapping the column
// stays center-relative. Frame (media) anchors are width-immune.
QString BlockModel::migrateInkForWidth(const QString& inkJson, qreal oldW, qreal newW) {
    if (qFuzzyCompare(oldW, newW)) return {};
    const qreal halfOld = oldW / 2.0;
    const qreal dxEdge = (newW - oldW) / 2.0;
    mn::DocInkAnchor a;
    if (!mn::docInkFromJson(inkJson, a)) return {};
    if (a.space != mn::DocInkAnchor::Px) return {};
    bool changed = false;
    for (qcv::ActiveStroke& s : a.strokes) {
        const QRectF b = qcv::strokeBoundsNorm(s);   // local px, oval-aware
        qreal shift = 0;
        if (b.right() < -halfOld)     shift = -dxEdge;   // left marginalia
        else if (b.left() > halfOld)  shift = dxEdge;    // right marginalia
        if (shift == 0.0) continue;
        const bool oval = (s.tool == qcv::DrawingTool::Oval && s.points.size() >= 2);
        for (size_t i = 0; i < s.points.size(); ++i) {
            if (oval && i == 1) continue;   // radii vector: never translated
            s.points[i].rx() += shift;
        }
        changed = true;
    }
    for (mn::SketchTextSpec& t : a.texts) {
        qreal shift = 0;
        if (t.x + t.w < -halfOld)  shift = -dxEdge;
        else if (t.x > halfOld)    shift = dxEdge;
        if (shift != 0.0) { t.x += shift; changed = true; }
    }
    return changed ? mn::docInkToJson(a) : QString();
}

void BlockModel::setPageWidth(qreal w) {
    if (!documentOpen()) return;
    w = std::clamp<qreal>(w, 400.0, 4000.0);   // sanity; the UI offers detents
    if (qFuzzyCompare(w, pageWidth_)) return;
    const qreal oldW = pageWidth_;

    struct Mig { int row; QString json; };
    std::vector<Mig> migs;
    for (auto it = inkByBlock_.constBegin(); it != inkByBlock_.constEnd(); ++it) {
        const int row = rowForId(it.key());
        if (row < 0) continue;
        const QString json = migrateInkForWidth(it.value(), oldW, w);
        if (!json.isEmpty()) migs.push_back({row, json});
    }

    // Ink rewrites (if any) fold into one txn; the width stamps onto that
    // entry so ⌘Z restores blobs + width atomically. With no ink to migrate,
    // a pure width entry is pushed by hand (endTxn drops empty snapshots).
    const size_t stackBefore = undo_.size();
    if (!migs.empty()) {
        int lo = INT_MAX, hi = -1;
        for (const Mig& m : migs) { lo = std::min(lo, m.row); hi = std::max(hi, m.row); }
        beginTxn(lo, hi);
        for (const Mig& m : migs) setBlockInk(m.row, m.json);
        endTxn();
    }
    pageWidth_ = w;
    doc_.setPageWidth(int(std::lround(w)));
    markDirty();
    if (undo_.size() > stackBefore && undoCur_ == static_cast<int>(undo_.size()) - 1) {
        undo_.back().widthBefore = oldW;
        undo_.back().widthAfter = w;
    } else {
        UndoEntry e;
        e.cRowB = cRow_; e.cColB = cCol_; e.aRowB = aRow_; e.aColB = aCol_;
        e.cRowA = cRow_; e.cColA = cCol_; e.aRowA = aRow_; e.aColA = aCol_;
        e.parent = undoCur_;
        e.widthBefore = oldW;
        e.widthAfter = w;
        e.ts = QDateTime::currentMSecsSinceEpoch();
        undo_.push_back(std::move(e));
        undoCur_ = static_cast<int>(undo_.size()) - 1;
        ++undoRev_; emit undoStackChanged();
    }
    emit pageWidthChanged();
}

// History-panel label: a read-time heuristic over the entry's snapshots —
// honest, cheap, and no mutator had to learn to describe itself. Order
// matters: width > count change > single-facet diffs > fallbacks.
QString BlockModel::entryLabel(const UndoEntry& e) const {
    if (e.widthBefore > 0)
        return tr("Page width %1").arg(int(std::lround(e.widthAfter)));
    const auto& b = e.before;
    const auto& a = e.after;
    if (!e.permBefore.empty()) return tr("Reorder rows");
    if (a.size() > b.size()) {
        const int n = int(a.size() - b.size());
        return n == 1 ? tr("Insert block") : tr("Insert %1 blocks").arg(n);
    }
    if (a.size() < b.size()) {
        const int n = int(b.size() - a.size());
        return n == 1 ? tr("Delete block") : tr("Delete %1 blocks").arg(n);
    }
    bool contentDiff = false, inkDiff = false, spanDiff = false;
    bool metaDiff = false, rankDiff = false, commentAdded = false, langDiff = false;
    bool mediaContent = false;
    int contentRows = 0;
    // Sparse entries carry pairs in `patches`; bands carry them in b/a
    // (equal sizes here — the insert/delete cases returned above).
    const size_t pairs = e.patches.empty() ? a.size() : e.patches.size();
    for (size_t i = 0; i < pairs; ++i) {
        const BlockSnap& x = e.patches.empty() ? b[i] : e.patches[i].before;
        const BlockSnap& y = e.patches.empty() ? a[i] : e.patches[i].after;
        if (x.content != y.content) {
            contentDiff = true;
            ++contentRows;
            if (y.type == Media) mediaContent = true;
        }
        if (x.ink != y.ink) inkDiff = true;
        if (x.rank != y.rank) rankDiff = true;
        if (x.lang != y.lang) langDiff = true;
        if (x.type != y.type || x.level != y.level
            || x.taskState != y.taskState || x.depth != y.depth) metaDiff = true;
        if (x.spans.size() != y.spans.size()) {
            spanDiff = true;
            // A comment span appearing is worth its own name.
            for (const Span& sp : y.spans)
                if (sp.kind == SpanComment) {
                    bool had = false;
                    for (const Span& sb : x.spans)
                        if (sb.kind == SpanComment && sb.href == sp.href) { had = true; break; }
                    if (!had) { commentAdded = true; break; }
                }
        } else {
            for (size_t j = 0; j < x.spans.size(); ++j)
                if (x.spans[j].s != y.spans[j].s || x.spans[j].e != y.spans[j].e
                    || x.spans[j].kind != y.spans[j].kind
                    || x.spans[j].href != y.spans[j].href) { spanDiff = true; break; }
        }
    }
    if (langDiff && !contentDiff && !spanDiff && !metaDiff) return tr("Code language");
    if (inkDiff && !contentDiff && !spanDiff && !metaDiff) return tr("Ink");
    if (commentAdded) return tr("Comment");
    if (spanDiff && !contentDiff && !metaDiff) return tr("Formatting");
    if (rankDiff && !contentDiff && !metaDiff) return tr("Move block");
    if (metaDiff && !contentDiff) return tr("Block type");
    // Resize runs carry their keys (imgw/skrz/tcimgw) — name them before the
    // generic Media/Typing branches would claim them.
    if (e.coalesce.startsWith(QLatin1String("imgw:"))
        || e.coalesce.startsWith(QLatin1String("skrz:"))
        || e.coalesce.startsWith(QLatin1String("tcimgw:")))
        return tr("Resize");
    if (mediaContent && contentRows == 1) return tr("Media");   // sketch/PDF-ink
    if (!e.coalesce.isEmpty()) {
        if (e.coalesce == QLatin1String("del") || e.coalesce.startsWith(QLatin1String("tcell-del:")))
            return tr("Delete text");
        return tr("Typing");
    }
    return tr("Edit");
}

QVariantList BlockModel::undoHistory() const {
    // The active path: ancestors of the current node, then newest-child
    // descendants — the exact states ⌘Z/⌘⇧Z walk.
    std::vector<int> path;
    for (int n = undoCur_; n >= 0; n = undo_[size_t(n)].parent) path.push_back(n);
    std::reverse(path.begin(), path.end());
    for (int n = undoCur_;;) {
        int child = -1;
        for (int i = int(undo_.size()) - 1; i >= 0; --i)
            if (undo_[size_t(i)].parent == n) { child = i; break; }
        if (child < 0) break;
        path.push_back(child);
        n = child;
    }
    QVariantList out;
    // Cap the render: keep the newest 200 states; a leading marker row
    // reports what was elided (never silently truncate).
    constexpr size_t kMax = 200;
    size_t first = 0;
    if (path.size() > kMax) {
        first = path.size() - kMax;
        QVariantMap m;
        m.insert(QStringLiteral("idx"), -2);   // marker, not clickable
        m.insert(QStringLiteral("label"), tr("… %1 earlier steps").arg(qint64(first)));
        m.insert(QStringLiteral("ts"), 0);
        m.insert(QStringLiteral("current"), false);
        m.insert(QStringLiteral("future"), false);
        out.push_back(m);
    } else {
        QVariantMap m;   // the baseline state, before every entry
        m.insert(QStringLiteral("idx"), -1);
        m.insert(QStringLiteral("label"), tr("Opened"));
        m.insert(QStringLiteral("ts"), 0);
        m.insert(QStringLiteral("current"), undoCur_ == -1);
        m.insert(QStringLiteral("future"), false);
        out.push_back(m);
    }
    // With the baseline current (undoCur_ == -1) every path entry is ahead
    // of now — start the latch flipped or nothing would read as future.
    bool past = undoCur_ >= 0;
    for (size_t i = first; i < path.size(); ++i) {
        const int idx = path[i];
        const UndoEntry& e = undo_[size_t(idx)];
        QVariantMap m;
        m.insert(QStringLiteral("idx"), idx);
        m.insert(QStringLiteral("label"), entryLabel(e));
        m.insert(QStringLiteral("ts"), e.ts);
        const bool cur = idx == undoCur_;
        m.insert(QStringLiteral("current"), cur);
        m.insert(QStringLiteral("future"), !past && !cur);
        if (cur) past = false;
        out.push_back(m);
    }
    return out;
}

void BlockModel::undoJumpTo(int target) {
    if (target == undoCur_ || target < -1 || target >= int(undo_.size())) return;
    // Ancestor (or the baseline) → step back; else walk redos along the
    // newest-child chain. A stale/off-path target stalls redo() into a
    // no-op and the guard exits — never a wrong state, just no jump.
    int n = undoCur_;
    while (n >= 0 && n != target) n = undo_[size_t(n)].parent;
    if (n == target) {
        while (undoCur_ != target) undo();
        return;
    }
    int prev = -2;
    while (undoCur_ != target && undoCur_ != prev) { prev = undoCur_; redo(); }
}

void BlockModel::noteCaret(int row, int col, int anchorRow, int anchorCol) {
    cRow_ = row; cCol_ = col; aRow_ = anchorRow; aCol_ = anchorCol;
    if (awaitingAfter_ && undoCur_ >= 0) {
        UndoEntry& e = undo_[undoCur_];
        e.cRowA = row; e.cColA = col; e.aRowA = anchorRow; e.aColA = anchorCol;
        awaitingAfter_ = false;
    }
}

void BlockModel::setHeading(int row, int level) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    Row& r = rows_[row];
    if (r.type != Paragraph && r.type != Heading && r.type != Quote
        && r.type != ListItem && r.type != TaskListItem) return;
    level = std::clamp(level, 0, 6);
    const uint8_t newType  = level == 0 ? static_cast<uint8_t>(Paragraph) : static_cast<uint8_t>(Heading);
    const uint8_t newLevel = level == 0 ? 0 : static_cast<uint8_t>(level);
    if (r.type == newType && r.level == newLevel) return;            // no-op
    beginTxn(row, row);
    r.type = newType;
    r.level = newLevel;
    r.taskState = 0;            // leaving a task item clears its status
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole});
    bumpLayout();                                                    // heading height differs
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::setBlockType(int row, int type) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    Row& r = rows_[row];
    if (r.type != Paragraph && r.type != Heading && r.type != Quote
        && r.type != ListItem && r.type != TaskListItem && r.type != OrderedListItem
        && r.type != Code) return;
    const uint8_t t = static_cast<uint8_t>(type);
    if (t != Paragraph && t != Quote && t != ListItem && t != TaskListItem
        && t != OrderedListItem) return;   // headings/code have their own paths
    if (r.type == t) return;
    beginTxn(row, row);
    r.type = t;
    r.level = 0;                 // leaving a heading clears its level
    r.taskState = 0;             // task status resets (todo when entering, cleared when leaving)
    r.lang.clear();              // leaving a code block clears its language
    if (!isListType(t)) r.depth = 0;   // leaving the list family drops nesting
                                       // (list↔task↔ordered keeps it)
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

int BlockModel::insertDivider(int afterRow) {
  afterRow = std::clamp(afterRow, -1, static_cast<int>(rows_.size()) - 1);
  return insertReplacingEmpty(afterRow, [&](int after) {
    const int at = after + 1;
    beginTxn(at, at - 1);        // empty `before`; after = [at,at]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (at > 0) ? ranks_[at - 1] : QString(),
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());
    beginInsertRows({}, at, at);
    Row r{}; r.type = Divider; r.param = 1; r.cell = laneAt(at);
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, QString());
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    indexInsert(static_cast<size_t>(at), estimatedHeight(r, laneWidthForInsert(at, r.cell)));
    endInsertRows();
    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QStringLiteral("divider"), attrsJson(r.type, r.level, r.lang, r.spans, r.taskState, r.cell, r.ratios, r.header, r.table), QString());
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return at;
  });
}

QString BlockModel::languageForRow(int row) const {
    if (rows_.empty()) return {};
    return rowAt(row).lang;
}

void BlockModel::makeCodeBlock(int row, const QString& lang) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    Row& r = rows_[row];
    if (r.type == Code && r.lang == lang) return;
    beginTxn(row, row);
    r.type = Code; r.level = 0; r.taskState = 0; r.lang = lang;
    r.spans.clear();             // inline markdown/spans are literal inside code
    persistContent(row);         // (spans went to attrs; content unchanged but persist meta)
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::setCodeLanguage(int row, const QString& lang) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    Row& r = rows_[row];
    if (r.type != Code || r.lang == lang) return;
    beginTxn(row, row);
    r.lang = lang;
    persistMeta(row);
    ++contentRevision_;          // CodeHighlighter.language binding depends on contentRevision
    emit contentChangedSpike();
    endTxn();
}

QStringList BlockModel::codeLanguages() const { return codeLanguageNames(); }

QString BlockModel::codeLanguageName(int row) const {
    if (rows_.empty() || rowAt(row).type != Code) return {};
    return codeLanguageDisplayName(rowAt(row).lang);
}

bool BlockModel::makeCodeBlockIfFence(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    if (rows_[row].type != Paragraph) return false;
    if (!content_[row].startsWith(QLatin1String("```"))) return false;
    const QString lang = content_[row].mid(3).trimmed();   // "```python" → "python"
    beginTxn(row, row);
    rows_[row].type = Code; rows_[row].level = 0; rows_[row].lang = lang;
    rows_[row].spans.clear();
    content_[row].clear();                                  // consume the fence line
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole, ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return true;
}

// === Tables ==============================================================
// The grid lives as compact JSON in `content`; mutations reserialize and persist
// through the existing txn chokepoint, so undo/redo/coalescing work unchanged.

const TableGrid& BlockModel::gridFor(int row) const {
    row = clampRow(row);
    if (tableCacheRow_ == row && tableCacheRev_ == contentRevision_) return tableCache_;
    tableCache_ = TableGrid::fromJson(content_[row]);
    tableCacheRow_ = row;
    tableCacheRev_ = contentRevision_;
    return tableCache_;
}

void BlockModel::mutateTable(int row, const std::function<void(TableGrid&)>& fn,
                             const QString& coalesce) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[row].type != Table) return;
    TableGrid g = TableGrid::fromJson(content_[row]);
    fn(g);
    beginTxn(row, row);
    content_[row] = g.toJson();
    rows_[row].param = static_cast<uint16_t>(std::clamp(g.rows(), 1, 65535));
    rows_[row].measured = false;                          // height may change (rows/cols/
                                                          // wrap/col-width) → re-measure;
                                                          // the cache (rowMeasured) is stale
    persistContent(row);
    tableCacheRow_ = -1;                                  // invalidate the parse cache
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn(coalesce);
}

int BlockModel::tableRows(int row) const {
    if (rowAt(row).type != Table) return 0;
    return gridFor(row).rows();
}
int BlockModel::tableColumns(int row) const {
    if (rowAt(row).type != Table) return 0;
    return gridFor(row).cols();
}
int BlockModel::tableHeaderRows(int row) const {
    if (rowAt(row).type != Table) return 0;
    return gridFor(row).headerRows();
}
QString BlockModel::tableCell(int row, int r, int c) const {
    if (rowAt(row).type != Table) return {};
    return gridFor(row).cellText(r, c);
}
int BlockModel::tableColWidth(int row, int c) const {
    if (rowAt(row).type != Table) return 0;
    return gridFor(row).colWidth(c);
}
int BlockModel::tableColAlign(int row, int c) const {
    if (rowAt(row).type != Table) return 0;
    return gridFor(row).colAlign(c);
}

void BlockModel::tableSetCell(int row, int r, int c, const QString& text) {
    // Spans clamp to the new text, so a cleared cell no longer keeps stale spans.
    mutateCellInline(row, r, c, [&](QString& t, std::vector<Span>& v) {
        mn::inl::setText(t, v, text);
        return true;
    }, QStringLiteral("tcell:%1:%2").arg(r).arg(c));
}
void BlockModel::tableSetCellColor(int row, int r0, int c0, int r1, int c1, bool fg, const QString& color) {
    if (r0 > r1) std::swap(r0, r1);
    if (c0 > c1) std::swap(c0, c1);
    mutateTable(row, [&](TableGrid& g){
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
                fg ? g.setCellFg(r, c, color) : g.setCellBg(r, c, color);
    });
}
void BlockModel::tableSetRowColor(int row, int r, bool fg, const QString& color) {
    mutateTable(row, [&](TableGrid& g){ fg ? g.setRowFg(r, color) : g.setRowBg(r, color); });
}
void BlockModel::tableSetColColor(int row, int c, bool fg, const QString& color) {
    mutateTable(row, [&](TableGrid& g){ fg ? g.setColFg(c, color) : g.setColBg(c, color); });
}
// Effective colour for rendering: cell wins, then row, then column, then none.
QString BlockModel::tableCellBg(int row, int r, int c) const {
    if (rowAt(row).type != Table) return {};
    const TableGrid& g = gridFor(row);
    QString v = g.cellBg(r, c);
    if (v.isEmpty()) v = g.rowBg(r);
    if (v.isEmpty()) v = g.colBg(c);
    return v;
}
QString BlockModel::tableCellFg(int row, int r, int c) const {
    if (rowAt(row).type != Table) return {};
    const TableGrid& g = gridFor(row);
    QString v = g.cellFg(r, c);
    if (v.isEmpty()) v = g.rowFg(r);
    if (v.isEmpty()) v = g.colFg(c);
    return v;
}
// --- cell inline spans (rich text inside table cells) ----------------------
// Cells persist spans as a JSON array of {s,e,k,u?}; these converters bridge to
// the Span vector so the block-level span helpers (addSpan/removeSpan/shift…)
// drive cell editing too.
std::vector<BlockModel::Span> BlockModel::cellSpansFromJson(const QJsonArray& a) {
    std::vector<Span> v; v.reserve(a.size());
    for (const QJsonValue& it : a) {
        const QJsonObject o = it.toObject();
        v.push_back({o.value(QStringLiteral("s")).toInt(), o.value(QStringLiteral("e")).toInt(),
                     static_cast<uint8_t>(o.value(QStringLiteral("k")).toInt()),
                     o.value(QStringLiteral("u")).toString()});
    }
    return v;
}
QJsonArray BlockModel::cellSpansToJson(const std::vector<Span>& v) {
    QJsonArray a;
    for (const Span& sp : v) {
        QJsonObject o;
        o.insert(QStringLiteral("s"), sp.s);
        o.insert(QStringLiteral("e"), sp.e);
        o.insert(QStringLiteral("k"), int(sp.kind));
        if (spanHasPayload(sp.kind) && !sp.href.isEmpty()) o.insert(QStringLiteral("u"), sp.href);
        a.append(o);
    }
    return a;
}

void BlockModel::mutateCellInline(int row, int r, int c,
                                  const std::function<bool(QString&, std::vector<Span>&)>& fn,
                                  const QString& coalesce) {
    mutateTable(row, [&](TableGrid& g) {
        QString t = g.cellText(r, c);
        std::vector<Span> v = cellSpansFromJson(g.cellSpans(r, c));
        if (!fn(t, v)) return;
        g.setCellText(r, c, t);
        g.setCellSpans(r, c, cellSpansToJson(v));
    }, coalesce);
}

QVariantList BlockModel::tableCellSpans(int row, int r, int c) const {
    if (rowAt(row).type != Table) return {};
    return mn::inl::spansToVariantList(cellSpansFromJson(gridFor(row).cellSpans(r, c)));
}

bool BlockModel::tableCellHasFormat(int row, int r, int c, int start, int end, const QString& kind) const {
    if (rowAt(row).type != Table) return false;
    const TableGrid& g = gridFor(row);
    return mn::inl::hasFormat(g.cellText(r, c), cellSpansFromJson(g.cellSpans(r, c)),
                              start, end, spanKindFromString(kind));
}

void BlockModel::tableSetCellFormat(int row, int r, int c, int start, int end, const QString& kind, bool on) {
    const uint8_t k = spanKindFromString(kind);
    if (!k) return;
    mutateCellInline(row, r, c, [&](QString& t, std::vector<Span>& v) {
        return mn::inl::setFormat(t, v, start, end, k, on);
    });
}

// Same rule as blocks: style + link + colour spans clear; comments and chips survive
// (a chip's text must stay its label).
void BlockModel::tableClearCellFormat(int row, int r, int c, int start, int end) {
    mutateCellInline(row, r, c, [&](QString& t, std::vector<Span>& v) {
        return mn::inl::clearFormat(t, v, start, end);
    });
}

// Span-aware cell text edits: the same engine ops as blocks, so formatting stays glued
// to its characters as the user types. Coalescing mirrors block typing: only
// single-char inserts ("type") or single-char deletes ("del") join a run, keyed per
// cell — a paste or a switch between typing and deleting starts a new undo entry.
void BlockModel::tableCellInsert(int row, int r, int c, int at, const QString& text) {
    if (text.isEmpty()) return;
    mutateCellInline(row, r, c, [&](QString& t, std::vector<Span>& v) {
        mn::inl::insertText(t, v, at, text);
        return true;
    }, text.size() == 1 ? QStringLiteral("tcell-type:%1:%2").arg(r).arg(c) : QString());
}

void BlockModel::tableCellReplace(int row, int r, int c, int s, int e, const QString& text) {
    mutateCellInline(row, r, c, [&](QString& t, std::vector<Span>& v) {
        return mn::inl::replaceRange(t, v, s, e, text);
    });
}

void BlockModel::tableCellDelete(int row, int r, int c, int from, int to) {
    mutateCellInline(row, r, c, [&](QString& t, std::vector<Span>& v) {
        return mn::inl::deleteRange(t, v, from, to);
    }, to - from == 1 ? QStringLiteral("tcell-del:%1:%2").arg(r).arg(c) : QString());
}

// --- images inside cells ---------------------------------------------------
static QString mediaJson(const MediaStore::ImageRef& ref);   // defined below with the media-block helpers
// Import via MediaStore (sidecar for clipboard bytes, in-place ref for files),
// stash the {src,w,h} descriptor in cell.media, and widen a too-narrow column
// to a sensible default so the image isn't squeezed. One undo step.
bool BlockModel::tableSetCellImageFromClipboard(int row, int r, int c) {
    if (!mediaStore_ || rowAt(row).type != Table) return false;
    const MediaStore::ImageRef ref = mediaStore_->importClipboardImage();
    if (!ref.ok()) return false;
    const QString json = mediaJson(ref);
    const int target = std::clamp(ref.w, 140, 360);
    mutateTable(row, [&](TableGrid& g){
        g.setCellMedia(r, c, json);
        if (g.colWidth(c) < target) g.setColWidth(c, target);
    });
    return true;
}
bool BlockModel::tableSetCellImageFromUrl(int row, int r, int c, const QString& fileUrl,
                                          bool forceCopy) {
    if (!mediaStore_ || rowAt(row).type != Table) return false;
    const MediaStore::ImageRef ref = mediaStore_->importFile(fileUrl, forceCopy);
    if (!ref.ok()) return false;
    const QString json = mediaJson(ref);
    const int target = std::clamp(ref.w, 140, 360);
    mutateTable(row, [&](TableGrid& g){
        g.setCellMedia(r, c, json);
        if (g.colWidth(c) < target) g.setColWidth(c, target);
    });
    return true;
}
void BlockModel::tableClearCellMedia(int row, int r, int c) {
    mutateTable(row, [&](TableGrid& g){ g.setCellMedia(r, c, QString()); });
}
void BlockModel::tableSetCellMedia(int row, int r, int c, const QString& json) {
    if (rowAt(row).type != Table) return;
    mutateTable(row, [&](TableGrid& g){ g.setCellMedia(r, c, json); });
}
QString BlockModel::tableCellMedia(int row, int r, int c) const {
    if (rowAt(row).type != Table) return {};
    return gridFor(row).cellMedia(r, c);
}
QString BlockModel::tableCellMediaUrl(int row, int r, int c) const {
    if (!mediaStore_ || rowAt(row).type != Table) return {};
    const QString m = gridFor(row).cellMedia(r, c);
    if (m.isEmpty()) return {};
    const QJsonObject o = QJsonDocument::fromJson(m.toUtf8()).object();
    return mediaStore_->resolveUrl(o.value(QStringLiteral("src")));
}
int BlockModel::tableCellMediaW(int row, int r, int c) const {
    if (rowAt(row).type != Table) return 0;
    const QString m = gridFor(row).cellMedia(r, c);
    if (m.isEmpty()) return 0;
    return QJsonDocument::fromJson(m.toUtf8()).object().value(QStringLiteral("w")).toInt();
}
int BlockModel::tableCellMediaH(int row, int r, int c) const {
    if (rowAt(row).type != Table) return 0;
    const QString m = gridFor(row).cellMedia(r, c);
    if (m.isEmpty()) return 0;
    return QJsonDocument::fromJson(m.toUtf8()).object().value(QStringLiteral("h")).toInt();
}
int BlockModel::tableCellMediaDw(int row, int r, int c) const {
    if (rowAt(row).type != Table) return 0;
    const QString m = gridFor(row).cellMedia(r, c);
    if (m.isEmpty()) return 0;
    return QJsonDocument::fromJson(m.toUtf8()).object().value(QStringLiteral("dw")).toInt();
}
void BlockModel::tableSetCellImageWidth(int row, int r, int c, int w) {
    mutateTable(row, [&](TableGrid& g){
        const QString m = g.cellMedia(r, c);
        if (m.isEmpty()) return;
        QJsonObject o = QJsonDocument::fromJson(m.toUtf8()).object();
        if (w <= 0) o.remove(QStringLiteral("dw"));
        else        o.insert(QStringLiteral("dw"), std::clamp(w, 40, 4000));
        g.setCellMedia(r, c, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    }, QStringLiteral("tcimgw:%1:%2").arg(r).arg(c));   // nudge-runs coalesce
}

void BlockModel::tableInsertRow(int row, int at)    { mutateTable(row, [&](TableGrid& g){ g.insertRow(at); }); }
void BlockModel::tableInsertColumn(int row, int at) { mutateTable(row, [&](TableGrid& g){ g.insertCol(at); }); }
void BlockModel::tableDeleteRow(int row, int at)    { mutateTable(row, [&](TableGrid& g){ g.deleteRow(at); }); }
void BlockModel::tableDeleteColumn(int row, int at) { mutateTable(row, [&](TableGrid& g){ g.deleteCol(at); }); }
void BlockModel::tableSetColWidth(int row, int c, int w) { mutateTable(row, [&](TableGrid& g){ g.setColWidth(c, w); }); }
void BlockModel::tableSetColAlign(int row, int c, int a) { mutateTable(row, [&](TableGrid& g){ g.setColAlign(c, a); }); }
void BlockModel::tableSetHeaderRows(int row, int n)      { mutateTable(row, [&](TableGrid& g){ g.setHeaderRows(n); }); }
void BlockModel::tableMoveRow(int row, int from, int to)    { mutateTable(row, [&](TableGrid& g){ g.moveRow(from, to); }); }
void BlockModel::tableMoveColumn(int row, int from, int to) { mutateTable(row, [&](TableGrid& g){ g.moveCol(from, to); }); }
void BlockModel::tableDuplicateRow(int row, int at)    { mutateTable(row, [&](TableGrid& g){ g.duplicateRow(at); }); }
void BlockModel::tableDuplicateColumn(int row, int at) { mutateTable(row, [&](TableGrid& g){ g.duplicateCol(at); }); }
void BlockModel::tableSortByColumn(int row, int c, bool asc) { mutateTable(row, [&](TableGrid& g){ g.sortByColumn(c, asc); }); }
void BlockModel::tableFillDown(int row, int r0, int c0, int r1, int c1)  { mutateTable(row, [&](TableGrid& g){ g.fillDown(r0, c0, r1, c1); }); }
void BlockModel::tableFillRight(int row, int r0, int c0, int r1, int c1) { mutateTable(row, [&](TableGrid& g){ g.fillRight(r0, c0, r1, c1); }); }

// ---- choice columns --------------------------------------------------------
int BlockModel::tableColumnKind(int row, int c) const {
    if (rowAt(row).type != Table) return 0;
    return gridFor(row).colKind(c);
}
void BlockModel::tableSetColumnKind(int row, int c, int kind) {
    mutateTable(row, [&](TableGrid& g){ g.setColKind(c, kind); });
}
QVariantList BlockModel::tableColumnOptions(int row, int c) const {
    QVariantList out;
    if (rowAt(row).type != Table) return out;
    for (const TableGrid::Option& o : gridFor(row).colOptions(c)) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), o.id);
        m.insert(QStringLiteral("label"), o.label);
        m.insert(QStringLiteral("color"), o.color);
        out.append(m);
    }
    return out;
}
QString BlockModel::tableAddOption(int row, int c, const QString& label, const QString& color) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[row].type != Table) return QString();
    const QString id = makeUlid();
    mutateTable(row, [&](TableGrid& g){ g.addOption(c, id, label, color); });
    return id;
}
void BlockModel::tableSetColumnOptions(int row, int c, const QVariantList& opts) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[row].type != Table) return;
    std::vector<TableGrid::Option> v;
    v.reserve(opts.size());
    for (const QVariant& it : opts) {
        const QVariantMap m = it.toMap();
        TableGrid::Option o;
        o.id = m.value(QStringLiteral("id")).toString();
        if (o.id.isEmpty()) o.id = makeUlid();              // mint id for a newly-added option
        o.label = m.value(QStringLiteral("label")).toString();
        o.color = m.value(QStringLiteral("color")).toString();
        v.push_back(std::move(o));
    }
    mutateTable(row, [&](TableGrid& g){ g.setColumnOptions(c, v); });
}
void BlockModel::tableRenameOption(int row, int c, const QString& id, const QString& label) {
    mutateTable(row, [&](TableGrid& g){ g.renameOption(c, id, label); });
}
void BlockModel::tableRecolorOption(int row, int c, const QString& id, const QString& color) {
    mutateTable(row, [&](TableGrid& g){ g.recolorOption(c, id, color); });
}
void BlockModel::tableRemoveOption(int row, int c, const QString& id) {
    mutateTable(row, [&](TableGrid& g){ g.removeOption(c, id); });
}
void BlockModel::tableMoveOption(int row, int c, const QString& id, int toIndex) {
    mutateTable(row, [&](TableGrid& g){ g.moveOption(c, id, toIndex); });
}
QString BlockModel::tableCellChoice(int row, int r, int c) const {
    if (rowAt(row).type != Table) return QString();
    return gridFor(row).cellChoice(r, c);
}
void BlockModel::tableSetCellChoice(int row, int r, int c, const QString& id) {
    mutateTable(row, [&](TableGrid& g){ g.setCellChoice(r, c, id); });
}
QString BlockModel::tableCellChoiceLabel(int row, int r, int c) const {
    if (rowAt(row).type != Table) return QString();
    const TableGrid& g = gridFor(row);
    return g.optionLabel(c, g.cellChoice(r, c));
}
QString BlockModel::tableCellChoiceColor(int row, int r, int c) const {
    if (rowAt(row).type != Table) return QString();
    const TableGrid& g = gridFor(row);
    return g.optionColor(c, g.cellChoice(r, c));
}
int BlockModel::tableCellCheck(int row, int r, int c) const {
    if (rowAt(row).type != Table) return 0;
    return gridFor(row).cellCheck(r, c);
}
void BlockModel::tableCycleCellCheck(int row, int r, int c) {
    mutateTable(row, [&](TableGrid& g){ g.cycleCellCheck(r, c); });
}
void BlockModel::tableSetCellCheck(int row, int r, int c, int state) {
    mutateTable(row, [&](TableGrid& g){ g.setCellCheck(r, c, state); });
}
QString BlockModel::tableRowBg(int row, int r) const {
    if (rowAt(row).type != Table) return QString();
    return gridFor(row).rowBg(r);
}

void BlockModel::tablePasteTSV(int row, int r, int c, const QString& tsv) {
    const TableGrid src = TableGrid::fromTSV(tsv);
    mutateTable(row, [&](TableGrid& g){
        while (g.rows() < r + src.rows()) g.insertRow(g.rows());   // grow to fit the paste
        while (g.cols() < c + src.cols()) g.insertCol(g.cols());
        for (int i = 0; i < src.rows(); ++i)
            for (int j = 0; j < src.cols(); ++j) {
                g.clearCellContents(r + i, c + j);   // nothing stale rides (spans/chip/image)
                g.setCellText(r + i, c + j, src.cellText(i, j));
            }
    });
}

QString BlockModel::tableRangeTSV(int row, int r0, int c0, int r1, int c1) const {
    if (rowAt(row).type != Table) return {};
    const TableGrid& g = gridFor(row);
    const int R0 = std::min(r0, r1), R1 = std::max(r0, r1);
    const int C0 = std::min(c0, c1), C1 = std::max(c0, c1);
    QString out;
    for (int r = R0; r <= R1; ++r) {
        for (int c = C0; c <= C1; ++c) {
            if (c > C0) out += QLatin1Char('\t');
            QString t = g.cellText(r, c);
            t.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
            out += t;
        }
        if (r < R1) out += QLatin1Char('\n');
    }
    return out;
}

QString BlockModel::tableRangeHtml(int row, int r0, int c0, int r1, int c1) const {
    if (rowAt(row).type != Table) return {};
    const TableGrid& g = gridFor(row);
    const int R0 = std::min(r0, r1), R1 = std::max(r0, r1);
    const int C0 = std::min(c0, c1), C1 = std::max(c0, c1);
    QString out = QStringLiteral("<table>");
    for (int r = R0; r <= R1; ++r) {
        out += QStringLiteral("<tr>");
        for (int c = C0; c <= C1; ++c)
            out += QStringLiteral("<td>") + g.cellText(r, c).toHtmlEscaped() + QStringLiteral("</td>");
        out += QStringLiteral("</tr>");
    }
    return out + QStringLiteral("</table>");
}

void BlockModel::tableClearRange(int row, int r0, int c0, int r1, int c1) {
    mutateTable(row, [&](TableGrid& g) {
        const int R0 = std::min(r0, r1), R1 = std::max(r0, r1);
        const int C0 = std::min(c0, c1), C1 = std::max(c0, c1);
        for (int r = R0; r <= R1; ++r)
            for (int c = C0; c <= C1; ++c) g.clearCellContents(r, c);
    });
}

// ---- Bulk ops over selection sets (table multi-select, 2026-08-21) --------

namespace {
// QML index list → sorted unique ints (ascending). Any order, dupes fine.
std::vector<int> sortedIndexSet(const QVariantList& in) {
    std::vector<int> v;
    v.reserve(size_t(in.size()));
    for (const QVariant& x : in) v.push_back(x.toInt());
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
} // namespace

void BlockModel::tableDeleteRows(int row, const QVariantList& rows) {
    const std::vector<int> set = sortedIndexSet(rows);
    if (set.empty()) return;
    mutateTable(row, [&](TableGrid& g) {
        for (auto it = set.rbegin(); it != set.rend(); ++it)   // descending: indices stay valid
            g.deleteRow(*it);                                   // floors at 1 row
    });
}
void BlockModel::tableDeleteColumns(int row, const QVariantList& cols) {
    const std::vector<int> set = sortedIndexSet(cols);
    if (set.empty()) return;
    mutateTable(row, [&](TableGrid& g) {
        for (auto it = set.rbegin(); it != set.rend(); ++it)
            g.deleteCol(*it);                                   // floors at 1 col
    });
}
void BlockModel::tableClearRows(int row, const QVariantList& rows) {
    const std::vector<int> set = sortedIndexSet(rows);
    if (set.empty()) return;
    mutateTable(row, [&](TableGrid& g) {
        for (int r : set)
            for (int c = 0; c < g.cols(); ++c) g.clearCellContents(r, c);
    });
}
void BlockModel::tableClearColumns(int row, const QVariantList& cols) {
    const std::vector<int> set = sortedIndexSet(cols);
    if (set.empty()) return;
    mutateTable(row, [&](TableGrid& g) {
        for (int c : set)
            for (int r = 0; r < g.rows(); ++r) g.clearCellContents(r, c);
    });
}
void BlockModel::tableSetRowsColor(int row, const QVariantList& rows,
                                   bool fg, const QString& color) {
    const std::vector<int> set = sortedIndexSet(rows);
    if (set.empty()) return;
    mutateTable(row, [&](TableGrid& g) {
        for (int r : set) fg ? g.setRowFg(r, color) : g.setRowBg(r, color);
    });
}
void BlockModel::tableSetColsColor(int row, const QVariantList& cols,
                                   bool fg, const QString& color) {
    const std::vector<int> set = sortedIndexSet(cols);
    if (set.empty()) return;
    mutateTable(row, [&](TableGrid& g) {
        for (int c : set) fg ? g.setColFg(c, color) : g.setColBg(c, color);
    });
}
void BlockModel::tableSetColsAlign(int row, const QVariantList& cols, int a) {
    const std::vector<int> set = sortedIndexSet(cols);
    if (set.empty()) return;
    mutateTable(row, [&](TableGrid& g) {
        for (int c : set) g.setColAlign(c, a);
    });
}
void BlockModel::tableSetColumnsKind(int row, const QVariantList& cols, int kind) {
    const std::vector<int> set = sortedIndexSet(cols);
    if (set.empty()) return;
    mutateTable(row, [&](TableGrid& g) {
        for (int c : set) g.setColKind(c, kind);   // per-column cell wipe, as single-kind does
    });
}

QString BlockModel::tableRowsTSV(int row, const QVariantList& rows) const {
    if (rowAt(row).type != Table) return {};
    const TableGrid& g = gridFor(row);
    const std::vector<int> set = sortedIndexSet(rows);
    QString out;
    bool firstRow = true;
    for (int r : set) {
        if (r < 0 || r >= g.rows()) continue;
        if (!firstRow) out += QLatin1Char('\n');
        firstRow = false;
        for (int c = 0; c < g.cols(); ++c) {
            if (c > 0) out += QLatin1Char('\t');
            QString t = g.cellDisplay(r, c);
            t.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
            out += t;
        }
    }
    return out;
}
QString BlockModel::tableRowsHtml(int row, const QVariantList& rows) const {
    if (rowAt(row).type != Table) return {};
    const TableGrid& g = gridFor(row);
    const std::vector<int> set = sortedIndexSet(rows);
    QString out = QStringLiteral("<table>");
    for (int r : set) {
        if (r < 0 || r >= g.rows()) continue;
        out += QStringLiteral("<tr>");
        for (int c = 0; c < g.cols(); ++c)
            out += QStringLiteral("<td>") + g.cellDisplay(r, c).toHtmlEscaped() + QStringLiteral("</td>");
        out += QStringLiteral("</tr>");
    }
    return out + QStringLiteral("</table>");
}
QString BlockModel::tableColsTSV(int row, const QVariantList& cols) const {
    if (rowAt(row).type != Table) return {};
    const TableGrid& g = gridFor(row);
    const std::vector<int> set = sortedIndexSet(cols);
    QString out;
    for (int r = 0; r < g.rows(); ++r) {
        if (r > 0) out += QLatin1Char('\n');
        bool firstCol = true;
        for (int c : set) {
            if (c < 0 || c >= g.cols()) continue;
            if (!firstCol) out += QLatin1Char('\t');
            firstCol = false;
            QString t = g.cellDisplay(r, c);
            t.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
            out += t;
        }
    }
    return out;
}
QString BlockModel::tableColsHtml(int row, const QVariantList& cols) const {
    if (rowAt(row).type != Table) return {};
    const TableGrid& g = gridFor(row);
    const std::vector<int> set = sortedIndexSet(cols);
    QString out = QStringLiteral("<table>");
    for (int r = 0; r < g.rows(); ++r) {
        out += QStringLiteral("<tr>");
        for (int c : set) {
            if (c < 0 || c >= g.cols()) continue;
            out += QStringLiteral("<td>") + g.cellDisplay(r, c).toHtmlEscaped() + QStringLiteral("</td>");
        }
        out += QStringLiteral("</tr>");
    }
    return out + QStringLiteral("</table>");
}

QStringList BlockModel::tableBlockIds() const {
    QStringList out;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].type == Table) out.append(ids_[i]);
    return out;
}

QStringList BlockModel::pdfBlockIds() const {
    QStringList out;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].type == Media && rows_[i].isPdf) out.append(ids_[i]);
    return out;
}

QStringList BlockModel::videoBlockIds() const {
    QStringList out;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].type == Media && rows_[i].isVideo) out.append(ids_[i]);
    return out;
}

QStringList BlockModel::sketchBlockIds() const {
    QStringList out;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].type == Media && rows_[i].isSketch) out.append(ids_[i]);
    return out;
}

int BlockModel::insertSketch(int afterRow) {
  return insertReplacingEmpty(afterRow, [&](int after) {
    const int at = std::clamp(after + 1, 0, static_cast<int>(rows_.size()));
    // Default canvas: PAGE-width × 480 source px (amends the square-480
    // ruling, 2026-08-21 — illustrations arrive full width, and exports
    // rasterize at page resolution). Strokes are normalized [0,1] of the
    // canvas — QCView's exact stroke schema, so the one engine
    // serializes/renders sketches and video notes alike.
    QJsonObject root;
    root.insert(QStringLiteral("kind"), QStringLiteral("sketch"));
    root.insert(QStringLiteral("w"), std::max(480, int(std::lround(pageWidth_))));
    root.insert(QStringLiteral("h"), 480);
    root.insert(QStringLiteral("version"), QStringLiteral("2.0"));
    root.insert(QStringLiteral("coordinate_system"), QStringLiteral("normalized"));
    root.insert(QStringLiteral("shapes"), QJsonArray{});
    const QString json = QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));

    beginTxn(at, at - 1);                        // empty `before`; after = [at,at]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (at > 0) ? ranks_[at - 1] : QString(),
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    beginInsertRows({}, at, at);
    Row r{}; r.cell = laneAt(at); r.type = Media;
    fillMediaMeta(r, json);                      // dims/kind from the descriptor
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, json);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    indexInsert(static_cast<size_t>(at), estimatedHeight(r, laneWidthForInsert(at, r.cell)));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), attrsJson(r.type, r.level, r.lang, r.spans, r.taskState, r.cell, r.ratios, r.header, r.table), json);

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return at;
  });
}

void BlockModel::sketchSetShapes(int row, const QString& strokesJson) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isSketch) return;
    // Merge: stroke fields come from the engine's JSON (which knows only the
    // QCView schema); the canvas meta (kind/w/h/dw) is preserved verbatim.
    QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const QJsonObject in = QJsonDocument::fromJson(strokesJson.toUtf8()).object();
    root.insert(QStringLiteral("version"),
                in.value(QStringLiteral("version")).toString(QStringLiteral("2.0")));
    root.insert(QStringLiteral("coordinate_system"),
                in.value(QStringLiteral("coordinate_system")).toString(QStringLiteral("normalized")));
    root.insert(QStringLiteral("shapes"),
                in.contains(QStringLiteral("shapes")) ? in.value(QStringLiteral("shapes"))
                                                      : QJsonValue(QJsonArray{}));
    beginTxn(row, row);
    content_[row] = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();   // no coalesce — one undo step per stroke (ruling 2026-06-11)
}

bool BlockModel::sketchAppendImage(int row, const QString& src, int iw, int ih) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isSketch) return false;
    if (src.isEmpty() || iw <= 0 || ih <= 0) return false;
    QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const double cw = root.value(QStringLiteral("w")).toInt(480);
    const double ch = root.value(QStringLiteral("h")).toInt(480);
    // Place centered, fit within 70% of the canvas (never upscale past intrinsic).
    const double frac = 0.70;
    const double s = std::min({ (cw * frac) / iw, (ch * frac) / ih, 1.0 });
    const double wN = (iw * s) / cw, hN = (ih * s) / ch;
    QJsonArray images = root.value(QStringLiteral("images")).toArray();
    // Cascade each subsequent paste off-center so multiples don't stack exactly
    // (until select/move lands). Clamp so the image stays on the canvas.
    const double off = std::min(0.04 * images.size(), 0.30);
    QJsonObject img;
    img.insert(QStringLiteral("src"), mn::toRef(src));
    img.insert(QStringLiteral("x"), std::clamp((1.0 - wN) / 2.0 + off, 0.0, 1.0 - wN));
    img.insert(QStringLiteral("y"), std::clamp((1.0 - hN) / 2.0 + off, 0.0, 1.0 - hN));
    img.insert(QStringLiteral("w"), wN);
    img.insert(QStringLiteral("h"), hN);
    images.append(img);
    root.insert(QStringLiteral("images"), images);

    beginTxn(row, row);
    content_[row] = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();   // one undo step per pasted image
    return true;
}

bool BlockModel::sketchAddImageFromClipboard(int row) {
    if (!mediaStore_) return false;
    const MediaStore::ImageRef ref = mediaStore_->importClipboardImage();
    if (!ref.ok()) return false;
    return sketchAppendImage(row, ref.src, ref.w, ref.h);
}

bool BlockModel::sketchAddImageFromUrl(int row, const QString& fileUrl, bool forceCopy) {
    if (!mediaStore_) return false;
    const MediaStore::ImageRef ref = mediaStore_->importFile(fileUrl, forceCopy);   // image only
    if (!ref.ok()) return false;
    return sketchAppendImage(row, ref.src, ref.w, ref.h);
}

QString BlockModel::sketchResolvedJson(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
    if (!rows_[row].isSketch || !mediaStore_) return content_[row];
    QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const QJsonArray images = root.value(QStringLiteral("images")).toArray();
    if (images.isEmpty()) return content_[row];
    QJsonArray out;
    for (const QJsonValue& v : images) {
        QJsonObject o = v.toObject();
        o.insert(QStringLiteral("src"),
                 mediaStore_->resolveUrl(o.value(QStringLiteral("src"))));
        out.append(o);
    }
    root.insert(QStringLiteral("images"), out);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void BlockModel::sketchSetImageRect(int row, int idx,
                                    qreal x, qreal y, qreal w, qreal h) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isSketch) return;
    QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    QJsonArray images = root.value(QStringLiteral("images")).toArray();
    if (idx < 0 || idx >= images.size()) return;
    QJsonObject o = images.at(idx).toObject();
    o.insert(QStringLiteral("x"), x);  o.insert(QStringLiteral("y"), y);
    o.insert(QStringLiteral("w"), w);  o.insert(QStringLiteral("h"), h);
    images.replace(idx, o);
    root.insert(QStringLiteral("images"), images);

    beginTxn(row, row);
    content_[row] = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::sketchRemoveImage(int row, int idx) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isSketch) return;
    QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    QJsonArray images = root.value(QStringLiteral("images")).toArray();
    if (idx < 0 || idx >= images.size()) return;
    images.removeAt(idx);
    root.insert(QStringLiteral("images"), images);

    beginTxn(row, row);
    content_[row] = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

// Canvas-frame bounds (source px). Min keeps a degenerate frame grabbable;
// max is the raster/export ceiling (a 4K paste keeps native resolution).
static constexpr int kSketchMinDim = 64;
static constexpr int kSketchMaxDim = 8192;

// Apply per-side source-px deltas to a sketch descriptor, rewriting stroke
// points and image rects so the ink keeps its position relative to the old
// frame. Pure JSON — unknown fields pass through. If the [min,max] clamp
// shrinks the requested growth, the left/top share is re-derived
// proportionally so the mapping always matches the w/h actually written.
// Ovals encode {center, radii}: radii are scale quantities, not positions —
// they rescale but take no origin shift (mirrors strokeBoundsNorm/paintStroke).
static QJsonObject renormalizeSketchContent(QJsonObject root,
                                            int dl, int dt, int dr, int db) {
    const int oldW = root.value(QStringLiteral("w")).toInt(480);
    const int oldH = root.value(QStringLiteral("h")).toInt(480);
    const int newW = std::clamp(oldW + dl + dr, kSketchMinDim, kSketchMaxDim);
    const int newH = std::clamp(oldH + dt + db, kSketchMinDim, kSketchMaxDim);
    auto effLeft = [](int dA, int dTotalWanted, int dTotalEff) {
        if (dTotalWanted == dTotalEff || dTotalWanted == 0) return dA;
        return int(std::lround(double(dA) * double(dTotalEff) / double(dTotalWanted)));
    };
    const double eDl = effLeft(dl, dl + dr, newW - oldW);
    const double eDt = effLeft(dt, dt + db, newH - oldH);
    const double oW = oldW, oH = oldH, nW = newW, nH = newH;

    QJsonArray shapes = root.value(QStringLiteral("shapes")).toArray();
    for (int i = 0; i < shapes.size(); ++i) {
        QJsonObject s = shapes.at(i).toObject();
        const bool oval = s.value(QStringLiteral("type")).toString()
                          == QLatin1String("oval");
        QJsonArray pts = s.value(QStringLiteral("points")).toArray();
        for (int j = 0; j < pts.size(); ++j) {
            const QJsonArray p = pts.at(j).toArray();
            if (p.size() < 2) continue;
            QJsonArray np;
            if (oval && j == 1) {   // radii: rescale only
                np.append(p.at(0).toDouble() * oW / nW);
                np.append(p.at(1).toDouble() * oH / nH);
            } else {
                np.append((p.at(0).toDouble() * oW + eDl) / nW);
                np.append((p.at(1).toDouble() * oH + eDt) / nH);
            }
            for (int k = 2; k < p.size(); ++k) np.append(p.at(k));
            pts.replace(j, np);
        }
        s.insert(QStringLiteral("points"), pts);
        shapes.replace(i, s);
    }
    root.insert(QStringLiteral("shapes"), shapes);

    if (root.contains(QStringLiteral("images"))) {
        QJsonArray images = root.value(QStringLiteral("images")).toArray();
        for (int i = 0; i < images.size(); ++i) {
            QJsonObject o = images.at(i).toObject();
            o.insert(QStringLiteral("x"),
                     (o.value(QStringLiteral("x")).toDouble() * oW + eDl) / nW);
            o.insert(QStringLiteral("y"),
                     (o.value(QStringLiteral("y")).toDouble() * oH + eDt) / nH);
            o.insert(QStringLiteral("w"),
                     o.value(QStringLiteral("w")).toDouble() * oW / nW);
            o.insert(QStringLiteral("h"),
                     o.value(QStringLiteral("h")).toDouble() * oH / nH);
            images.replace(i, o);
        }
        root.insert(QStringLiteral("images"), images);
    }

    if (root.contains(QStringLiteral("texts"))) {
        QJsonArray texts = root.value(QStringLiteral("texts")).toArray();
        for (int i = 0; i < texts.size(); ++i) {
            QJsonObject o = texts.at(i).toObject();
            o.insert(QStringLiteral("x"),
                     (o.value(QStringLiteral("x")).toDouble() * oW + eDl) / nW);
            o.insert(QStringLiteral("y"),
                     (o.value(QStringLiteral("y")).toDouble() * oH + eDt) / nH);
            o.insert(QStringLiteral("w"),
                     o.value(QStringLiteral("w")).toDouble() * oW / nW);
            // size (source px), text, color untouched: absolute wrap width and
            // glyph size are preserved, like image absolute size.
            texts.replace(i, o);
        }
        root.insert(QStringLiteral("texts"), texts);
    }

    root.insert(QStringLiteral("w"), newW);
    root.insert(QStringLiteral("h"), newH);
    return root;
}

// Signed ink bbox of a sketch descriptor in source px (strokes padded by half
// their width, images as-is), or an invalid rect when the sketch is empty.
static QRectF sketchInkBoundsSrc(const QJsonObject& root) {
    const double w = root.value(QStringLiteral("w")).toInt(480);
    const double h = root.value(QStringLiteral("h")).toInt(480);
    QRectF acc;
    auto add = [&acc](const QRectF& r) { acc = acc.isValid() ? acc.united(r) : r; };
    const QJsonArray shapes = root.value(QStringLiteral("shapes")).toArray();
    for (const QJsonValue& v : shapes) {
        const QJsonObject s = v.toObject();
        const QJsonArray pts = s.value(QStringLiteral("points")).toArray();
        if (pts.isEmpty()) continue;
        QRectF b;
        if (s.value(QStringLiteral("type")).toString() == QLatin1String("oval")
            && pts.size() >= 2) {
            const QJsonArray c = pts.at(0).toArray(), r = pts.at(1).toArray();
            if (c.size() < 2 || r.size() < 2) continue;
            const double rx = std::abs(r.at(0).toDouble()) * w;
            const double ry = std::abs(r.at(1).toDouble()) * h;
            b = QRectF(c.at(0).toDouble() * w - rx, c.at(1).toDouble() * h - ry,
                       2.0 * rx, 2.0 * ry);
        } else {
            double minX = 0, maxX = 0, minY = 0, maxY = 0; bool first = true;
            for (const QJsonValue& pv : pts) {
                const QJsonArray p = pv.toArray();
                if (p.size() < 2) continue;
                const double x = p.at(0).toDouble() * w, y = p.at(1).toDouble() * h;
                if (first) { minX = maxX = x; minY = maxY = y; first = false; }
                else { minX = std::min(minX, x); maxX = std::max(maxX, x);
                       minY = std::min(minY, y); maxY = std::max(maxY, y); }
            }
            if (first) continue;
            b = QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
        }
        const double pad = s.value(QStringLiteral("stroke_width")).toDouble(2.5) * 0.5;
        add(b.adjusted(-pad, -pad, pad, pad));
    }
    const QJsonArray images = root.value(QStringLiteral("images")).toArray();
    for (const QJsonValue& v : images) {
        const QJsonObject o = v.toObject();
        add(QRectF(o.value(QStringLiteral("x")).toDouble() * w,
                   o.value(QStringLiteral("y")).toDouble() * h,
                   o.value(QStringLiteral("w")).toDouble() * w,
                   o.value(QStringLiteral("h")).toDouble() * h));
    }
    for (mn::SketchTextSpec t : mn::parseSketchTexts(root)) {
        t.family = mn::sketchTextFamily();
        const QRectF r = mn::sketchTextRectSrc(t, w, h);   // derived height
        if (r.height() > 0) add(r);
    }
    return acc;
}

void BlockModel::sketchResizeCanvas(int row, int dl, int dt, int dr, int db) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isSketch) return;
    if (dl == 0 && dt == 0 && dr == 0 && db == 0) return;
    const QJsonObject root = renormalizeSketchContent(
        QJsonDocument::fromJson(content_[row].toUtf8()).object(), dl, dt, dr, db);
    const QString json = QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (json == content_[row]) return;   // clamped to a no-op
    // The setMediaWidth sequence: w/h feed layout, so unlike the other sketch
    // mutators this must re-derive the media meta + Fenwick height.
    beginTxn(row, row);
    content_[row] = json;
    fillMediaMeta(rows_[row], json);
    rows_[row].measured = false;
    setIndexHeight(static_cast<size_t>(row), estimatedHeight(rows_[row], laneWidthOfRow(row)));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn(QStringLiteral("skrz:") + ids_[row]);   // frame nudges coalesce (see setMediaWidth)
}

bool BlockModel::sketchFitToInk(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isSketch) return false;
    const QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const QRectF ink = sketchInkBoundsSrc(root);
    if (!ink.isValid()) return false;                  // empty sketch
    const double margin = 8.0;
    const int oldW = root.value(QStringLiteral("w")).toInt(480);
    const int oldH = root.value(QStringLiteral("h")).toInt(480);
    // Epsilon absorbs the renormalization round-trip (coord*newW can land a
    // hair under an exact px), so fitting an already-fit frame is a no-op.
    const double eps = 1e-6;
    const int dl = -int(std::floor(ink.left() - margin + eps));
    const int dt = -int(std::floor(ink.top() - margin + eps));
    const int dr = int(std::ceil(ink.right() + margin - eps)) - oldW;
    const int db = int(std::ceil(ink.bottom() + margin - eps)) - oldH;
    if (dl == 0 && dt == 0 && dr == 0 && db == 0) return false;
    sketchResizeCanvas(row, dl, dt, dr, db);
    return true;
}

int BlockModel::sketchAddText(int row, qreal x, qreal y, qreal w,
                              const QString& text, qreal size,
                              const QString& colorHex) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isSketch) return -1;
    if (text.trimmed().isEmpty() || size <= 0) return -1;
    QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const double srcW = root.value(QStringLiteral("w")).toInt(480);
    QJsonObject t;
    t.insert(QStringLiteral("text"), text);
    t.insert(QStringLiteral("x"), x);
    t.insert(QStringLiteral("y"), y);
    t.insert(QStringLiteral("w"), std::max(w, (2.0 * size) / srcW));   // 2em floor
    t.insert(QStringLiteral("size"), size);
    t.insert(QStringLiteral("color"), colorHex);
    QJsonArray texts = root.value(QStringLiteral("texts")).toArray();
    texts.append(t);
    root.insert(QStringLiteral("texts"), texts);

    beginTxn(row, row);
    content_[row] = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return texts.size() - 1;
}

void BlockModel::sketchSetText(int row, int idx, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isSketch) return;
    QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    QJsonArray texts = root.value(QStringLiteral("texts")).toArray();
    if (idx < 0 || idx >= texts.size()) return;
    QJsonObject o = texts.at(idx).toObject();
    if (o.value(QStringLiteral("text")).toString() == text) return;   // no txn
    if (text.trimmed().isEmpty()) {
        texts.removeAt(idx);            // blank commit = delete (overlay contract)
    } else {
        o.insert(QStringLiteral("text"), text);
        texts.replace(idx, o);
    }
    root.insert(QStringLiteral("texts"), texts);

    beginTxn(row, row);
    content_[row] = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::sketchSetTextBox(int row, int idx, qreal x, qreal y,
                                  qreal w, qreal size) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isSketch) return;
    if (size <= 0) return;
    QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    QJsonArray texts = root.value(QStringLiteral("texts")).toArray();
    if (idx < 0 || idx >= texts.size()) return;
    const double srcW = root.value(QStringLiteral("w")).toInt(480);
    QJsonObject o = texts.at(idx).toObject();
    o.insert(QStringLiteral("x"), x);
    o.insert(QStringLiteral("y"), y);
    o.insert(QStringLiteral("w"), std::max(w, (2.0 * size) / srcW));
    o.insert(QStringLiteral("size"), size);
    texts.replace(idx, o);
    root.insert(QStringLiteral("texts"), texts);

    beginTxn(row, row);
    content_[row] = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::sketchRemoveText(int row, int idx) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isSketch) return;
    QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    QJsonArray texts = root.value(QStringLiteral("texts")).toArray();
    if (idx < 0 || idx >= texts.size()) return;
    texts.removeAt(idx);
    root.insert(QStringLiteral("texts"), texts);

    beginTxn(row, row);
    content_[row] = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

int BlockModel::rowForId(const QString& id) const {
    for (size_t i = 0; i < ids_.size(); ++i)
        if (ids_[i] == id) return static_cast<int>(i);
    return -1;
}

QString BlockModel::idForRow(int row) const {
    return (row >= 0 && row < static_cast<int>(ids_.size())) ? ids_[row] : QString();
}

int BlockModel::insertTable(int afterRow, int nRows, int nCols) {
  nRows = std::max(1, nRows); nCols = std::max(1, nCols);
  const QString json = TableGrid::makeEmpty(nRows, nCols).toJson();
  return insertReplacingEmpty(afterRow, [&](int after) {
    const int at = std::clamp(after + 1, 0, static_cast<int>(rows_.size()));
    beginTxn(at, at - 1);                        // empty `before`; after = [at,at]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (at > 0) ? ranks_[at - 1] : QString(),
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    beginInsertRows({}, at, at);
    Row r{}; r.cell = laneAt(at); r.type = Table; r.param = static_cast<uint16_t>(nRows);
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, json);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    indexInsert(static_cast<size_t>(at), estimatedHeight(r, laneWidthForInsert(at, r.cell)));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), attrsJson(r.type, r.level, r.lang, r.spans, r.taskState, r.cell, r.ratios, r.header, r.table), json);

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return at;
  });
}

int BlockModel::insertTableFromTSV(int afterRow, const QString& tsv) {
  const TableGrid g = TableGrid::fromTSV(tsv);
  if (g.rows() < 1 || g.cols() < 1) return -1;
  const QString json = g.toJson();
  return insertReplacingEmpty(afterRow, [&](int after) {
    const int at = std::clamp(after + 1, 0, static_cast<int>(rows_.size()));
    beginTxn(at, at - 1);                         // empty `before`; after = [at,at]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (at > 0) ? ranks_[at - 1] : QString(),
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    beginInsertRows({}, at, at);
    Row r{}; r.cell = laneAt(at); r.type = Table; r.param = static_cast<uint16_t>(g.rows());
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, json);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    indexInsert(static_cast<size_t>(at), estimatedHeight(r, laneWidthForInsert(at, r.cell)));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), attrsJson(r.type, r.level, r.lang, r.spans, r.taskState, r.cell, r.ratios, r.header, r.table), json);

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return at;
  });
}

// === Media ===============================================================
// The descriptor ({src,w,h}) lives as JSON in `content` (like tables), so undo +
// persistence reuse the chokepoint. Bytes are never stored here (see MediaStore).

bool BlockModel::isConsumableAnchor(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    if (rows_[row].type != Paragraph) return false;
    if (!content_[row].isEmpty()) return false;
    return inkForRow(row).isEmpty();   // ink pins its block — never consume it
}

int BlockModel::insertReplacingEmpty(int afterRow, const std::function<int(int)>& ins) {
    if (!isConsumableAnchor(afterRow)) return ins(afterRow);
    // One undo step: the outer txn region is the consumed anchor (net row
    // delta 0 — one in, one out), and the nested insert/remove txns fold in.
    beginTxn(afterRow, afterRow);
    const int at = ins(afterRow);
    removeBlock(afterRow);
    endTxn();
    return at - 1;                     // the new block slid into the anchor's row
}

int BlockModel::insertMedia(int afterRow, const QString& json, uint16_t aspectParam) {
  return insertReplacingEmpty(afterRow, [&](int after) {
    const int at = std::clamp(after + 1, 0, static_cast<int>(rows_.size()));
    beginTxn(at, at - 1);
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (at > 0) ? ranks_[at - 1] : QString(),
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    beginInsertRows({}, at, at);
    Row r{}; r.cell = laneAt(at); r.type = Media; r.param = aspectParam;
    fillMediaMeta(r, json);   // dims/video/aspect-param from the descriptor (single source)
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, json);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    indexInsert(static_cast<size_t>(at), estimatedHeight(r, laneWidthForInsert(at, r.cell)));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), attrsJson(r.type, r.level, r.lang, r.spans, r.taskState, r.cell, r.ratios, r.header, r.table), json);

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return at;
  });
}

// toRef converts an absolute referenced path to a portable {vol,rel} ref when it
// falls under a configured volume; relative ".minnotes/…" (sidecar) and http(s)
// srcs match no volume root and pass through unchanged, so it's safe to apply to
// every descriptor builder uniformly.
static QString mediaJson(const MediaStore::ImageRef& ref) {
    return MediaStore::imageDescriptorJson(ref);   // shared with Importer
}
static QString videoMediaJson(const MediaStore::VideoRef& ref) {
    QJsonObject o;
    o.insert(QStringLiteral("src"), mn::toRef(ref.src));
    o.insert(QStringLiteral("w"), ref.w);
    o.insert(QStringLiteral("h"), ref.h);
    o.insert(QStringLiteral("kind"), QStringLiteral("video"));
    o.insert(QStringLiteral("durMs"), ref.durationMs);
    o.insert(QStringLiteral("frames"), ref.frames);
    o.insert(QStringLiteral("fps"), ref.fps);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
static QString remoteImageJson(const QString& url) {
    return MediaStore::remoteImageDescriptorJson(url);   // shared with Importer
}
static QString pdfMediaJson(const MediaStore::PdfRef& ref) {
    QJsonObject o;
    o.insert(QStringLiteral("src"),   mn::toRef(ref.src));
    o.insert(QStringLiteral("w"),     ref.w);
    o.insert(QStringLiteral("h"),     ref.h);
    o.insert(QStringLiteral("kind"),  QStringLiteral("pdf"));
    o.insert(QStringLiteral("pages"), ref.pages);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
static QString fileMediaJson(const QString& path) {
    const QFileInfo fi(path);
    QJsonObject o;
    o.insert(QStringLiteral("src"),  mn::toRef(fi.absoluteFilePath()));
    o.insert(QStringLiteral("kind"), QStringLiteral("file"));
    o.insert(QStringLiteral("name"), fi.fileName());
    o.insert(QStringLiteral("ext"),  fi.suffix().toLower());
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
static uint16_t aspectParam(int w, int h) {
    if (w <= 0 || h <= 0) return 56;     // 16:9 fallback (no jump until real dims)
    return static_cast<uint16_t>(std::clamp(int(100.0 * h / w + 0.5), 1, 1000));
}
static uint16_t aspectParam(const MediaStore::ImageRef& ref) {
    return aspectParam(ref.w, ref.h);
}

int BlockModel::insertImageFromUrl(int afterRow, const QString& fileUrl, bool forceCopy) {
    if (!mediaStore_) return -1;
    const MediaStore::ImageRef ref = mediaStore_->importFile(fileUrl, forceCopy);
    if (!ref.ok()) return -1;
    return insertMedia(afterRow, mediaJson(ref), aspectParam(ref));
}

int BlockModel::insertImageFromClipboard(int afterRow) {
    if (!mediaStore_) return -1;
    const MediaStore::ImageRef ref = mediaStore_->importClipboardImage();
    if (!ref.ok()) return -1;
    return insertMedia(afterRow, mediaJson(ref), aspectParam(ref));
}

int BlockModel::insertVideoFromUrl(int afterRow, const QString& fileUrl) {
    if (!mediaStore_) return -1;
    const MediaStore::VideoRef ref = mediaStore_->importVideoFile(fileUrl);
    if (!ref.ok()) return -1;
    return insertMedia(afterRow, videoMediaJson(ref), aspectParam(ref.w, ref.h));
}

int BlockModel::insertPdfFromUrl(int afterRow, const QString& fileUrl) {
    if (!mediaStore_) return -1;
    const MediaStore::PdfRef ref = mediaStore_->importPdfFile(fileUrl);
    if (!ref.ok()) return -1;
    return insertMedia(afterRow, pdfMediaJson(ref), aspectParam(ref.w, ref.h));
}

int BlockModel::insertFileFromUrl(int afterRow, const QString& fileUrl) {
    const QString path = fileUrl.startsWith(QLatin1String("file:"))
                       ? QUrl(fileUrl).toLocalFile() : fileUrl;
    if (path.isEmpty()) return -1;
    return insertMedia(afterRow, fileMediaJson(path), static_cast<uint16_t>(kFileChip));
}

int BlockModel::insertMediaFromUrl(int afterRow, const QString& fileUrl, bool forceCopy) {
    // Video / PDF (by extension + a successful probe), else a loadable image,
    // else a generic file attachment chip — so any dropped/pasted file lands.
    if (MediaStore::isVideoPath(fileUrl)) {
        const int r = insertVideoFromUrl(afterRow, fileUrl);
        if (r >= 0) return r;
    }
    if (MediaStore::isPdfPath(fileUrl)) {
        const int r = insertPdfFromUrl(afterRow, fileUrl);
        if (r >= 0) return r;
    }
    const int r = insertImageFromUrl(afterRow, fileUrl, forceCopy);
    if (r >= 0) return r;
    // Importable document formats (md/txt/csv/html/…) and .mnpkg packages
    // never become file chips: hand the path to QML (drop AND url-paste both
    // funnel here — ONE seam), which raises the import flow (or opens the
    // package). −1 = nothing inserted.
    if (!Importer::formatForPath(fileUrl).isEmpty() || mnpkg::isPackagePath(fileUrl)) {
        emit importFileRequested(fileUrl);
        return -1;
    }
    return insertFileFromUrl(afterRow, fileUrl);
}

int BlockModel::rewriteMediaSrcs(const QHash<QString, QString>& absToRel) {
    if (rows_.empty() || !mediaStore_ || absToRel.isEmpty()) return 0;
    // Match by RESOLVED local path so absolute strings and {vol,rel} refs
    // both hit; relative .minnotes srcs resolve inside the doc and miss the
    // map. (Blocking resolveUrl is fine: collect refuses package views, so
    // nothing here can trigger an extraction.)
    auto relFor = [&](const QJsonValue& src) -> QString {
        const QString url = mediaStore_->resolveUrl(src);
        if (url.isEmpty() || !url.startsWith(QLatin1String("file:"))) return {};
        const auto it = absToRel.constFind(QDir::cleanPath(QUrl(url).toLocalFile()));
        return it == absToRel.constEnd() ? QString() : it.value();
    };
    // Gather every change first: the group snapshot needs [lo,hi] up front,
    // and a no-op must not burn an undo entry.
    struct MediaEdit { int row; QString json; };
    std::vector<MediaEdit> mediaEdits;
    struct CellEdit { int row; std::vector<std::tuple<int, int, QString>> cells; };
    std::vector<CellEdit> cellEdits;
    int count = 0, lo = -1, hi = -1;
    const int n = static_cast<int>(rows_.size());
    for (int r = 0; r < n; ++r) {
        bool changed = false;
        if (rows_[r].type == Media) {
            QJsonObject root = QJsonDocument::fromJson(content_[r].toUtf8()).object();
            if (root.value(QStringLiteral("kind")).toString() == QLatin1String("sketch")) {
                QJsonArray images = root.value(QStringLiteral("images")).toArray();
                for (int i = 0; i < images.size(); ++i) {
                    QJsonObject o = images.at(i).toObject();
                    const QString rel = relFor(o.value(QStringLiteral("src")));
                    if (rel.isEmpty()) continue;
                    o.insert(QStringLiteral("src"), rel);
                    images.replace(i, o);
                    changed = true; ++count;
                }
                if (changed) root.insert(QStringLiteral("images"), images);
            } else {
                const QString rel = relFor(root.value(QStringLiteral("src")));
                if (!rel.isEmpty()) {
                    root.insert(QStringLiteral("src"), rel);
                    changed = true; ++count;
                }
            }
            if (changed)
                mediaEdits.push_back({r, QString::fromUtf8(
                    QJsonDocument(root).toJson(QJsonDocument::Compact))});
        } else if (rows_[r].type == Table) {
            const TableGrid g = TableGrid::fromJson(content_[r]);
            CellEdit ce{r, {}};
            for (int tr = 0; tr < g.rows(); ++tr)
                for (int tc = 0; tc < g.cols(); ++tc) {
                    const QString desc = g.cellMedia(tr, tc);
                    if (desc.isEmpty()) continue;
                    QJsonObject o = QJsonDocument::fromJson(desc.toUtf8()).object();
                    const QString rel = relFor(o.value(QStringLiteral("src")));
                    if (rel.isEmpty()) continue;
                    o.insert(QStringLiteral("src"), rel);
                    ce.cells.emplace_back(tr, tc, QString::fromUtf8(
                        QJsonDocument(o).toJson(QJsonDocument::Compact)));
                    ++count;
                }
            if (!ce.cells.empty()) { cellEdits.push_back(std::move(ce)); changed = true; }
        }
        if (changed) { if (lo < 0) lo = r; hi = r; }
    }
    if (count == 0) return 0;

    beginTxn(lo, hi);   // one entry; inner mutators nest
    for (const MediaEdit& me : mediaEdits) {
        content_[me.row] = me.json;
        persistContent(me.row);
        emit dataChanged(index(me.row), index(me.row), {ContentRole});
    }
    for (const CellEdit& ce : cellEdits)
        mutateTable(ce.row, [&](TableGrid& g) {
            for (const auto& [tr, tc, json] : ce.cells) g.setCellMedia(tr, tc, json);
        });
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return count;
}

// Display URL — NON-BLOCKING: packaged media not yet extracted returns ""
// while a background pull runs; completion bumps contentRevision so bound
// delegates re-resolve (their loading windows cover the wait).
QString BlockModel::mediaUrl(int row) const {
    if (rows_.empty() || !mediaStore_) return {};   // no doc / empty model → no media
    row = clampRow(row);
    if (rows_[row].type != Media) return {};
    const QJsonObject o = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    return mediaStore_->resolveUrlAsync(o.value(QStringLiteral("src")));
}
// Display local path (poster/PDF image providers) — same non-blocking rules.
QString BlockModel::mediaViewPath(int row) const {
    const QString url = mediaUrl(row);
    return url.isEmpty() ? QString() : QUrl(url).toLocalFile();
}
// True while row's packaged media is mid-extraction ("loading…", not
// "unavailable"). Bound with a contentRevision dep so it re-polls.
bool BlockModel::mediaExtracting(int row) const {
    if (rows_.empty() || !mediaStore_) return false;
    row = clampRow(row);
    if (rows_[row].type != Media) return false;
    const QJsonObject o = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    return mediaStore_->extractionPending(o.value(QStringLiteral("src")).toString());
}
QString BlockModel::mediaPlaybackSource(int row) const {
    if (rows_.empty() || !mediaStore_) return {};
    row = clampRow(row);
    if (rows_[row].type != Media) return {};
    const QJsonObject o = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    return mediaStore_->playbackSourceFor(o.value(QStringLiteral("src")));
}
QString BlockModel::mediaAnchorPath(int row) const {
    if (rows_.empty() || !mediaStore_) return {};
    row = clampRow(row);
    if (rows_[row].type != Media) return {};
    const QJsonObject o = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    return mediaStore_->anchorPathFor(o.value(QStringLiteral("src")));
}
// File-op local path — BLOCKING: reveal/export get a real file (packaged
// media extracts inline; the explicit-action cost).
QString BlockModel::mediaLocalPath(int row) const {
    if (rows_.empty() || !mediaStore_) return {};
    row = clampRow(row);
    if (rows_[row].type != Media) return {};
    const QJsonObject o = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const QString url = mediaStore_->resolveUrl(o.value(QStringLiteral("src")));
    return url.isEmpty() ? QString() : QUrl(url).toLocalFile();
}
void BlockModel::refreshMedia() {
    ++contentRevision_;
    emit contentChangedSpike();
}
void BlockModel::revealMedia(int row) const {
    const QString path = mediaLocalPath(row);
    if (path.isEmpty() || !QFileInfo::exists(path)) return;
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), { QStringLiteral("-R"), path });
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"),
                            { QStringLiteral("/select,") + QDir::toNativeSeparators(path) });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}
void BlockModel::revealMediaFolder() const {
    if (!mediaStore_) return;
    // Package views: the store anchors to the extraction scratch — show the
    // .mnpkg itself instead. No .minnotes yet (nothing pasted/collected) →
    // select the document, so the user still lands in the right folder.
    QString dir = QDir::cleanPath(mediaStore_->docDir()) + QStringLiteral("/.minnotes");
    const bool pkg = !mediaStore_->packageSource().isEmpty();
    if (pkg || !QFileInfo::exists(dir)) {
        if (!QFileInfo::exists(docPath_)) return;
#if defined(Q_OS_MACOS)
        QProcess::startDetached(QStringLiteral("open"), { QStringLiteral("-R"), docPath_ });
#elif defined(Q_OS_WIN)
        QProcess::startDetached(QStringLiteral("explorer"),
                                { QStringLiteral("/select,") + QDir::toNativeSeparators(docPath_) });
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(docPath_).absolutePath()));
#endif
        return;
    }
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), { dir });
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"), { QDir::toNativeSeparators(dir) });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
#endif
}
QString BlockModel::mediaFileName(int row) const {
    row = clampRow(row);
    if (rowAt(row).type != Media) return {};
    const QJsonObject o = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const QString name = o.value(QStringLiteral("name")).toString();
    if (!name.isEmpty()) return name;
    // Derive from the descriptor src as a pure STRING op — this is a display
    // binding (tab labels, file chips) evaluated at delegate build, and the
    // old mediaLocalPath fallback block-extracted packaged media just to
    // read its basename (the frozen-package-open bug, caught by `sample`).
    const QJsonValue src = o.value(QStringLiteral("src"));
    if (src.isObject())
        return QFileInfo(src.toObject().value(QStringLiteral("rel")).toString()).fileName();
    return QFileInfo(src.toString()).fileName();
}
int BlockModel::mediaPdfPages(int row) const {
    row = clampRow(row);
    if (rowAt(row).type != Media) return 0;
    return QJsonDocument::fromJson(content_[row].toUtf8())
               .object().value(QStringLiteral("pages")).toInt();
}

QString BlockModel::pdfPageInk(int row, int page) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isPdf) return {};
    const QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const QJsonValue pageV = root.value(QStringLiteral("ink"))
                                 .toObject().value(QString::number(page));
    if (!pageV.isObject()) return {};
    return QString::fromUtf8(QJsonDocument(pageV.toObject()).toJson(QJsonDocument::Compact));
}

// Write one page's envelope back into the block, applying the shared drop
// rules: a page with no shapes AND no texts loses its key; an empty "ink"
// map leaves the root. One txn (no coalesce — one undo step per gesture,
// sketch ruling 2026-06-11).
void BlockModel::writePdfPageObject(int row, int page, QJsonObject pageObj) {
    QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    QJsonObject ink = root.value(QStringLiteral("ink")).toObject();
    const bool empty = pageObj.value(QStringLiteral("shapes")).toArray().isEmpty()
                    && pageObj.value(QStringLiteral("texts")).toArray().isEmpty();
    if (empty) {
        ink.remove(QString::number(page));
    } else {
        // Pages born from a chip-only edit still carry the full envelope.
        if (!pageObj.contains(QStringLiteral("version")))
            pageObj.insert(QStringLiteral("version"), QStringLiteral("2.0"));
        if (!pageObj.contains(QStringLiteral("coordinate_system")))
            pageObj.insert(QStringLiteral("coordinate_system"), QStringLiteral("normalized"));
        ink.insert(QString::number(page), pageObj);
    }
    if (ink.isEmpty()) root.remove(QStringLiteral("ink"));
    else root.insert(QStringLiteral("ink"), ink);

    beginTxn(row, row);
    content_[row] = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::pdfSetPageInk(int row, int page, const QString& strokesJson) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isPdf) return;
    const QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const QJsonObject prior = root.value(QStringLiteral("ink")).toObject()
                                  .value(QString::number(page)).toObject();
    const QJsonObject in = QJsonDocument::fromJson(strokesJson.toUtf8()).object();
    const QJsonArray shapes = in.value(QStringLiteral("shapes")).toArray();
    QJsonObject env;
    env.insert(QStringLiteral("version"),
               in.value(QStringLiteral("version")).toString(QStringLiteral("2.0")));
    env.insert(QStringLiteral("coordinate_system"),
               in.value(QStringLiteral("coordinate_system")).toString(QStringLiteral("normalized")));
    if (!shapes.isEmpty()) env.insert(QStringLiteral("shapes"), shapes);
    // Preserve the page's text chips: the canvas's edited() carries STROKES
    // only, so rebuilding the envelope from it would erase texts (the
    // 2026-08-20 chips blocker). writePdfPageObject drops the page only
    // when both arrays are empty.
    const QJsonArray texts = prior.value(QStringLiteral("texts")).toArray();
    if (!texts.isEmpty()) env.insert(QStringLiteral("texts"), texts);
    writePdfPageObject(row, page, env);
}

// --- PDF page text chips (2026-08-20): the sketch text contract applied to
// one page's envelope ("ink" → page → "texts"). Same element schema
// {x,y,w,text,size,color} — x/y/w normalized to the page, size in SOURCE px
// (PDF points), height never stored. Same rules: 2em width floor, blank
// text deletes, unchanged text is a no-op; every call = one undo step.
int BlockModel::pdfAddPageText(int row, int page, qreal x, qreal y, qreal w,
                               const QString& text, qreal size,
                               const QString& colorHex) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isPdf) return -1;
    if (page < 0 || text.trimmed().isEmpty() || size <= 0) return -1;
    const QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const double srcW = root.value(QStringLiteral("w")).toInt(612);
    QJsonObject pageObj = root.value(QStringLiteral("ink")).toObject()
                              .value(QString::number(page)).toObject();
    QJsonObject t;
    t.insert(QStringLiteral("text"), text);
    t.insert(QStringLiteral("x"), x);
    t.insert(QStringLiteral("y"), y);
    t.insert(QStringLiteral("w"), std::max(w, (2.0 * size) / srcW));   // 2em floor
    t.insert(QStringLiteral("size"), size);
    t.insert(QStringLiteral("color"), colorHex);
    QJsonArray texts = pageObj.value(QStringLiteral("texts")).toArray();
    texts.append(t);
    pageObj.insert(QStringLiteral("texts"), texts);
    writePdfPageObject(row, page, pageObj);
    return texts.size() - 1;
}

void BlockModel::pdfSetPageText(int row, int page, int idx, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isPdf) return;
    QJsonObject pageObj = QJsonDocument::fromJson(content_[row].toUtf8()).object()
                              .value(QStringLiteral("ink")).toObject()
                              .value(QString::number(page)).toObject();
    QJsonArray texts = pageObj.value(QStringLiteral("texts")).toArray();
    if (idx < 0 || idx >= texts.size()) return;
    QJsonObject o = texts.at(idx).toObject();
    if (o.value(QStringLiteral("text")).toString() == text) return;   // no txn
    if (text.trimmed().isEmpty()) {
        texts.removeAt(idx);            // blank commit = delete (overlay contract)
    } else {
        o.insert(QStringLiteral("text"), text);
        texts.replace(idx, o);
    }
    pageObj.insert(QStringLiteral("texts"), texts);
    writePdfPageObject(row, page, pageObj);
}

void BlockModel::pdfSetPageTextBox(int row, int page, int idx, qreal x, qreal y,
                                   qreal w, qreal size) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isPdf) return;
    if (size <= 0) return;
    const QJsonObject root = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    const double srcW = root.value(QStringLiteral("w")).toInt(612);
    QJsonObject pageObj = root.value(QStringLiteral("ink")).toObject()
                              .value(QString::number(page)).toObject();
    QJsonArray texts = pageObj.value(QStringLiteral("texts")).toArray();
    if (idx < 0 || idx >= texts.size()) return;
    QJsonObject o = texts.at(idx).toObject();
    o.insert(QStringLiteral("x"), x);
    o.insert(QStringLiteral("y"), y);
    o.insert(QStringLiteral("w"), std::max(w, (2.0 * size) / srcW));
    o.insert(QStringLiteral("size"), size);
    texts.replace(idx, o);
    pageObj.insert(QStringLiteral("texts"), texts);
    writePdfPageObject(row, page, pageObj);
}

void BlockModel::pdfRemovePageText(int row, int page, int idx) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !rows_[row].isPdf) return;
    QJsonObject pageObj = QJsonDocument::fromJson(content_[row].toUtf8()).object()
                              .value(QStringLiteral("ink")).toObject()
                              .value(QString::number(page)).toObject();
    QJsonArray texts = pageObj.value(QStringLiteral("texts")).toArray();
    if (idx < 0 || idx >= texts.size()) return;
    texts.removeAt(idx);
    pageObj.insert(QStringLiteral("texts"), texts);
    writePdfPageObject(row, page, pageObj);
}
// Open the media's file in the sibling ufb browser via its deep-link scheme:
// ufb:///{os}/{percent-encoded path} (slashes kept literal, matching ufb's
// build_path_uri). No-op if ufb isn't installed (no handler registered).
void BlockModel::openMediaInUfb(int row) const {
    const QString path = mediaLocalPath(row);
    if (path.isEmpty()) return;
#if defined(Q_OS_WIN)
    const QString os = QStringLiteral("win");
#else
    const QString os = QStringLiteral("mac");
#endif
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(path, "/"));
    QDesktopServices::openUrl(QUrl(QStringLiteral("ufb:///") + os + QLatin1Char('/') + enc));
}
int BlockModel::mediaW(int row) const {
    row = clampRow(row);
    if (rowAt(row).type != Media) return 0;
    return QJsonDocument::fromJson(content_[row].toUtf8()).object().value(QStringLiteral("w")).toInt();
}
int BlockModel::mediaH(int row) const {
    row = clampRow(row);
    if (rowAt(row).type != Media) return 0;
    return QJsonDocument::fromJson(content_[row].toUtf8()).object().value(QStringLiteral("h")).toInt();
}
QString BlockModel::mediaKind(int row) const {
    row = clampRow(row);
    if (rowAt(row).type != Media) return {};
    const QString k = QJsonDocument::fromJson(content_[row].toUtf8())
                          .object().value(QStringLiteral("kind")).toString();
    return k.isEmpty() ? QStringLiteral("image") : k;   // legacy {src,w,h} = image
}
double BlockModel::mediaFps(int row) const {
    row = clampRow(row);
    if (rowAt(row).type != Media) return 0.0;
    return QJsonDocument::fromJson(content_[row].toUtf8()).object().value(QStringLiteral("fps")).toDouble();
}
int BlockModel::mediaFrames(int row) const {
    row = clampRow(row);
    if (rowAt(row).type != Media) return 0;
    return QJsonDocument::fromJson(content_[row].toUtf8()).object().value(QStringLiteral("frames")).toInt();
}
qreal BlockModel::mediaDurationMs(int row) const {
    row = clampRow(row);
    if (rowAt(row).type != Media) return 0;
    return QJsonDocument::fromJson(content_[row].toUtf8()).object().value(QStringLiteral("durMs")).toDouble();
}

void BlockModel::clearFormat(int row, int start, int end) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || isOpaqueRow(row)) return;
    std::vector<Span> spans = rows_[row].spans;
    if (!mn::inl::clearFormat(content_[row], spans, start, end)) return;   // comments + chips survive
    commitRowSpans(row, std::move(spans));
}

int BlockModel::applyMarkdownTrigger(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return 0;
    // List item → task item: the bare "- " rule fires on the first space, so by the
    // time "[ ] " is typed the block is already a ListItem. Promote it here. ("[/] "
    // in-progress, "[x] " done.)
    if (rows_[row].type == ListItem) {
        const QString& c = content_[row];
        int st = -1, tstrip = 4;   // "[ ] "/"[/] "/"[x] " are all 4 chars
        if (c.startsWith(QLatin1String("[ ] ")))      st = TaskTodo;
        else if (c.startsWith(QLatin1String("[/] "))) st = TaskDoing;
        else if (c.startsWith(QLatin1String("[x] ")) || c.startsWith(QLatin1String("[X] "))) st = TaskDone;
        if (st < 0) return 0;
        beginTxn(row, row);
        rows_[row].type = static_cast<uint8_t>(TaskListItem);
        rows_[row].taskState = static_cast<uint8_t>(st);
        content_[row] = content_[row].mid(tstrip);
        persistContent(row);
        persistMeta(row);
        emit dataChanged(index(row), index(row), {TypeRole, ContentRole});
        bumpLayout();
        ++contentRevision_;
        emit contentChangedSpike();
        endTxn();
        return tstrip;
    }
    if (rows_[row].type != Paragraph) return 0;   // only transform plain paragraphs
    BlockType t = Paragraph; int level = 0, strip = 0;
    if (!matchMarkdownPrefix(content_[row], t, level, strip)) return 0;

    beginTxn(row, row);
    rows_[row].type = static_cast<uint8_t>(t);
    if (t == TaskListItem) {                  // `level` carried the initial task state
        rows_[row].taskState = static_cast<uint8_t>(level);
        rows_[row].level = 0;
    } else {
        rows_[row].level = static_cast<uint8_t>(level);
    }
    content_[row] = content_[row].mid(strip);
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole, ContentRole});
    bumpLayout();                 // type change → height changes; delegate re-measures
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return strip;
}

bool BlockModel::makeDividerIfMarker(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    if (rows_[row].type != Paragraph) return false;
    const QString c = content_[row].trimmed();
    if (c != QLatin1String("---") && c != QLatin1String("***") && c != QLatin1String("___"))
        return false;
    beginTxn(row, row);
    rows_[row].type = Divider;
    rows_[row].level = 0;
    content_[row].clear();
    rows_[row].spans.clear();
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole, ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return true;
}

QString BlockModel::genBase(int row, const Row& r) const {
    static const char* kWords[] = {
        "block", "virtual", "scroll", "height", "index", "fenwick", "delegate",
        "reflow", "viewport", "markdown", "render", "cache", "ring", "buffer",
        "cursor", "selection", "offset", "settle", "estimate", "measure" };
    constexpr int kN = int(sizeof(kWords) / sizeof(kWords[0]));

    switch (r.type) {
    case Heading:
        return QStringLiteral("Section %1 — %2 %3")
            .arg(row).arg(kWords[rowHash(row) % kN]).arg(kWords[rowHash(row + 7) % kN]);
    case Media:
        return QStringLiteral("media #%1  (aspect %2)").arg(row).arg(r.param / 100.0, 0, 'f', 2);
    case Code:
    case Paragraph:
    default: {
        QString s;
        const int lines = std::max<int>(1, r.param);
        for (int l = 0; l < lines; ++l) {
            const uint32_t seed = rowHash(row * 131 + l);
            const int wc = 6 + (seed % 9);
            for (int w = 0; w < wc; ++w)
                s += QString::fromLatin1(kWords[(seed + w * 2654435761u) % kN]) % u' ';
            if (l + 1 < lines) s += u'\n';
        }
        if (r.type == Code) s = QStringLiteral("fn block_%1() {\n").arg(row) % s % u"\n}";
        else s = QStringLiteral("[%1]  ").arg(row) % s;   // number paragraphs for legible caret tracking
        return s.trimmed();
    }
    }
}

QString BlockModel::textAt(int row) const {
    return (row >= 0 && row < static_cast<int>(content_.size())) ? content_[row] : QString();
}

QString BlockModel::contentForRow(int row) const {
    if (rows_.empty()) return {};
    return textAt(clampRow(row));
}

// --- Semantic format spans --------------------------------------------------
// The span interval + offset rules live in the inline text engine (InlineText.h);
// BlockModel's static span helpers forward there.

QVariantList BlockModel::spansForRow(int row) const {
    if (rows_.empty()) return {};
    return mn::inl::spansToVariantList(rowAt(row).spans);
}

// A payload span (link/color/highlight) over [start,end): drop any same-kind span
// overlapping the range (they don't merge — each carries its own URL/color), then
// add the new one if a payload was given (empty = remove that kind here).
void BlockModel::setPayloadSpan(int row, int start, int end, uint8_t kind,
                                const QString& payload, const QString& coalesce) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || isOpaqueRow(row)) return;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end) return;
    beginTxn(row, row);
    std::vector<Span>& v = rows_[row].spans;
    std::vector<Span> kept;
    for (const Span& sp : v)
        if (!(sp.kind == kind && sp.s < end && sp.e > start)) kept.push_back(sp);
    if (!payload.isEmpty()) kept.push_back({start, end, kind, payload});
    v = std::move(kept);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn(coalesce);
}
void BlockModel::setLink(int row, int start, int end, const QString& url) {
    setPayloadSpan(row, start, end, SpanLink, url);
}
void BlockModel::setTextColor(int row, int start, int end, const QString& color,
                              const QString& coalesce) {
    setPayloadSpan(row, start, end, SpanFgColor, color, coalesce);
}
void BlockModel::setHighlight(int row, int start, int end, const QString& color,
                              const QString& coalesce) {
    setPayloadSpan(row, start, end, SpanHighlight, color, coalesce);
}

// --- DT-2: inline choice chips (2026-08-20) -----------------------------
// Payload = {"o":[{"id","l","c"}...],"v":selectedId} (the table option
// shape, short keys); the span's TEXT is the selected LABEL — exporters
// flatten to the selected value with no cases, copy-as-text and search
// just work. Every mutator = one txn.

// Labels are stored as BLOCK TEXT, and convertMarkdown scans raw text with
// no span awareness (load canonicalization + commitMarkdown) — an
// unescapable marker char in a label would be eaten and shift the chip.
// Strip the markdown-active set (inline-only rule; table options never
// meet convertMarkdown).
// The chip label / payload rules live in the inline text engine (InlineText.h).
using mn::inl::sanitizeChoiceLabel;
using mn::inl::choiceLabelFor;
using mn::inl::choiceColorFor;
// The default set (ruling 2026-08-20): the app's tri-state culture. "v"
// preselects the first option; the label to insert is choiceLabelFor(v).
static QJsonObject defaultChoicePayload() {
    QJsonArray opts;
    const char* defs[3][2] = { {"To do", "#8A8A8A"},
                               {"Doing", "#0189F1"},
                               {"Done",  "#58A65C"} };
    QString firstId;
    for (int i = 0; i < 3; ++i) {
        QJsonObject o;
        const QString id = makeUlid();
        if (i == 0) firstId = id;
        o.insert(QStringLiteral("id"), id);
        o.insert(QStringLiteral("l"), QLatin1String(defs[i][0]));
        o.insert(QStringLiteral("c"), QLatin1String(defs[i][1]));
        opts.append(o);
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("o"), opts);
    payload.insert(QStringLiteral("v"), firstId);
    return payload;
}

// The chip span whose range starts at `spanStart` (the span address the
// picker holds), or nullptr.
BlockModel::Span* BlockModel::choiceSpanAt(int row, int spanStart) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
    for (Span& sp : rows_[row].spans)
        if (sp.kind == SpanChoice && sp.s == spanStart) return &sp;
    return nullptr;
}

int BlockModel::insertChoiceAt(int row, int col) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return -1;
    const uint8_t t = rows_[row].type;
    if (t == Media || t == Table || t == Divider || t == Code) return -1;
    QString text = content_[row];
    std::vector<Span> spans = rows_[row].spans;
    const int at = mn::inl::insertChoice(text, spans, col, defaultChoicePayload());
    commitRowTextAndSpans(row, std::move(text), std::move(spans));
    return at;
}

void BlockModel::commitRowTextAndSpans(int row, QString&& text, std::vector<Span>&& spans) {
    beginTxn(row, row);
    content_[row] = std::move(text);
    rows_[row].spans = std::move(spans);
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

bool BlockModel::editRowChoice(int row, int spanStart, const mn::inl::ChoiceEdit& edit) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    QString text = content_[row];
    std::vector<Span> spans = rows_[row].spans;
    if (!mn::inl::editChoice(text, spans, spanStart, edit)) return false;
    commitRowTextAndSpans(row, std::move(text), std::move(spans));
    return true;
}

bool BlockModel::editCellChoice(int row, int r, int c, int spanStart, const mn::inl::ChoiceEdit& edit) {
    bool applied = false;
    mutateCellInline(row, r, c, [&](QString& t, std::vector<Span>& v) {
        applied = mn::inl::editChoice(t, v, spanStart, edit);
        return applied;
    });
    return applied;
}

QString BlockModel::choiceAt(int row, int col) const {
    if (rows_.empty()) return {};
    const Span* sp = mn::inl::choiceAt(rowAt(row).spans, col);
    return sp ? sp->href : QString();
}
QVariantList BlockModel::choiceRangeAt(int row, int col) const {
    if (rows_.empty()) return {};
    const Span* sp = mn::inl::choiceAt(rowAt(row).spans, col);
    return sp ? QVariantList{ sp->s, sp->e } : QVariantList();
}

// Swap the selected option: replace the label TEXT and the payload's "v" in
// ONE txn. Other spans on the row shift as if [s,e) was retyped.
void BlockModel::setChoiceSelected(int row, int spanStart, const QString& optionId) {
    editRowChoice(row, spanStart, [&](QJsonObject& payload, QString& label) {
        return mn::inl::selectOption(payload, optionId, label);
    });
}

// Shared tail for label/payload rewrites: swap [s,e) text for `label`,
// shift every OTHER span, restore the chip span with its new bounds.
void BlockModel::replaceChoiceText(int row, int spanStart, const QString& label,
                                   const QJsonObject& payload) {
    editRowChoice(row, spanStart, [&](QJsonObject& p, QString& shown) {
        p = payload;
        shown = label;
        return true;
    });
}

QString BlockModel::choiceAddOption(int row, int spanStart, const QString& label,
                                    const QString& colorHex) {
    const QString id = makeUlid();
    const bool applied = editRowChoice(row, spanStart, [&](QJsonObject& payload, QString& shown) {
        mn::inl::addOption(payload, id, label, colorHex, shown);
        return true;
    });
    return applied ? id : QString();
}

// The options-editor commit: preserve ids, mint empty ones, sanitize labels;
// a deleted selected id falls back to the FIRST option (text follows —
// the text==label invariant); an empty set removes the chip.
void BlockModel::setChoiceOptions(int row, int spanStart, const QVariantList& options) {
    if (options.isEmpty()) { removeChoiceAt(row, spanStart); return; }
    editRowChoice(row, spanStart, [&](QJsonObject& payload, QString& label) {
        return mn::inl::setOptions(payload, options, [] { return makeUlid(); }, label);
    });
}

void BlockModel::removeChoiceAt(int row, int spanStart) {
    Span* sp = choiceSpanAt(row, spanStart);
    if (!sp) return;
    deleteRange(row, sp->s, row, sp->e);   // full cover — the span dies with it
}

QVariantList BlockModel::choiceRangesForRow(int row) const {
    if (rows_.empty()) return {};
    return mn::inl::choiceRanges(rows_[clampRow(row)].spans);
}

// --- Cell chips (2026-08-21): the same DT-2 chip inside a table TEXT cell.
// The chip span rides the cell's span list (cellSpans JSON) and every rule
// carries over: text == label, payload = {"o":[...],"v":id}, spanStart is
// the address. Typed (choice/check) BODY cells refuse — they render a
// widget, not text; headers of typed columns are still text and accept.
// Each op is one mutateTable (one undo entry). Exports need nothing: cell
// span emitters already pass unknown kinds through as plain label text.

int BlockModel::tableInsertChoiceAt(int row, int r, int c, int col) {
    if (row < 0 || row >= static_cast<int>(rows_.size())
        || rows_[row].type != Table || r < 0 || c < 0) return -1;
    if (tableColumnKind(row, c) != 0 && r >= tableHeaderRows(row)) return -1;
    if (r >= tableRows(row) || c >= tableColumns(row)) return -1;
    int out = -1;
    const QJsonObject payload = defaultChoicePayload();
    mutateCellInline(row, r, c, [&](QString& t, std::vector<Span>& v) {
        out = mn::inl::insertChoice(t, v, col, payload);
        return true;
    });
    return out;
}

QString BlockModel::tableChoiceAt(int row, int r, int c, int col) const {
    if (rows_.empty() || rowAt(row).type != Table) return {};
    const std::vector<Span> v = cellSpansFromJson(gridFor(row).cellSpans(r, c));
    const Span* sp = mn::inl::choiceAt(v, col);
    return sp ? sp->href : QString();
}

QVariantList BlockModel::tableChoiceRangeAt(int row, int r, int c, int col) const {
    if (rows_.empty() || rowAt(row).type != Table) return {};
    const std::vector<Span> v = cellSpansFromJson(gridFor(row).cellSpans(r, c));
    const Span* sp = mn::inl::choiceAt(v, col);
    return sp ? QVariantList{ sp->s, sp->e } : QVariantList();
}

void BlockModel::tableSetChoiceSelected(int row, int r, int c, int spanStart,
                                        const QString& optionId) {
    editCellChoice(row, r, c, spanStart, [&](QJsonObject& payload, QString& label) {
        return mn::inl::selectOption(payload, optionId, label);
    });
}

QString BlockModel::tableChoiceAddOption(int row, int r, int c, int spanStart,
                                         const QString& label, const QString& colorHex) {
    const QString id = makeUlid();
    const bool applied = editCellChoice(row, r, c, spanStart, [&](QJsonObject& payload, QString& shown) {
        mn::inl::addOption(payload, id, label, colorHex, shown);
        return true;
    });
    return applied ? id : QString();
}

void BlockModel::tableSetChoiceOptions(int row, int r, int c, int spanStart,
                                       const QVariantList& options) {
    if (options.isEmpty()) { tableRemoveChoiceAt(row, r, c, spanStart); return; }
    editCellChoice(row, r, c, spanStart, [&](QJsonObject& payload, QString& label) {
        return mn::inl::setOptions(payload, options, [] { return makeUlid(); }, label);
    });
}

void BlockModel::tableRemoveChoiceAt(int row, int r, int c, int spanStart) {
    // Its own txn (no coalesce key): a removal never merges into a typing run's undo
    // entry. Full-cover delete — the span dies with its label.
    mutateCellInline(row, r, c, [&](QString& t, std::vector<Span>& v) {
        const Span* sp = mn::inl::choiceAt(v, spanStart);
        if (!sp) return false;
        const int s = sp->s, e = sp->e;
        return mn::inl::deleteRange(t, v, s, e);
    });
}

QVariantList BlockModel::tableChoiceRangesForCell(int row, int r, int c) const {
    if (rows_.empty() || rowAt(row).type != Table) return {};
    return mn::inl::choiceRanges(cellSpansFromJson(gridFor(row).cellSpans(r, c)));
}

QString BlockModel::linkAt(int row, int col) const {
    if (rows_.empty()) return {};
    for (const Span& sp : rowAt(row).spans)
        if (sp.kind == SpanLink && col >= sp.s && col < sp.e) return sp.href;
    return {};
}

// --- Comments (tier 3 annotations) -------------------------------------
QString BlockModel::addComment(int row, int start, int end) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || !doc_.isOpen()) return {};
    if (isOpaqueRow(row)) return {};
    const int len = content_[row].size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end) return {};
    const QString id = makeUlid();
    doc_.createThread(id);                                 // NOT undoable, by design
    setPayloadSpan(row, start, end, SpanComment, id);      // undoable txn (the link pattern)
    ++commentsRevision_;
    emit commentsChanged();
    return id;
}

QString BlockModel::commentAt(int row, int col) const {
    if (rows_.empty()) return {};
    for (const Span& sp : rowAt(row).spans)
        if (sp.kind == SpanComment && col >= sp.s && col < sp.e) return sp.href;
    return {};
}

QVariantList BlockModel::commentRangesForRow(int row) const {
    QVariantList out;
    if (rows_.empty()) return out;
    row = clampRow(row);
    // Resolved-ness rides along (2026-08-21): the page chip dims for a
    // settled conversation. One thread fetch per call — the callers are
    // already gated on commentsRevision, and commented rows are few.
    QHash<QString, bool> resolvedById;
    if (doc_.isOpen())
        for (const auto& t : doc_.commentThreads())
            resolvedById.insert(t.id, t.resolved);
    for (const Span& sp : rows_[row].spans) {
        if (sp.kind != SpanComment || sp.e <= sp.s) continue;
        QVariantMap m;
        m.insert(QStringLiteral("s"), sp.s);
        m.insert(QStringLiteral("e"), sp.e);
        m.insert(QStringLiteral("id"), sp.href);
        m.insert(QStringLiteral("resolved"), resolvedById.value(sp.href, false));
        out.append(m);
    }
    return out;
}

QVariantList BlockModel::commentPinRows() const {
    QVariantList out;
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
        for (const Span& sp : rows_[i].spans)
            if (sp.kind == SpanComment && sp.e > sp.s) { out.append(i); break; }
    return out;
}

int BlockModel::threadAnchorRow(const QString& threadId) const {
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
        for (const Span& sp : rows_[i].spans)
            if (sp.kind == SpanComment && sp.href == threadId) return i;
    return -1;
}

void BlockModel::unlinkThread(const QString& threadId) {
    // Collect the rows carrying this thread's span(s), then remove them in
    // one grouped, undoable step (undo re-links the thread).
    std::vector<int> rows;
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
        for (const Span& sp : rows_[i].spans)
            if (sp.kind == SpanComment && sp.href == threadId) { rows.push_back(i); break; }
    if (rows.empty()) return;
    beginTxn(rows.front(), rows.back());
    for (int r : rows) {
        auto& spans = rows_[r].spans;
        spans.erase(std::remove_if(spans.begin(), spans.end(),
                                   [&](const Span& sp) {
                                       return sp.kind == SpanComment && sp.href == threadId;
                                   }),
                    spans.end());
        persistMeta(r);
        emit dataChanged(index(r), index(r), {});
    }
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

QVariantList BlockModel::commentThreads() const {
    QVariantList out;
    if (!doc_.isOpen()) return out;
    // DOCUMENT order, not creation order: anchored threads sort by (row, span
    // start) — reading top to bottom matches the panel top to bottom — with
    // orphaned ("Unanchored") threads grouped last, oldest first.
    struct Entry { QVariantMap m; int row; int col; qint64 created; };
    std::vector<Entry> entries;
    const auto threads = doc_.commentThreads();
    entries.reserve(threads.size());
    for (const auto& t : threads) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), t.id);
        m.insert(QStringLiteral("resolved"), t.resolved);
        m.insert(QStringLiteral("created"), t.created);
        const int row = threadAnchorRow(t.id);
        m.insert(QStringLiteral("row"), row);              // -1 = orphaned ("Unanchored")
        QString excerpt;
        int col = 0;
        if (row >= 0) {
            for (const Span& sp : rows_[row].spans)
                if (sp.kind == SpanComment && sp.href == t.id) {
                    excerpt = content_[row].mid(sp.s, std::min(60, sp.e - sp.s));
                    col = sp.s;
                    break;
                }
        }
        m.insert(QStringLiteral("excerpt"), excerpt);
        entries.push_back({std::move(m), row, col, t.created});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        const bool ao = a.row < 0, bo = b.row < 0;
        if (ao != bo) return bo;                     // anchored before orphaned
        if (ao) return a.created < b.created;        // orphans: oldest first
        if (a.row != b.row) return a.row < b.row;
        return a.col < b.col;
    });
    for (auto& e : entries) out.append(std::move(e.m));
    return out;
}

QVariantList BlockModel::commentMessages(const QString& threadId) const {
    QVariantList out;
    if (!doc_.isOpen()) return out;
    for (const auto& msg : doc_.commentMessages(threadId)) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), msg.id);
        m.insert(QStringLiteral("body"), msg.body);
        m.insert(QStringLiteral("created"), msg.created);
        m.insert(QStringLiteral("modified"), msg.modified);
        out.append(m);
    }
    return out;
}

void BlockModel::addCommentMessage(const QString& threadId, const QString& body) {
    if (!doc_.isOpen() || body.isEmpty()) return;
    doc_.insertMessage(makeUlid(), threadId, body);
    ++commentsRevision_; emit commentsChanged();
    markDirty();
}

void BlockModel::updateCommentMessage(const QString& msgId, const QString& body) {
    if (!doc_.isOpen()) return;
    doc_.updateMessage(msgId, body);
    ++commentsRevision_; emit commentsChanged();
    markDirty();
}

void BlockModel::removeCommentMessage(const QString& msgId) {
    if (!doc_.isOpen()) return;
    doc_.deleteMessage(msgId);
    ++commentsRevision_; emit commentsChanged();
    markDirty();
}

void BlockModel::setThreadResolved(const QString& threadId, bool resolved) {
    if (!doc_.isOpen()) return;
    doc_.setThreadResolved(threadId, resolved);
    ++commentsRevision_; emit commentsChanged();
    markDirty();
}

void BlockModel::deleteThread(const QString& threadId) {
    if (!doc_.isOpen()) return;
    unlinkThread(threadId);            // span removal is undoable...
    doc_.deleteThread(threadId);       // ...the bodies are gone for good
    ++commentsRevision_; emit commentsChanged();
    markDirty();
}

QVariantList BlockModel::linkRangeAt(int row, int col) const {
    if (rows_.empty()) return {};
    for (const Span& sp : rowAt(row).spans)
        if (sp.kind == SpanLink && col >= sp.s && col < sp.e) return QVariantList{ sp.s, sp.e };
    return {};
}

QVariantList BlockModel::codeRangesForRow(int row) const {
    QVariantList out;
    if (rows_.empty()) return out;
    row = clampRow(row);
    auto push = [&](int s, int e) {
        if (e <= s) return;
        QVariantMap m; m.insert(QStringLiteral("s"), s); m.insert(QStringLiteral("e"), e);
        out.append(m);
    };
    const QString& s = content_[row];
    // No inline-code chips inside a real code block, nor on a "```" fence line
    // (the backticks there are a code-block trigger, kept literal).
    if (rows_[row].type == Code || s.startsWith(QLatin1String("```"))) return out;
    for (int i = 0, n = s.size(); i < n; ) {                  // markdown `code` inner runs
        if (s[i] == QLatin1Char('`')) {
            const int j = s.indexOf(QLatin1Char('`'), i + 1);
            if (j > i) { push(i + 1, j); i = j + 1; continue; }
        }
        ++i;
    }
    for (const Span& sp : rows_[row].spans)                   // semantic code spans
        if (sp.kind == SpanCode) push(sp.s, sp.e);
    return out;
}

// Highlight spans for a row: [{s,e,color}]. The view draws these as overlay
// rects BELOW its selection layer (a char-format background would paint above
// the selection — the same occlusion the code chips dodge).
QVariantList BlockModel::highlightRangesForRow(int row) const {
    QVariantList out;
    if (rows_.empty()) return out;
    row = clampRow(row);
    for (const Span& sp : rows_[row].spans) {
        if (sp.kind != SpanHighlight || sp.e <= sp.s) continue;
        QVariantMap m;
        m.insert(QStringLiteral("s"), sp.s);
        m.insert(QStringLiteral("e"), sp.e);
        m.insert(QStringLiteral("color"), sp.href);   // payload hex
        out.append(m);
    }
    return out;
}

bool BlockModel::convertMarkdown(const QString& src, const std::vector<Span>& existing,
                                 QString& cleanText, std::vector<Span>& outSpans) {
    const int n = src.size();
    QString out; out.reserve(n);
    std::vector<int> map(n + 1, 0);          // old col → clean col
    std::vector<Span> found;
    auto keep = [&](int p) { map[p] = out.size(); out.append(src[p]); };
    auto drop = [&](int p) { map[p] = out.size(); };   // marker removed → maps to current clean pos

    int i = 0;
    while (i < n) {
        const QChar c = src[i];
        if (c == QLatin1Char('`')) {
            const int j = src.indexOf(QLatin1Char('`'), i + 1);
            if (j > i) {
                drop(i); const int s = out.size();
                for (int p = i + 1; p < j; ++p) keep(p);
                found.push_back({s, static_cast<int>(out.size()), SpanCode});
                drop(j); i = j + 1; continue;
            }
        } else if (c == QLatin1Char('*') && i + 1 < n && src[i + 1] == QLatin1Char('*')) {
            const int j = src.indexOf(QStringLiteral("**"), i + 2);
            if (j > i + 1) {
                drop(i); drop(i + 1); const int s = out.size();
                for (int p = i + 2; p < j; ++p) keep(p);
                found.push_back({s, static_cast<int>(out.size()), SpanBold});
                drop(j); drop(j + 1); i = j + 2; continue;
            }
        } else if (c == QLatin1Char('*')) {
            const int j = src.indexOf(QLatin1Char('*'), i + 1);
            if (j > i) {
                drop(i); const int s = out.size();
                for (int p = i + 1; p < j; ++p) keep(p);
                found.push_back({s, static_cast<int>(out.size()), SpanItalic});
                drop(j); i = j + 1; continue;
            }
        } else if (c == QLatin1Char('~') && i + 1 < n && src[i + 1] == QLatin1Char('~')) {
            const int j = src.indexOf(QStringLiteral("~~"), i + 2);
            if (j > i + 1) {
                drop(i); drop(i + 1); const int s = out.size();
                for (int p = i + 2; p < j; ++p) keep(p);
                found.push_back({s, static_cast<int>(out.size()), SpanStrike});
                drop(j); drop(j + 1); i = j + 2; continue;
            }
        } else if (c == QLatin1Char('[')) {              // [label](url) → link span
            const int j = src.indexOf(QLatin1Char(']'), i + 1);
            if (j > i && j + 1 < n && src[j + 1] == QLatin1Char('(')) {
                const int k = src.indexOf(QLatin1Char(')'), j + 2);
                if (k > j + 1) {
                    drop(i); const int s = out.size();
                    for (int p = i + 1; p < j; ++p) keep(p);          // the label
                    const QString url = src.mid(j + 2, k - (j + 2));
                    found.push_back({s, static_cast<int>(out.size()), SpanLink, url});
                    drop(j);                                          // ]
                    for (int p = j + 1; p <= k; ++p) drop(p);         // (url)
                    i = k + 1; continue;
                }
            }
        }
        keep(i); ++i;
    }
    map[n] = out.size();
    if (out == src) return false;            // no markers consumed

    std::vector<Span> merged;
    for (const Span& sp : existing) {        // pre-existing spans → clean coords
        const int s = map[std::clamp(sp.s, 0, n)], e = map[std::clamp(sp.e, 0, n)];
        if (spanHasPayload(sp.kind)) { if (e > s) merged.push_back({s, e, sp.kind, sp.href}); }
        else addSpan(merged, s, e, sp.kind);
    }
    for (const Span& sp : found) {
        if (spanHasPayload(sp.kind)) merged.push_back(sp);   // keep payload; don't merge
        else addSpan(merged, sp.s, sp.e, sp.kind);
    }
    cleanText = out;
    outSpans = merged;
    return true;
}

void BlockModel::commitMarkdown(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const uint8_t t = rows_[row].type;
    if (t != Paragraph && t != Quote && t != ListItem && t != TaskListItem) return;   // where inline md renders
    // A "```"/"```lang" fence is a code-block trigger (consumed on Enter); never
    // run inline conversion on it, which would eat the backticks into a stray span.
    if (content_[row].startsWith(QLatin1String("```"))) return;
    QString clean; std::vector<Span> spans;
    if (!convertMarkdown(content_[row], rows_[row].spans, clean, spans)) return;
    beginTxn(row, row);
    content_[row] = clean;
    rows_[row].spans = spans;
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();                            // markers gone → height may change
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();                                // distinct undo step (Cmd-Z restores the markdown)
}

void BlockModel::toggleFormat(int row, int start, int end, const QString& kind) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || isOpaqueRow(row)) return;
    std::vector<Span> spans = rows_[row].spans;
    if (!mn::inl::toggleFormat(content_[row], spans, start, end, spanKindFromString(kind))) return;
    commitRowSpans(row, std::move(spans));
}

QVariant BlockModel::data(const QModelIndex& index, int role) const {
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
    const Row& r = rows_[row];
    switch (role) {
    case TypeRole:     return r.type;
    case ContentRole:  return textAt(row);
    case HeightRole:   return layout().height(row);
    case MeasuredRole: return r.measured;
    default:           return {};
    }
}

QHash<int, QByteArray> BlockModel::roleNames() const {
    return {
        {TypeRole, "blockType"},
        {ContentRole, "blockContent"},
        {HeightRole, "blockHeight"},
        {MeasuredRole, "blockMeasured"},
    };
}

void BlockModel::bumpLayout() {
    ++layoutRevision_;
    // Row set may have changed (insert/remove/undo) — keep the width max
    // honest. Cheap linear scan; widths are a soft cache (delegates
    // re-report on their next render, exactly like heights).
    refreshMaxContentWidth();
    emit layoutChangedSpike();
}

// The horizontal sibling of the height contract: delegates report their
// natural (uncapped) width once measured; the max drives the view's
// contentWidth so the PAGE scrolls horizontally for wide blocks. Reporters:
// tables + code (delegate-measured); user-widened media publishes model-side
// via fillMediaMeta (known-geometry). Other text lives inside the measure
// and reports 0.
void BlockModel::setMeasuredWidth(int row, qreal w) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || w <= 0.0) return;
    const uint16_t v = static_cast<uint16_t>(std::clamp<qreal>(w, 0.0, 65535.0));
    if (rows_[row].measuredW == v) return;
    rows_[row].measuredW = v;
    refreshMaxContentWidth();
}

void BlockModel::refreshMaxContentWidth() {
    double m = 0.0;
    for (const Row& r : rows_) m = std::max(m, static_cast<double>(r.measuredW));
    tableGeom(-1);                                     // SR-4: tables wider than the page widen the content
    for (auto it = geoms_.cbegin(); it != geoms_.cend(); ++it) m = std::max(m, it.value().width);
    if (m != maxContentWidth_) {
        maxContentWidth_ = m;
        emit maxContentWidthChanged();
    }
}

void BlockModel::setMeasuredHeight(int row, qreal h) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || h <= 0.0) return;
    const double before = layout().height(static_cast<size_t>(row));
    const double delta = setIndexHeight(static_cast<size_t>(row), h);
    if (!rows_[row].measured) rows_[row].measured = true;
    // A lane block's delta is its split row's delta — report the TOP entry, which is
    // what the view compensates its scroll position by.
    if (delta != 0.0)
        emit heightSettled(static_cast<int>(layout().topOf(static_cast<std::size_t>(row))), delta);
    // Bump on the BLOCK's change, not the row's: in a lane that isn't the tallest the
    // row keeps its height, but the blocks below it in that lane still move.
    if (delta != 0.0 || h != before) bumpLayout();
}

qreal BlockModel::mediaDisplayHeight(int row) const {
    const Row& r = rowAt(row);
    return (r.type == Media) ? mediaFrameHeight(r, laneWidthOfRow(row)) : 0.0;
}
int BlockModel::mediaDispWidth(int row) const {
    const Row& r = rowAt(row);
    return (r.type == Media) ? int(mediaDisplayWidth(r, laneWidthOfRow(row)) + 0.5) : 0;
}
void BlockModel::setMediaWidth(int row, int w) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[row].type != Media) return;
    QJsonObject o = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    if (w <= 0) o.remove(QStringLiteral("dw"));                 // reset to intrinsic/default
    else        o.insert(QStringLiteral("dw"), std::clamp(w, 80, 65535));   // uncapped past the
                                                                // page (user ruling) — the drag
                                                                // distance is the practical cap
    const QString json = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    beginTxn(row, row);
    content_[row] = json;
    fillMediaMeta(rows_[row], json);
    rows_[row].measured = false;
    setIndexHeight(static_cast<size_t>(row), estimatedHeight(rows_[row], laneWidthOfRow(row)));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    // Coalesce a nudge-run (2026-08-21): consecutive resizes of the same
    // media block merge like a typing run — ⌘Z returns to the pre-run size
    // instead of replaying every intermediate width.
    endTxn(QStringLiteral("imgw:") + ids_[row]);
}

void BlockModel::setContentWidth(qreal w) {
    if (w <= 0.0 || std::abs(w - contentWidth_) < 0.5) return;
    contentWidth_ = w;
    // Media heights are derived from this width — re-derive them in the Fenwick.
    // (Media never measures back, so the estimate is the authoritative height.)
    bool any = false;
    const std::vector<double> lw = laneWidths();
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].type == Media) {
            // Compare the block's own height: media in a short lane moves its lane
            // siblings without changing the split row's height.
            const double h = estimatedHeight(rows_[i], lw[i]);
            if (h != layout().height(i)) { setIndexHeight(i, h); any = true; }
        }
    if (any) bumpLayout();
}

void BlockModel::setContent(int row, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    beginTxn(row, row);
    const bool hadSpans = !rows_[row].spans.empty();
    mn::inl::setText(content_[row], rows_[row].spans, text);   // spans clamp to the new text
    persistContent(row);                          // write-through to SQLite
    if (hadSpans) persistMeta(row);
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    // Height re-measure follows from the delegate's implicitHeight change.
}

void BlockModel::deleteRange(int aRow, int aCol, int fRow, int fCol) {
    int loRow = aRow, loCol = aCol, hiRow = fRow, hiCol = fCol;
    if (aRow > fRow || (aRow == fRow && aCol > fCol)) {
        loRow = fRow; loCol = fCol; hiRow = aRow; hiCol = aCol;
    }
    if (rows_.empty()) return;
    loRow = clampRow(loRow);
    hiRow = clampRow(hiRow);
    // Block-grain rule at opaque ends: an opaque LOW row can't host the merged
    // text (deleteSelectionRange removes it whole instead); an opaque HIGH row
    // is consumed whole — its descriptor is never spliced into prose.
    if (isOpaqueRow(loRow)) return;
    // Editing never merges across a lane edge (SR-0 P2): both ends must sit in the
    // same container. Two top-level ends take any split rows between them whole.
    if (rows_[loRow].cell != rows_[hiRow].cell || splitRowOf(loRow) != splitRowOf(hiRow)) return;
    beginTxn(loRow, hiRow);

    // Merge surviving head of lo block with surviving tail of hi block.
    const QString loText = textAt(loRow);
    const QString hiText = textAt(hiRow);
    const int loClip = std::min<int>(loCol, loText.size());
    const int hiClip = isOpaqueRow(hiRow) ? hiText.size() : std::min<int>(hiCol, hiText.size());
    const QString merged = loText.left(loClip) + hiText.mid(hiClip);
    content_[loRow] = merged;
    persistContent(loRow);
    emit dataChanged(index(loRow), index(loRow), {ContentRole});

    // Span bookkeeping (rows_[hiRow] still valid — the erase happens below).
    if (hiRow == loRow) {
        shiftSpansDelete(rows_[loRow].spans, loClip, hiClip);
    } else {
        std::vector<Span> kept;
        for (const Span& sp : rows_[loRow].spans)            // lo keeps [0, loClip)
            if (sp.s < loClip) kept.push_back({sp.s, std::min(sp.e, loClip), sp.kind, sp.href});
        for (const Span& sp : rows_[hiRow].spans) {          // hi tail [hiClip,*) → after loClip
            const int s = std::max(sp.s, hiClip);
            if (sp.e > s) kept.push_back({s - hiClip + loClip, sp.e - hiClip + loClip, sp.kind, sp.href});
        }
        rows_[loRow].spans = kept;
    }
    persistMeta(loRow);

    if (hiRow > loRow) {
        const int first = loRow + 1, last = hiRow, cnt = last - first + 1;
        for (int i = first; i <= last; ++i) {
            dropBlockInk(ids_[i]);   // hash sync; DB cascades via FK
            if (doc_.isOpen()) doc_.deleteBlock(ids_[i]);   // ids still valid pre-erase
        }
        beginRemoveRows({}, first, last);
        // Gather current (measured/estimated) heights of surviving rows.
        std::vector<double> hs;
        hs.reserve(rows_.size() - static_cast<size_t>(cnt));
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
            if (i < first || i > last) hs.push_back(layout().height(static_cast<size_t>(i)));
        rows_.erase(rows_.begin() + first, rows_.begin() + last + 1);
        content_.erase(content_.begin() + first, content_.begin() + last + 1);
        ids_.erase(ids_.begin() + first, ids_.begin() + last + 1);
        ranks_.erase(ranks_.begin() + first, ranks_.begin() + last + 1);
        reindex(std::move(hs));
        endRemoveRows();
        bumpLayout();
    }
    ++contentRevision_;
    emit contentChangedSpike();
    // Single-char same-block deletes (backspace/delete key) coalesce.
    endTxn((hiRow == loRow && hiClip - loClip == 1) ? QStringLiteral("del") : QString());
}

void BlockModel::insertText(int row, int col, const QString& text, int marks,
                            const QString& fgColor, const QString& bgColor) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (isOpaqueRow(row)) return;                    // never type into a descriptor
    beginTxn(row, row);
    std::vector<Span>& spans = rows_[row].spans;
    col = mn::inl::insertText(content_[row], spans, col, text);   // clamps; existing spans shift
    persistContent(row);
    // Code blocks are multi-line: keep the line-count param honest when the
    // insert carries newlines (verbatim paste; single Enter presses too).
    if (rows_[row].type == Code && text.contains(QLatin1Char('\n')))
        rows_[row].param = static_cast<uint16_t>(
            std::clamp<int>(content_[row].count(QLatin1Char('\n')) + 1, 1, 65535));
    // Armed typing attributes (marks + colour pens) span the new run.
    mn::inl::applyTypingAttributes(spans, col, col + int(text.size()), marks, fgColor, bgColor);
    if (!spans.empty()) persistMeta(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn(text.size() == 1 ? QStringLiteral("type") : QString());   // coalesce typing
}

QVariantList BlockModel::pasteText(int row, int col, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return {};

    // Normalize newlines, split into non-blank lines (blank lines are paragraph
    // separators — collapsed). Each remaining line becomes one block.
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    // A segment is one block-to-be: a non-blank line, or a whole ``` fence
    // (0.5.0): the fence's lines travel VERBATIM (blank lines kept, no
    // markdown parsing), the opener's tag is the language; an unclosed fence
    // runs to the end of the paste.
    struct Seg { QString text; bool code = false; QString lang; };
    std::vector<Seg> segs;
    const QStringList lines = t.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString tr = lines[i].trimmed();
        if (tr.startsWith(QLatin1String("```"))) {
            Seg cs; cs.code = true; cs.lang = tr.mid(3).trimmed();
            QStringList body;
            int j = i + 1;
            for (; j < lines.size(); ++j) {
                if (lines[j].trimmed() == QLatin1String("```")) break;
                body << lines[j];
            }
            cs.text = body.join(QLatin1Char('\n'));
            segs.push_back(std::move(cs));
            i = j;                                   // skip the closing fence (or the end)
            continue;
        }
        if (!tr.isEmpty()) segs.push_back({lines[i], false, QString()});
    }
    if (segs.empty()) return {};

    // Parse one segment → (type, level, lang, clean text, spans): block-prefix
    // then inline markdown; a fence segment is a Code block as-is.
    auto parseSeg = [&](const Seg& sg, uint8_t& type, uint8_t& level, uint8_t& taskState,
                        QString& outLang, QString& outText, std::vector<Span>& outSpans,
                        bool& hadPrefix) {
        const QString& seg = sg.text;
        type = Paragraph; level = 0; taskState = 0; outLang.clear();
        outText = seg; outSpans.clear(); hadPrefix = false;
        if (sg.code) { type = Code; outLang = sg.lang; hadPrefix = true; return; }
        const bool fence = false;
        QString body = seg;
        if (!fence) {
            BlockType bt; int lvl = 0, strip = 0;
            if (matchMarkdownPrefix(seg, bt, lvl, strip)) {
                hadPrefix = true;
                type = static_cast<uint8_t>(bt);
                level = (bt == Heading) ? static_cast<uint8_t>(lvl) : 0;
                taskState = (bt == TaskListItem) ? static_cast<uint8_t>(lvl) : 0;  // matchMarkdownPrefix put state in lvl
                if (bt == Divider) { outText.clear(); return; }
                body = seg.mid(strip);
            }
            QString clean; std::vector<Span> sp;
            if (convertMarkdown(body, {}, clean, sp)) { outText = clean; outSpans = sp; return; }
        }
        outText = body;
    };

    const int n = static_cast<int>(segs.size());
    // Opaque targets (media/table/divider) hold non-prose content — never write
    // text into them; leave them intact and splice everything AFTER instead.
    const bool opaque = (rows_[row].type == Media || rows_[row].type == Table
                         || rows_[row].type == Divider);

    QString left, right;
    std::vector<Span> leftS, rightS;
    if (!opaque) {
        const QString s = content_[row];
        col = std::clamp(col, 0, static_cast<int>(s.size()));
        left = s.left(col); right = s.mid(col);
        // Split the current block's spans at the caret: left stays, right (rebased
        // to 0) moves to the tail of the LAST pasted block.
        for (const Span& sp : rows_[row].spans) {
            if (sp.s < col) leftS.push_back({sp.s, std::min(sp.e, col), sp.kind, sp.href});
            if (sp.e > col) rightS.push_back({std::max(sp.s, col) - col, sp.e - col, sp.kind, sp.href});
        }
    }

    beginTxn(row, row);
    int caretRow = row, caretCol = 0;

    // Append a span, preserving the link target: addSpan() only carries a kind (it
    // merges same-kind runs), which would drop a link's href — so push links whole.
    auto appendSpan = [](std::vector<Span>& v, int s, int e, uint8_t kind, const QString& href) {
        if (s >= e) return;
        if (spanHasPayload(kind)) v.push_back({s, e, kind, href});
        else addSpan(v, s, e, kind);
    };

    // Splice segs[startSeg..] as fresh blocks after `afterRow`; the LAST one gets
    // the trailing tail (right/rightS) and the caret lands at its boundary.
    auto appendBlocksAfter = [&](int afterRow, int startSeg) {
        const int cnt = n - startSeg;
        if (cnt <= 0) return;
        const int first = afterRow + 1, last = afterRow + cnt;
        QString prevRank = ranks_[afterRow];
        const QString nextRank = (first < static_cast<int>(ranks_.size())) ? ranks_[first] : QString();
        struct NewBlk { QString id, rank, content; uint8_t type, level, taskState; QString lang; std::vector<Span> spans; int8_t cell; };
        std::vector<NewBlk> made;
        beginInsertRows({}, first, last);
        for (int k = 0; k < cnt; ++k) {
            const int at = first + k;
            uint8_t tj, lj, tsj; QString langj, textj; std::vector<Span> spj; bool pfxj;
            parseSeg(segs[static_cast<size_t>(startSeg + k)], tj, lj, tsj, langj, textj, spj, pfxj);
            QString contentj = textj;
            std::vector<Span> spans = spj;
            if (k == cnt - 1) {                        // last block carries the tail
                caretRow = at; caretCol = textj.size();
                for (const Span& sp : rightS)
                    appendSpan(spans, sp.s + contentj.size(), sp.e + contentj.size(), sp.kind, sp.href);
                contentj += right;
            }
            Row r{}; r.type = tj; r.level = lj; r.taskState = tsj; r.lang = langj; r.spans = spans;
            r.cell = laneAt(at);
            r.param = (tj == Code)
                ? static_cast<uint16_t>(std::clamp<int>(contentj.count(QLatin1Char('\n')) + 1, 1, 65535))
                : 1;
            const QString newId = makeUlid();
            const QString newRank = rankBetween(prevRank, nextRank);
            prevRank = newRank;
            rows_.insert(rows_.begin() + at, r);
            content_.insert(content_.begin() + at, contentj);
            ids_.insert(ids_.begin() + at, newId);
            ranks_.insert(ranks_.begin() + at, newRank);
            indexInsert(static_cast<size_t>(at), estimatedHeight(r, laneWidthForInsert(at, r.cell)));
            made.push_back({newId, newRank, contentj, tj, lj, tsj, langj, spans, r.cell});
        }
        endInsertRows();
        if (doc_.isOpen())
            for (const NewBlk& b : made)
                doc_.appendBlock(b.id, b.rank, 0, QString::fromLatin1(typeToString(b.type)),
                                 attrsJson(b.type, b.level, b.lang, b.spans, b.taskState, b.cell, std::vector<float>{}, 0, QString()), b.content);
    };

    // A leading fence merges only into an EMPTY row (it becomes the code
    // block); into prose it lands after the row like an opaque target.
    const bool codeFirstApart = !opaque && segs[0].code && !(left.isEmpty() && right.isEmpty());
    if (opaque || codeFirstApart) {
        right.clear(); rightS.clear();                 // the row keeps its tail
        appendBlocksAfter(row, 0);                     // the block stays untouched
    } else {
        // ---- Block 0: merge seg[0] into the current block at the caret ----
        uint8_t t0, l0, ts0; QString lang0, text0; std::vector<Span> sp0; bool pfx0;
        parseSeg(segs[0], t0, l0, ts0, lang0, text0, sp0, pfx0);
        // Adopt the parsed block type only at a clean start (nothing to the left)
        // AND when the line carried a real prefix; else keep the block's type and
        // treat seg[0] as inline-styled text.
        if (left.isEmpty() && pfx0) {
            rows_[row].type = t0; rows_[row].level = l0; rows_[row].taskState = ts0;
            rows_[row].lang = lang0;
        }
        QString newContent = left + text0;
        std::vector<Span> newSpans = leftS;
        for (const Span& sp : sp0)
            appendSpan(newSpans, sp.s + left.size(), sp.e + left.size(), sp.kind, sp.href);
        if (n == 1) {                                  // single line: tail stays here
            caretRow = row; caretCol = newContent.size();
            for (const Span& sp : rightS)
                appendSpan(newSpans, sp.s + newContent.size(), sp.e + newContent.size(), sp.kind, sp.href);
            newContent += right;
        }
        content_[row] = newContent;
        rows_[row].spans = newSpans;
        rows_[row].param = (rows_[row].type == Code)
            ? static_cast<uint16_t>(std::clamp<int>(newContent.count(QLatin1Char('\n')) + 1, 1, 65535))
            : 1;
        persistContent(row);
        persistMeta(row);
        emit dataChanged(index(row), index(row), {ContentRole});
        if (n >= 2) appendBlocksAfter(row, 1);         // remaining lines → new blocks
    }

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return QVariantList{ caretRow, caretCol };
}

QVariantList BlockModel::pasteHtml(int row, int col, const QString& html) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return {};

    QTextDocument doc;
    doc.setHtml(html);

    // The frame-tree walker lives in Importer (shared with the file importers);
    // paste has no source directory, so relative image srcs don't resolve here.
    std::vector<BlockSpec> specs =
        Importer::specsFromTextDocument(doc, mediaStore_.get(), QString());
    expandTableSpecs(specs);                 // SR-4: pasted tables land as derived tables
    if (specs.empty()) return {};

    const bool opaque = (rows_[row].type == Media || rows_[row].type == Table
                         || rows_[row].type == Divider);

    // A single plain paragraph → splice it INLINE at the caret (so pasting a few
    // styled words from a browser stays in the sentence and keeps its formatting),
    // unless the target block is opaque (then fall through to insert-after).
    if (specs.size() == 1 && specs[0].type == Paragraph && !opaque) {
        const BlockSpec& sp = specs[0];
        const QString s = content_[row];
        col = std::clamp(col, 0, static_cast<int>(s.size()));
        beginTxn(row, row);
        shiftSpansInsert(rows_[row].spans, col, sp.text.size());
        for (const Span& x : sp.spans) {
            if (spanHasPayload(x.kind)) rows_[row].spans.push_back({x.s + col, x.e + col, x.kind, x.href});
            else addSpan(rows_[row].spans, x.s + col, x.e + col, x.kind);
        }
        content_[row] = s.left(col) + sp.text + s.mid(col);
        persistContent(row);
        persistMeta(row);
        emit dataChanged(index(row), index(row), {ContentRole});
        bumpLayout();
        ++contentRevision_;
        emit contentChangedSpike();
        endTxn();
        return QVariantList{ row, col + static_cast<int>(sp.text.size()) };
    }

    const auto [caretRow, caretCol] = insertSpecs(row, specs, /*allowReuseBlankRow=*/true);
    fetchRemoteMediaIn(row, caretRow);     // localize any remote <img> in the background
    return QVariantList{ caretRow, caretCol };
}

// Inverse of the spec sink (2026-08-20, the tab-merge program): one row →
// the BlockSpec spliceSpecsAt reproduces faithfully. Media/Table ride their
// content JSON whole (PDF page ink + chips live inside the descriptor, so
// they travel free); everything else carries text + spans WITH payloads.
BlockModel::BlockSpec BlockModel::specForRow(int row) const {
    BlockSpec sp;
    if (row < 0 || row >= static_cast<int>(rows_.size())) return sp;
    const Row& r = rows_[row];
    sp.cell = r.cell;
    sp.ratios = r.ratios;
    sp.header = r.header;
    sp.table = r.table;
    if (r.type == Media) {
        sp.type = Media;
        sp.mediaJson = content_[row];
        return sp;
    }
    if (r.type == Table) {
        sp.type = Table;
        sp.tableJson = content_[row];
        return sp;
    }
    sp.type = r.type;
    sp.level = r.level;
    sp.taskState = r.taskState;
    sp.depth = r.depth;
    sp.lang = r.lang;
    sp.text = content_[row];
    sp.spans = r.spans;
    return sp;
}

// Replay a migrated comment thread with its history intact. NOT undoable,
// by the same design as createThread — thread rows are doc-local side
// tables; the SpanComment anchors are what the undo stack tracks.
void BlockModel::importCommentThread(const ThreadImport& t) {
    if (!doc_.isOpen() || t.id.isEmpty()) return;
    doc_.createThreadAt(t.id, t.created, t.resolved);
    for (const ThreadMessage& m : t.messages)
        doc_.insertMessageAt(m.id, t.id, m.body, m.created, m.modified);
    ++commentsRevision_;
    emit commentsChanged();
}

std::pair<int,int> BlockModel::insertSpecs(int row, const std::vector<BlockSpec>& specs,
                                           bool allowReuseBlankRow) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || specs.empty())
        return { std::max(0, row), 0 };
    return spliceSpecsAt(row + 1, specs, allowReuseBlankRow);
}

// Gap-addressed core (2026-08-20, the tab-merge program): gap g = between
// rows g-1 and g, so 0 = top of document and count = the end — addresses
// insertSpecs (gap = row+1) never could. The blank-anchor fold only ever
// looks ABOVE the gap, matching the paste convention.
bool BlockModel::specsNeedTopLevel(const std::vector<BlockSpec>& specs) {
    for (const BlockSpec& sp : specs)
        if (sp.type == Split || (sp.type == Table && sp.mediaJson.isEmpty())) return true;
    return false;
}

int BlockModel::spliceGapFor(int gap, const std::vector<BlockSpec>& specs, int lane) const {
    const int n = static_cast<int>(rows_.size());
    gap = std::clamp(gap, 0, n);
    if (lane != -1 && !specsNeedTopLevel(specs)) return gap;
    while (gap < n && rows_[size_t(gap)].cell >= 0) ++gap;   // inside a split row → below it
    return gap;
}

int BlockModel::mergeGapFor(int gap) const {
    const int n = static_cast<int>(rows_.size());
    gap = std::clamp(gap, 0, n);
    for (;;) {
        while (gap < n && rows_[size_t(gap)].cell >= 0) ++gap;
        if (gap > 0 && gap < n && rows_[size_t(gap)].type == Split && rows_[size_t(gap - 1)].cell >= 0) {
            gap = splitRowEnd(gap) + 1;                         // between two split rows: past the next
            continue;
        }
        return gap;
    }
}

bool BlockModel::sanitizeSpecStructure(std::vector<BlockSpec>& specs) {
    if (specs.empty()) return false;
    std::vector<Row> rows(specs.size());
    for (size_t k = 0; k < specs.size(); ++k) {
        rows[k].type = specs[k].type;
        rows[k].cell = specs[k].cell;
        rows[k].ratios = specs[k].ratios;
        rows[k].header = specs[k].type == Split ? specs[k].header : 0;
    }
    const StructurePlan p = planStructure(rows, 0, rows.size() - 1);
    bool kept = false;
    for (size_t k = 0; k < specs.size(); ++k) {
        if (p.remove[k]) { specs[k] = BlockSpec{}; continue; }
        specs[k].cell = p.cell[k];
        specs[k].ratios = (specs[k].type == Split) ? p.ratios[k] : std::vector<float>{};
        if (specs[k].type != Split) { specs[k].header = 0; specs[k].table.clear(); }
        if (specs[k].type == Split) kept = true;
    }
    return kept;
}

BlockModel::CopyBand BlockModel::copyBand(int loRow, int loCol, int hiRow, int hiCol) const {
    CopyBand b{ loRow, loCol, hiRow, hiCol, false };
    const int n = static_cast<int>(rows_.size());
    if (n == 0 || loRow < 0 || hiRow >= n || loRow > hiRow) return b;
    const Row& lo = rows_[size_t(loRow)];
    const Row& hi = rows_[size_t(hiRow)];
    if (lo.cell >= 0 && lo.cell == hi.cell && splitRowOf(loRow) == splitRowOf(hiRow)) return b;
    const auto [wlo, whi] = wholeSplitRows(loRow, hiRow);
    if (wlo != loRow) { b.lo = wlo; b.loCol = 0; }
    if (whi != hiRow) { b.hi = whi; b.hiCol = std::numeric_limits<int>::max(); }
    for (int r = b.lo; r <= b.hi && !b.keepLanes; ++r) b.keepLanes = rows_[size_t(r)].type == Split;
    if (b.keepLanes && rows_[size_t(b.hi)].cell >= 0) b.hiCol = std::numeric_limits<int>::max();  // blocks in lanes copy whole
    return b;
}

std::pair<int,int> BlockModel::spliceSpecsAt(int gap, const std::vector<BlockSpec>& specsArg,
                                             bool allowReuseAnchorAbove, int lane) {
    const int n = static_cast<int>(rows_.size());
    // SR-4 S8a: a Table spec (an importer's IR, an old payload, a merge snapshot) lands as a derived table.
    // Callers with parallel data (ink, comment anchors) expand first and remap; this is the safety net.
    bool legacyTable = false;
    for (const BlockSpec& sp : specsArg) legacyTable = legacyTable || (sp.type == Table && sp.mediaJson.isEmpty());
    std::vector<BlockSpec> expanded;
    if (legacyTable) { expanded = specsArg; expandTableSpecs(expanded); }
    const std::vector<BlockSpec>& specsIn = legacyTable ? expanded : specsArg;
    if (specsIn.empty()) return { std::clamp(gap - 1, 0, std::max(0, n - 1)), 0 };
    const bool topLevel = lane == -1 || specsNeedTopLevel(specsIn);
    gap = spliceGapFor(gap, specsIn, lane);
    bool carriesSplit = false;
    for (const BlockSpec& sp : specsIn) carriesSplit = carriesSplit || sp.type == Split;
    std::vector<BlockSpec> sanitized;
    if (carriesSplit) { sanitized = specsIn; sanitizeSpecStructure(sanitized); }
    const std::vector<BlockSpec>& specs = carriesSplit ? sanitized : specsIn;

    // Turn a spec into (Row, content). depth/lang carry through (their former
    // silent drop at appendBlock was the latent paste bug this extraction
    // fixed); Code rows keep the makeCodeBlock convention param = line count.
    auto specRow = [&](const BlockSpec& sp, Row& r, QString& content) {
        r = Row{};
        if (!sp.mediaJson.isEmpty()) {              // embedded image → Media block
            r.type = Media;
            content = sp.mediaJson;
            fillMediaMeta(r, content);              // dims/aspect-param from the descriptor
        } else if (sp.type == Table) {
            r.type = Table;
            r.param = static_cast<uint16_t>(std::max(1, TableGrid::fromJson(sp.tableJson).rows()));
            content = sp.tableJson;
        } else {
            r.type = sp.type; r.level = sp.level; r.taskState = sp.taskState;
            r.depth = sp.depth; r.lang = sp.lang;
            r.param = (sp.type == Code)
                ? static_cast<uint16_t>(std::clamp<int>(sp.text.count(QLatin1Char('\n')) + 1, 1, 65535))
                : 1;
            r.spans = sp.spans;
            if (sp.type == Split) { r.ratios = sp.ratios; r.header = sp.header; r.table = sp.table; }
            content = sp.text;
        }
    };

    const int anchor = gap - 1;   // the row above the gap, if any
    bool reuse = false;
    if (allowReuseAnchorAbove && anchor >= 0 && (!topLevel || rows_[size_t(anchor)].cell < 0)) {
        const bool opaque = (rows_[anchor].type == Media || rows_[anchor].type == Table
                             || rows_[anchor].type == Divider);
        reuse = !opaque && content_[anchor].isEmpty()
            && (rows_[anchor].type == Paragraph || rows_[anchor].type == Heading
                || rows_[anchor].type == Quote || rows_[anchor].type == ListItem);
    }

    // Band: the anchor when folding into it; empty (lo > hi) for a pure
    // insertion — endTxn's delta math turns that into an after-band of
    // exactly the inserted rows.
    if (reuse) beginTxn(anchor, anchor);
    else       beginTxn(gap, gap - 1);
    int caretRow = std::max(0, anchor), caretCol = 0;
    int startSpec = 0;

    if (reuse) {                                          // fold the 1st block into the blank row
        Row r; QString content; specRow(specs[0], r, content);
        r.cell = rows_[anchor].cell;                     // the blank row keeps its lane
        rows_[anchor] = r;
        content_[anchor] = content;
        persistContent(anchor);
        persistMeta(anchor);
        emit dataChanged(index(anchor), index(anchor), {ContentRole});
        caretRow = anchor; caretCol = (specs[0].type == Table) ? 0 : content.size();
        startSpec = 1;
    }

    const int cnt = static_cast<int>(specs.size()) - startSpec;
    if (cnt > 0) {
        const int first = gap, last = gap + cnt - 1;
        QString prevRank = (gap > 0) ? ranks_[gap - 1] : QString();
        const QString nextRank = (gap < n) ? ranks_[gap] : QString();
        struct NewBlk { QString id, rank, content; Row r; };
        std::vector<NewBlk> made;
        beginInsertRows({}, first, last);
        // Split-row specs carry their own structure: estimate a lane's width from the incoming record's
        // ratios rather than asking the model, whose table grouping each insert dirties (a rebuild per row
        // was O(n²) for a big import). Media heights re-derive once the splice is in.
        const std::vector<float>* incomingRatios = nullptr;
        for (int k = 0; k < cnt; ++k) {
            const int at = first + k;
            const BlockSpec& sp = specs[startSpec + k];
            Row r; QString content; specRow(sp, r, content);
            r.cell = carriesSplit ? sp.cell : topLevel ? int8_t(-1) : laneAt(at);
            const QString id = makeUlid();
            const QString rk = rankBetween(prevRank, nextRank); prevRank = rk;
            rows_.insert(rows_.begin() + at, r);
            content_.insert(content_.begin() + at, content);
            ids_.insert(ids_.begin() + at, id);
            ranks_.insert(ranks_.begin() + at, rk);
            if (carriesSplit && r.cell < 0) incomingRatios = r.type == Split ? &sp.ratios : nullptr;
            const double laneW = !carriesSplit ? laneWidthForInsert(at, r.cell)
                               : r.cell < 0 || !incomingRatios ? contentWidth_
                               : laneWidthFrom(*incomingRatios, r.cell);
            indexInsert(static_cast<size_t>(at), estimatedHeight(r, laneW));
            made.push_back({id, rk, content, r});
            caretRow = at; caretCol = (sp.type == Table) ? 0 : content.size();
        }
        endInsertRows();
        if (doc_.isOpen())
            for (const NewBlk& b : made)
                doc_.appendBlock(b.id, b.rank, b.r.depth, QString::fromLatin1(typeToString(b.r.type)),
                                 attrsJson(b.r.type, b.r.level, b.r.lang, b.r.spans, b.r.taskState, b.r.cell, b.r.ratios, b.r.header, b.r.table), b.content);
        if (carriesSplit) rederiveMedia(first, last);   // their lanes' real widths (the loop estimated)
    }

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return { caretRow, caretCol };
}

// Scan [lo,hi] for media blocks whose src is a remote http(s) URL and download
// each into the sidecar; on completion swap the descriptor to the local copy
// (keyed by block id, so it survives reorders). The block displays the remote
// URL until then. Best-effort: a failed fetch just leaves the remote src.
void BlockModel::fetchRemoteMediaIn(int loRow, int hiRow) {
    if (!mediaStore_) return;
    loRow = std::max(0, loRow);
    hiRow = std::min(hiRow, static_cast<int>(rows_.size()) - 1);
    for (int r = loRow; r <= hiRow; ++r) {
        if (rows_[r].type != Media) continue;
        const QString src = QJsonDocument::fromJson(content_[r].toUtf8())
                                .object().value(QStringLiteral("src")).toString();
        if (!src.startsWith(QLatin1String("http"))) continue;
        if (!net_) net_ = new QNetworkAccessManager(this);
        const QString id = ids_[r];
        QNetworkRequest req((QUrl(src)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("minNotes"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = net_->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) return;   // leave remote src
            QImage img;
            if (!img.loadFromData(reply->readAll()) || img.isNull()) return;
            const MediaStore::ImageRef ref = mediaStore_->importImage(img);
            if (ref.ok()) updateMediaDescriptor(id, QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("src"), ref.src},
                                          {QStringLiteral("w"), ref.w},
                                          {QStringLiteral("h"), ref.h}}).toJson(QJsonDocument::Compact)));
        });
    }
}

// Swap a media block's descriptor in place (a remote image finished localizing).
// Not a txn — it's a background system update, not a user edit; undoing the paste
// still removes the whole block.
void BlockModel::updateMediaDescriptor(const QString& blockId, const QString& json) {
    const int row = rowForId(blockId);
    if (row < 0 || rows_[row].type != Media) return;
    content_[row] = json;
    fillMediaMeta(rows_[row], json);
    rows_[row].measured = false;
    setIndexHeight(static_cast<size_t>(row), estimatedHeight(rows_[row], laneWidthOfRow(row)));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
}

void BlockModel::splitBlock(int row, int col) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (isOpaqueRow(row)) return;                    // descriptors don't split
    if (rows_[row].type == Split) return;            // a split row's record holds no text
    // A split strictly inside a choice chip would clone it into two chips
    // sharing one payload (the straddle path below copies href to both
    // halves) — snap to the chip's end instead (DT-2, 2026-08-20).
    for (const Span& sp : rows_[row].spans)
        if (sp.kind == SpanChoice && col > sp.s && col < sp.e) { col = sp.e; break; }
    beginTxn(row, row);                          // after grows to [row, row+1]
    const QString s = content_[row];
    col = std::clamp(col, 0, static_cast<int>(s.size()));
    const QString left = s.left(col), right = s.mid(col);
    const int at = row + 1;
    const QString newId = makeUlid();
    const QString newRank = rankBetween(ranks_[row],
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    // Split spans at `col`: left part stays, right part moves to the new block.
    std::vector<Span> leftS, rightS;
    for (const Span& sp : rows_[row].spans) {
        if (sp.s < col) leftS.push_back({sp.s, std::min(sp.e, col), sp.kind, sp.href});
        if (sp.e > col) rightS.push_back({std::max(sp.s, col) - col, sp.e - col, sp.kind, sp.href});
    }
    rows_[row].spans = leftS;

    content_[row] = left;
    persistContent(row);
    persistMeta(row);

    beginInsertRows({}, at, at);
    Row r{}; r.type = Paragraph; r.param = 1; r.spans = rightS;
    r.cell = rows_[row].cell;                        // Enter inside a lane stays in that lane
    // List continuation: Enter inside a list item makes the next item of the
    // SAME kind at the SAME depth (a new task starts todo, not a copy of the
    // finished state). The QML splitLine handles the exit gesture (Enter on
    // an EMPTY item demotes it to a paragraph instead of splitting).
    if (isListType(rows_[row].type)) {
        r.type = rows_[row].type;
        r.depth = rows_[row].depth;
    }
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, right);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    indexInsert(static_cast<size_t>(at), estimatedHeight(r, laneWidthForInsert(at, r.cell)));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, r.depth, QString::fromLatin1(typeToString(r.type)), QString(), right);
    if (!rightS.empty() || r.type != Paragraph || r.cell >= 0) persistMeta(at);   // spans / list / lane meta

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

bool BlockModel::isOpaqueRow(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    const uint8_t t = rows_[row].type;
    return t == Media || t == Table || t == Divider;
}

void BlockModel::insertParagraphRaw(int row) {
    row = std::clamp(row, 0, static_cast<int>(rows_.size()));
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (row > 0) ? ranks_[row - 1] : QString(),
        (row < static_cast<int>(ranks_.size())) ? ranks_[row] : QString());

    beginInsertRows({}, row, row);
    Row r{}; r.type = Paragraph; r.param = 1; r.cell = laneAt(row);
    rows_.insert(rows_.begin() + row, r);
    content_.insert(content_.begin() + row, QString());
    ids_.insert(ids_.begin() + row, newId);
    ranks_.insert(ranks_.begin() + row, newRank);
    indexInsert(static_cast<size_t>(row), estimatedHeight(r, laneWidthForInsert(row, r.cell)));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), attrsJson(r.type, r.level, r.lang, r.spans, r.taskState, r.cell, r.ratios, r.header, r.table), QString());
}

void BlockModel::insertBlock(int row) {
    row = std::clamp(row, 0, static_cast<int>(rows_.size()));
    beginTxn(row, row - 1);                      // empty `before`; after = [row,row]
    insertParagraphRaw(row);
    bumpLayout();
    ++contentRevision_;            // row→content mapping shifted: refresh content bindings
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::duplicateBlock(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (rows_[row].type == Split) return;        // a record isn't a block; its row moves whole
    const int at = row + 1;
    beginTxn(at, at - 1);                        // empty `before`; after = [at,at]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(ranks_[row],
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    beginInsertRows({}, at, at);
    Row r = rows_[row];                          // copies type/level/lang/spans/param
    r.measured = false;
    const QString text = content_[row];
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, text);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    indexInsert(static_cast<size_t>(at), estimatedHeight(r, laneWidthForInsert(at, r.cell)));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)),
                         attrsJson(r.type, r.level, r.lang, r.spans, r.taskState, r.cell, r.ratios, r.header, r.table), text);

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::removeBlocks(int loRow, int hiRow) {
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return;
    loRow = std::clamp(loRow, 0, n - 1);
    hiRow = std::clamp(hiRow, 0, n - 1);
    if (loRow > hiRow) std::swap(loRow, hiRow);
    // A record in the range takes its whole split row with it.
    for (int i = loRow; i <= hiRow; ++i)
        if (rows_[size_t(i)].type == Split) hiRow = std::max(hiRow, splitRowEnd(i));
    const int cnt = hiRow - loRow + 1;
    const auto band = wholeSplitRows(loRow, hiRow);   // A4 below may reshape a split row
    beginTxn(band.first, band.second);           // after = the reshaped band, or the refill row
    for (int i = loRow; i <= hiRow; ++i) {
        dropBlockInk(ids_[i]);   // hash sync; DB cascades via FK
        if (doc_.isOpen()) doc_.deleteBlock(ids_[i]);
    }
    beginRemoveRows({}, loRow, hiRow);
    std::vector<double> hs;
    hs.reserve(rows_.size() - static_cast<size_t>(cnt));
    for (int i = 0; i < n; ++i)
        if (i < loRow || i > hiRow) hs.push_back(layout().height(static_cast<size_t>(i)));
    rows_.erase(rows_.begin() + loRow, rows_.begin() + hiRow + 1);
    content_.erase(content_.begin() + loRow, content_.begin() + hiRow + 1);
    ids_.erase(ids_.begin() + loRow, ids_.begin() + hiRow + 1);
    ranks_.erase(ranks_.begin() + loRow, ranks_.begin() + hiRow + 1);
    reindex(std::move(hs));
    endRemoveRows();
    normalizeStructure(band.first, band.second - cnt);
    // Never leave an empty document: the band's after-side becomes the fresh
    // paragraph (delta = -cnt + 1 → after = [lo, lo]).
    if (rows_.empty()) insertParagraphRaw(0);
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

QVariantList BlockModel::deleteSelectionRange(int loRow, int loCol, int hiRow, int hiCol) {
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return {};
    if (loRow > hiRow || (loRow == hiRow && loCol > hiCol)) {
        std::swap(loRow, hiRow); std::swap(loCol, hiCol);
    }
    loRow = std::clamp(loRow, 0, n - 1);
    hiRow = std::clamp(hiRow, 0, n - 1);
    // Across containers (SR-0 §4.10). Inside one split row, every lane from the low end's
    // to the high end's is cleared — structure stays. Otherwise it's a row range: split
    // rows it touches go whole and top-level ends keep what lies outside the selection.
    const int recLo = splitRowOf(loRow), recHi = splitRowOf(hiRow);
    // SR-4 A7 / §4.10: ends in different cells of one table → clear the cell rectangle; the
    // table's rows, columns and colours stay.
    const int headLo = tableHeadOf(loRow), headHi = tableHeadOf(hiRow);
    const int cellLo = rows_[size_t(loRow)].cell, cellHi = rows_[size_t(hiRow)].cell;
    if (headLo >= 0 && headLo == headHi && cellLo >= 0 && cellHi >= 0 && (recLo != recHi || cellLo != cellHi)) {
        const int ra = gridRowOf(loRow), rb = gridRowOf(hiRow);
        const int r0 = std::min(ra, rb), c0 = std::min(cellLo, cellHi);
        gridClearCells(headLo, r0, c0, std::max(ra, rb), std::max(cellLo, cellHi));
        return QVariantList{ std::max(0, gridCellAt(headLo, r0, c0)), 0 };
    }
    if (recLo >= 0 && recLo == recHi && rows_[size_t(loRow)].cell != rows_[size_t(hiRow)].cell)
        return clearLanes(recLo, rows_[size_t(loRow)].cell, rows_[size_t(hiRow)].cell);
    if (recLo != recHi) {
        // A row range reaching into a table takes the whole table — no orphaned table rows.
        if (headLo >= 0) { loRow = headLo; loCol = 0; }
        if (headHi >= 0) {
            hiRow = tableBand(headHi).second;
            hiCol = static_cast<int>(content_[size_t(hiRow)].size());
        }
        return deleteRowRange(loRow, loCol, hiRow, hiCol);
    }
    const bool loOpaque = isOpaqueRow(loRow), hiOpaque = isOpaqueRow(hiRow);
    if (!loOpaque) {
        if (loRow == hiRow && loCol == hiCol) return QVariantList{ loRow, loCol };   // nothing selected
        deleteRange(loRow, loCol, hiRow, hiCol);          // an opaque high row is consumed whole
        return QVariantList{ loRow, std::min<int>(loCol, content_[loRow].size()) };
    }
    // Opaque low end → whole blocks go. A text high row keeps its tail
    // (the part after hiCol) and slides up into the low slot.
    beginTxn(loRow, hiRow);
    if (loRow == hiRow || hiOpaque) removeBlocks(loRow, hiRow);
    else {
        deleteRange(hiRow, 0, hiRow, hiCol);
        removeBlocks(loRow, hiRow - 1);
    }
    endTxn();
    const int land = std::min(loRow, static_cast<int>(rows_.size()) - 1);
    return QVariantList{ land, 0 };
}

void BlockModel::removeBlock(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    // A record stands for its whole split row. Removing a lane's last block
    // collapses the lane (A4) — the band covers whole split rows either way.
    const int last = rows_[size_t(row)].type == Split ? splitRowEnd(row) : row;
    const auto band = wholeSplitRows(row, last);
    beginTxn(band.first, band.second);           // after = the reshaped band (often empty)
    for (int i = last; i >= row; --i) removeRowRaw(i);
    normalizeStructure(band.first, band.second - (last - row + 1));
    bumpLayout();
    ++contentRevision_;            // row→content mapping shifted: refresh content bindings
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::moveBlock(int from, int to) { moveBlocks(from, 1, to); }

int BlockModel::rowAfterMove(int r, int from, int count, int to) {
    if (count <= 0 || from == to) return r;
    if (r >= from && r < from + count) return r + (to - from);          // inside the run
    if (from < to && r >= from + count && r < to + count) return r - count;   // slid up
    if (to < from && r >= to && r < from) return r + count;                   // slid down
    return r;
}

void BlockModel::moveBlocks(int from, int count, int to, int targetLane) {
    const int n = static_cast<int>(rows_.size());
    // In place is a no-op — unless the run changes lane where it stands.
    if (count < 1 || from < 0 || from + count > n || to < 0 || to > n - count
        || (from == to && targetLane == kInheritLane)) return;
    // Split rows move whole, and only between top-level rows; any other run joins
    // the lane (or the top level) it lands in (PLAN-SR3 S3b).
    bool carriesSplit = false;
    for (int k = from; k < from + count; ++k)
        if (rows_[size_t(k)].type == Split) carriesSplit = true;
    const bool asTopLevel = targetLane == -1;
    if (carriesSplit) {
        if (targetLane >= 0) return;                                           // split rows never go in a lane
        if (rows_[size_t(from)].cell >= 0) return;                             // starts inside a split row
        if (from + count < n && rows_[size_t(from + count)].cell >= 0) return; // ends inside one
    }
    auto origOf = [&](int reduced) { return reduced < from ? reduced : reduced + count; };
    if (carriesSplit || asTopLevel) {
        // The destination must be a top-level gap: the row that will sit right BELOW the
        // run can't be a lane block. (A gap just after a split row is top level.)
        if (to < n - count && rows_[size_t(origOf(to))].cell >= 0) return;
    } else if (targetLane >= 0) {
        // A lane gap: the row right above or below the run must be a block of that lane.
        const bool below = to < n - count && rows_[size_t(origOf(to))].cell == targetLane;
        const bool above = to > 0 && rows_[size_t(origOf(to - 1))].cell == targetLane;
        if (!below && !above) return;
    }
    // The touched band: every row between the two positions, inclusive of the run
    // at either end, widened to whole split rows (a lane the run leaves may
    // collapse). Ids permute inside it → a full-band entry.
    const auto band = wholeSplitRows(std::min(from, to), std::max(from, to) + count - 1);
    beginTxn(band.first, band.second);

    // Lift the run out (content/type/spans + measured heights travel with it).
    std::vector<Row> rs(rows_.begin() + from, rows_.begin() + from + count);
    std::vector<QString> ids(ids_.begin() + from, ids_.begin() + from + count);
    std::vector<QString> cs(content_.begin() + from, content_.begin() + from + count);
    std::vector<double> hs;
    hs.reserve(static_cast<size_t>(count));
    for (int k = 0; k < count; ++k) hs.push_back(layout().height(static_cast<size_t>(from + k)));
    rows_.erase(rows_.begin() + from, rows_.begin() + from + count);
    content_.erase(content_.begin() + from, content_.begin() + from + count);
    ids_.erase(ids_.begin() + from, ids_.begin() + from + count);
    ranks_.erase(ranks_.begin() + from, ranks_.begin() + from + count);
    for (int k = 0; k < count; ++k) indexErase(static_cast<size_t>(from));

    // Chain fresh ranks between the destination neighbours (reduced list).
    const int sz = static_cast<int>(ranks_.size());
    QString prev = (to > 0)  ? ranks_[to - 1] : QString();
    const QString next = (to < sz) ? ranks_[to] : QString();
    for (int k = 0; k < count; ++k) {
        const QString rk = rankBetween(prev, next);
        prev = rk;
        const int at = to + k;
        Row moved = rs[static_cast<size_t>(k)];
        if (!carriesSplit)                                           // the destination's lane
            moved.cell = targetLane >= 0 ? static_cast<int8_t>(targetLane) : asTopLevel ? int8_t(-1) : laneAt(at);
        rows_.insert(rows_.begin() + at, moved);
        content_.insert(content_.begin() + at, cs[static_cast<size_t>(k)]);
        ids_.insert(ids_.begin() + at, ids[static_cast<size_t>(k)]);
        ranks_.insert(ranks_.begin() + at, rk);
        indexInsert(static_cast<size_t>(at), hs[static_cast<size_t>(k)]);
        if (doc_.isOpen()) doc_.updateRank(ids[static_cast<size_t>(k)], rk);
        if (moved.cell != rs[static_cast<size_t>(k)].cell) persistMeta(at);
    }
    normalizeStructure(band.first, band.second);   // the lane the run left may be empty now
    rederiveMedia(band.first, band.second);        // moved media lays out at its new width

    bumpLayout();                 // positions change; cells re-read yForRow/content
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

// === Rich clipboard (0.5.0) ==============================================

std::vector<BlockModel::BlockSpec> BlockModel::specsForRange(int loRow, int loCol,
                                                             int hiRow, int hiCol) const {
    std::vector<BlockSpec> out;
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return out;
    if (loRow > hiRow || (loRow == hiRow && loCol > hiCol)) {
        std::swap(loRow, hiRow); std::swap(loCol, hiCol);
    }
    loRow = std::clamp(loRow, 0, n - 1);
    hiRow = std::clamp(hiRow, 0, n - 1);
    const CopyBand band = copyBand(loRow, loCol, hiRow, hiCol);
    loRow = band.lo; loCol = band.loCol; hiRow = band.hi; hiCol = band.hiCol;
    for (int r = loRow; r <= hiRow; ++r) {
        BlockSpec sp = specForRow(r);
        if (!band.keepLanes) { sp.cell = -1; sp.ratios.clear(); sp.header = 0; sp.table.clear(); }
        if (isOpaqueRow(r)) { out.push_back(std::move(sp)); continue; }   // whole-in
        const int len = sp.text.size();
        int from = (r == loRow) ? std::clamp(loCol, 0, len) : 0;
        int to   = (r == hiRow) ? std::clamp(hiCol, 0, len) : len;
        if (from > to) std::swap(from, to);
        if (from == 0 && to == len) { out.push_back(std::move(sp)); continue; }
        // Slice: spans clipped to [from,to) and rebased; a chip cut in half
        // would be a broken widget → only whole chips travel.
        std::vector<Span> clipped;
        for (const Span& s : sp.spans) {
            const int a = std::max(s.s, from), b = std::min(s.e, to);
            if (a >= b) continue;
            if (s.kind == SpanChoice && (s.s < from || s.e > to)) continue;
            clipped.push_back({a - from, b - from, s.kind, s.href});
        }
        sp.text = sp.text.mid(from, to - from);
        sp.spans = std::move(clipped);
        out.push_back(std::move(sp));
    }
    return out;
}

QString BlockModel::plainTextForRange(int loRow, int loCol, int hiRow, int hiCol) const {
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return {};
    if (loRow > hiRow || (loRow == hiRow && loCol > hiCol)) {
        std::swap(loRow, hiRow); std::swap(loCol, hiCol);
    }
    loRow = std::clamp(loRow, 0, n - 1);
    hiRow = std::clamp(hiRow, 0, n - 1);
    const CopyBand band = copyBand(loRow, loCol, hiRow, hiCol);
    loRow = band.lo; loCol = band.loCol; hiRow = band.hi; hiCol = band.hiCol;
    QStringList parts;
    for (int r = loRow; r <= hiRow; ++r) {
        const uint8_t t = rows_[r].type;
        if (t == Media || t == Split) continue;              // no honest text form / lanes read in order
        if (t == Divider) { parts << QStringLiteral("---"); continue; }
        if (t == Table) {
            const TableGrid& g = gridFor(r);
            parts << tableRangeTSV(r, 0, 0, g.rows() - 1, g.cols() - 1);
            continue;
        }
        const QString& s = content_[r];
        const int len = s.size();
        int from = (r == loRow) ? std::clamp(loCol, 0, len) : 0;
        int to   = (r == hiRow) ? std::clamp(hiCol, 0, len) : len;
        if (from > to) std::swap(from, to);
        parts << s.mid(from, to - from);
    }
    return parts.join(QLatin1Char('\n'));
}

QString BlockModel::clipboardPayloadForRange(int loRow, int loCol, int hiRow, int hiCol) const {
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return {};
    if (loRow > hiRow || (loRow == hiRow && loCol > hiCol)) {
        std::swap(loRow, hiRow); std::swap(loCol, hiCol);
    }
    loRow = std::clamp(loRow, 0, n - 1);
    hiRow = std::clamp(hiRow, 0, n - 1);
    const CopyBand band = copyBand(loRow, loCol, hiRow, hiCol);     // the grain specsForRange copies
    loRow = band.lo; loCol = band.loCol; hiRow = band.hi; hiCol = band.hiCol;

    BlockClipboard::Payload p;
    p.docPath = docPath_;
    p.docDir = mediaAnchorDir();
    p.package = mediaStore_ ? mediaStore_->packageSource() : QString();
    p.pageWidth = pageWidth_;
    p.specs = specsForRange(loRow, loCol, hiRow, hiCol);
    if (p.specs.empty()) return {};

    // Ink is pinned to the WHOLE block: only rows copied whole carry it.
    p.ink.reserve(p.specs.size());
    for (int r = loRow; r <= hiRow; ++r) {
        bool whole = isOpaqueRow(r);
        if (!whole) {
            const int len = content_[r].size();
            const int from = (r == loRow) ? std::clamp(loCol, 0, len) : 0;
            const int to   = (r == hiRow) ? std::clamp(hiCol, 0, len) : len;
            whole = (from == 0 && to == len);
        }
        p.ink.push_back(whole ? inkForRow(r) : QString());
    }

    // Comment threads: bodies ride with their anchors (SOURCE ids — the
    // paste remints or re-anchors); ghost anchors are stripped.
    QHash<QString, QVariantMap> threads;
    for (const QVariant& tv : commentThreads()) {
        const QVariantMap m = tv.toMap();
        threads.insert(m.value(QStringLiteral("id")).toString(), m);
    }
    QSet<QString> harvested;
    for (BlockSpec& sp : p.specs) {
        for (auto it = sp.spans.begin(); it != sp.spans.end();) {
            if (it->kind != SpanComment) { ++it; continue; }
            const auto th = threads.constFind(it->href);
            if (th == threads.constEnd()) { it = sp.spans.erase(it); continue; }
            if (!harvested.contains(it->href)) {
                harvested.insert(it->href);
                ThreadImport ti;
                ti.id = it->href;
                ti.created = th->value(QStringLiteral("created")).toLongLong();
                ti.resolved = th->value(QStringLiteral("resolved")).toBool();
                for (const QVariant& mv : commentMessages(it->href)) {
                    const QVariantMap mm = mv.toMap();
                    ti.messages.push_back({QString(), mm.value(QStringLiteral("body")).toString(),
                                           mm.value(QStringLiteral("created")).toLongLong(),
                                           mm.value(QStringLiteral("modified")).toLongLong()});
                }
                p.threads.push_back(std::move(ti));
            }
            ++it;
        }
    }

    // Asset snapshot: where every collected src's bytes live right now, so a
    // paste after the source tab closes still finds them (no side effects).
    const AssetTransfer::Source src = AssetTransfer::Source::fromStore(mediaStore_.get());
    QSet<QString> seen;
    AssetTransfer::forEachSrc(p.specs, [&](const QJsonValue& v, bool isVideo) {
        if (!v.isString()) return;
        const QString rel = v.toString();
        if (!rel.startsWith(QLatin1String(".minnotes/")) || seen.contains(rel)) return;
        seen.insert(rel);
        const PackageExporter::Resolved res = src.resolve ? src.resolve(rel) : PackageExporter::Resolved{};
        if (!res.ok) return;
        BlockClipboard::Asset a;
        a.rel = rel; a.abs = res.absPath; a.entry = res.packageEntry;
        a.bytes = res.bytes; a.video = isVideo;
        p.assets.push_back(std::move(a));
    });
    return QString::fromUtf8(BlockClipboard::encode(p));
}

std::pair<int,int> BlockModel::pasteSpecsAt(int row, int col, std::vector<BlockSpec> specs,
                                            const std::vector<QString>& inkIn, qreal srcPageWidth) {
    lastPasteRelocated_ = false;
    // SR-4 S8a: a pasted Table spec (an old payload) lands as a derived table; ink stays with its spec's first row.
    std::vector<QString> ink = inkIn;
    {
        const size_t before = specs.size();
        const std::vector<int> at = expandTableSpecs(specs);
        if (specs.size() != before) {
            std::vector<QString> moved(specs.size());
            for (size_t k = 0; k < at.size() && k < inkIn.size(); ++k)
                if (at[k] >= 0) moved[size_t(at[k])] = inkIn[k];
            ink.swap(moved);
        }
    }
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return { 0, 0 };
    row = std::clamp(row, 0, n - 1);
    if (specs.empty()) return { row, std::clamp(col, 0, static_cast<int>(content_[row].size())) };
    const bool needTop = specsNeedTopLevel(specs);

    auto textish = [](const BlockSpec& sp) {
        return sp.mediaJson.isEmpty() && sp.type != Table && sp.type != Divider && sp.type != Split;
    };
    const bool migrate = srcPageWidth > 0 && !qFuzzyCompare(srcPageWidth, pageWidth_);
    // Lay ink for specs[k0..] onto rows firstRow.. (parallel). The FIRST one
    // may be a merged row: never overwrite ink it already has.
    auto layInk = [&](int firstRow, size_t k0, size_t k1, bool firstMerged) {
        for (size_t k = k0; k < k1 && k < specs.size() && k < ink.size(); ++k) {
            if (ink[k].isEmpty()) continue;
            const int r = firstRow + static_cast<int>(k - k0);
            if (r < 0 || r >= static_cast<int>(rows_.size())) continue;
            if (k == k0 && firstMerged && !inkForRow(r).isEmpty()) continue;
            QString j = ink[k];
            if (migrate) {
                const QString m = migrateInkForWidth(j, srcPageWidth, pageWidth_);
                if (!m.isEmpty()) j = m;
            }
            setBlockInk(r, j);
        }
    };
    auto finish = [&]() {
        bumpLayout();
        ++contentRevision_;
        emit contentChangedSpike();
    };

    // A split row or a table can't sit in a lane: with the caret in one, the
    // whole paste lands below its split row (SR-3 S8; the editor Toasts it).
    // Callers' undo bands must cover the whole split row (ClipboardPaster).
    if (needTop && splitRowOf(row) >= 0) {
        const int gap = splitRowEnd(splitRowOf(row)) + 1;
        beginTxn(gap, gap - 1);
        const auto caret = spliceSpecsAt(gap, specs, /*allowReuseAnchorAbove=*/false, -1);
        layInk(gap, 0, specs.size(), false);
        lastPasteRelocated_ = true;
        finish();
        endTxn();
        return caret;
    }
    const int spliceLane = needTop ? -1 : kInheritLane;

    // Opaque target, or an opaque first spec into a non-empty row: everything
    // lands AFTER the row. An opaque first spec into an EMPTY simple row folds
    // into it (spliceSpecsAt's anchor reuse).
    const bool targetOpaque = isOpaqueRow(row);
    if (targetOpaque || !textish(specs[0])) {
        const bool emptySimple = !targetOpaque && content_[row].isEmpty()
            && (rows_[row].type == Paragraph || rows_[row].type == Heading
                || rows_[row].type == Quote || rows_[row].type == ListItem);
        if (emptySimple) beginTxn(row, row); else beginTxn(row + 1, row);
        const auto caret = spliceSpecsAt(row + 1, specs, /*allowReuseAnchorAbove=*/emptySimple, spliceLane);
        layInk(emptySimple ? row : row + 1, 0, specs.size(), false);
        finish();
        endTxn();
        return caret;
    }

    // Text target + text-ish first spec: split the row at the caret, merge.
    beginTxn(row, row);
    const QString s = content_[row];
    col = std::clamp(col, 0, static_cast<int>(s.size()));
    for (const Span& sp : rows_[row].spans)           // never split a chip
        if (sp.kind == SpanChoice && col > sp.s && col < sp.e) { col = sp.e; break; }
    const QString left = s.left(col), right = s.mid(col);
    std::vector<Span> leftS, rightS;
    for (const Span& sp : rows_[row].spans) {
        if (sp.s < col) leftS.push_back({sp.s, std::min(sp.e, col), sp.kind, sp.href});
        if (sp.e > col) rightS.push_back({std::max(sp.s, col) - col, sp.e - col, sp.kind, sp.href});
    }
    auto appendSpan = [](std::vector<Span>& v, int a, int b, uint8_t kind, const QString& href) {
        if (a >= b) return;
        if (spanHasPayload(kind)) v.push_back({a, b, kind, href});
        else addSpan(v, a, b, kind);
    };
    const BlockSpec first = specs[0];
    // Adopt the first spec's block identity only at a CLEAN start and when it
    // carries one (a plain paragraph pasted at the start of a heading must
    // not demote the heading).
    const bool adopt = left.isEmpty()
        && (first.type != Paragraph || (right.isEmpty() && rows_[row].type == Paragraph));
    if (adopt) {
        rows_[row].type = first.type; rows_[row].level = first.level;
        rows_[row].taskState = first.taskState; rows_[row].depth = first.depth;
        rows_[row].lang = first.lang;
    }
    QString newContent = left + first.text;
    std::vector<Span> newSpans = leftS;
    for (const Span& sp : first.spans)
        appendSpan(newSpans, sp.s + left.size(), sp.e + left.size(), sp.kind, sp.href);
    int caretRow = row, caretCol = newContent.size();
    const bool single = specs.size() == 1;
    if (single) {
        for (const Span& sp : rightS)
            appendSpan(newSpans, sp.s + newContent.size(), sp.e + newContent.size(), sp.kind, sp.href);
        newContent += right;
    }
    content_[row] = newContent;
    rows_[row].spans = newSpans;
    rows_[row].param = (rows_[row].type == Code)
        ? static_cast<uint16_t>(std::clamp<int>(newContent.count(QLatin1Char('\n')) + 1, 1, 65535)) : 1;
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {ContentRole});

    if (!single) {
        std::vector<BlockSpec> rest(specs.begin() + 1, specs.end());
        const size_t lastIdx = rest.size() - 1;
        bool tailSpec = false;
        if (textish(rest[lastIdx])) {
            BlockSpec& last = rest[lastIdx];
            caretCol = last.text.size();
            for (const Span& sp : rightS)
                appendSpan(last.spans, sp.s + last.text.size(), sp.e + last.text.size(), sp.kind, sp.href);
            last.text += right;
        } else {
            caretCol = 0;
            if (!right.isEmpty() || !rightS.empty()) {
                BlockSpec tail; tail.type = Paragraph; tail.text = right; tail.spans = rightS;
                rest.push_back(std::move(tail));
                tailSpec = true;
            }
        }
        spliceSpecsAt(row + 1, rest, /*allowReuseAnchorAbove=*/false, spliceLane);
        caretRow = row + 1 + static_cast<int>(lastIdx);
        Q_UNUSED(tailSpec);
        layInk(row + 1, 1, specs.size(), false);
    }
    layInk(row, 0, 1, /*firstMerged=*/true);   // only spec[0]'s ink, onto the merged row
    finish();
    endTxn();
    return { caretRow, caretCol };
}

bool BlockModel::htmlIsBareRemoteImage(const QString& html) const {
    return Importer::htmlIsBareRemoteImage(html);
}

void BlockModel::duplicateBlocks(int loRow, int hiRow) {
    const int n = static_cast<int>(rows_.size());
    if (n == 0) return;
    loRow = std::clamp(loRow, 0, n - 1);
    hiRow = std::clamp(hiRow, 0, n - 1);
    if (loRow > hiRow) std::swap(loRow, hiRow);
    const CopyBand band = copyBand(loRow, 0, hiRow, 0);   // copy grain: one lane, or whole split rows
    loRow = band.lo; hiRow = band.hi;
    std::vector<BlockSpec> specs;
    std::vector<QString> ink;
    for (int r = loRow; r <= hiRow; ++r) {
        specs.push_back(specForRow(r));
        if (!band.keepLanes) { specs.back().cell = -1; specs.back().ratios.clear(); specs.back().header = 0; specs.back().table.clear(); }
        ink.push_back(inkForRow(r));
    }
    const int gap = spliceGapFor(hiRow + 1, specs);
    beginTxn(gap, gap - 1);                      // empty before; after = the copies
    spliceSpecsAt(gap, specs, /*allowReuseAnchorAbove=*/false);
    for (size_t k = 0; k < ink.size(); ++k)
        if (!ink[k].isEmpty()) setBlockInk(gap + static_cast<int>(k), ink[k]);
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

// === Spell menu: replace a range as ONE transaction ========================
void BlockModel::replaceText(int row, int s, int e, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || isOpaqueRow(row)) return;
    // Engine edit on copies first: an empty no-op must not open a txn. A span
    // covering the replaced range keeps it; others shift or clip (InlineText.h).
    QString edited = content_[row];
    std::vector<Span> spans = rows_[row].spans;
    const bool hadSpans = !spans.empty();
    if (!mn::inl::replaceRange(edited, spans, s, e, text)) return;
    beginTxn(row, row);
    content_[row] = edited;
    persistContent(row);
    if (hadSpans) {
        rows_[row].spans = std::move(spans);
        persistMeta(row);
    }
    if (rows_[row].type == Code)
        rows_[row].param = static_cast<uint16_t>(std::clamp<int>(content_[row].count(QLatin1Char('\n')) + 1, 1, 65535));
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}
