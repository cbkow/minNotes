#include "BlockModel.h"
#include "Importer.h"
#include "PathMap.h"
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
static inline bool spanHasPayload(uint8_t k) {
    return k == BlockModel::SpanLink || k == BlockModel::SpanFgColor
        || k == BlockModel::SpanHighlight || k == BlockModel::SpanComment
        || k == BlockModel::SpanChoice;
}
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
    return QFileInfo(docPath_).completeBaseName();   // basename without .mndb/.mnpkg
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
    fenwick_.reset(std::vector<double>{});
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
    return dir + QStringLiteral("/work-") + makeUlid() + QStringLiteral(".mndb");
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

    // .mnpkg fork: packages are SEALED SNAPSHOTS (user ruling 2026-08-18 —
    // .mndb is the ONE editable format; a package is produced by Export and
    // received for viewing). Opening one is a LAZY read: only document.mndb
    // extracts up front (instant regardless of package size); media stays in
    // the archive until something touches it (MediaStore::resolvePath pulls
    // `media/<x>` → `.minnotes/<x>` on first access; videos bring their
    // .qcview sidecar). The doc opens with UNTITLED semantics — you can read,
    // play, even type, but nothing persists until Save As materializes a
    // real .mndb; the package file itself is NEVER written. docPath_ stays
    // the .mnpkg for tab dedupe, recents, and documentName.
    if (!untitled && mnpkg::isPackagePath(path)) {
        if (!QFileInfo::exists(path)) return false;
        pkgDir_ = scratchDir() + QStringLiteral("/pkg-") + makeUlid();
        scratchPath_ = pkgDir_ + QStringLiteral("/document.mndb");
        if (!mnpkg::extractEntry(path, QLatin1String(mnpkg::kDbEntry), scratchPath_)) {
            qWarning() << "BlockModel: package has no readable document.mndb" << path;
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
    docPath_ = path;
    untitled_ = untitled;
    mediaStore_ = std::make_unique<MediaStore>(path);   // media anchored to the ORIGINAL folder
    recordOriginalStat();
    dirty_ = false;
    setSaveState(SaveClean);
    clearUndo();
    return true;
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
    const QString path = dir + QStringLiteral("/untitled-") + makeUlid() + QStringLiteral(".mndb");
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
    if (!path.endsWith(QLatin1String(".mndb"), Qt::CaseInsensitive)) path += QStringLiteral(".mndb");
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
    std::vector<int> canonicalized;   // rows whose markdown markers were consumed

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
                canonicalized.push_back(static_cast<int>(rows_.size()));   // row index of the push below
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
        heights.push_back(estimatedHeight(r));
    }
    // Persist the conversions into the WORKING COPY in one transaction (the
    // original file updates on the next explicit save, as always). This is a
    // normalization, not a user edit: no undo entries (the txn chokepoint is
    // bypassed deliberately), no dirty flag.
    if (!canonicalized.empty() && doc_.isOpen()) {
        doc_.begin();
        for (int row : canonicalized) { persistContent(row); persistMeta(row); }
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
    fenwick_.reset(std::move(heights));
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
        heights.push_back(estimatedHeight(r));
    }
    fenwick_.reset(std::move(heights));
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
double BlockModel::mediaDisplayWidth(const Row& r) const {
    if (r.dispW > 0) return r.dispW;
    if (r.isPdf) return contentWidth_;   // fit the page
    // Sketches ride the image path since the frame became user-sized: natural
    // (canvas) width up to the page, dw drag-resize honoured like any image.
    if (r.mediaW > 0) return std::min<double>(contentWidth_, r.mediaW);
    return contentWidth_;
}

// Displayed media frame height: dispW = min(contentWidth, w) (never upscaled),
// height = round(dispW * h/w). Pure function of the probed dims + the layout
// width — recomputed when setContentWidth changes (resize). Falls back to the
// aspect param if intrinsic dims are missing.
double BlockModel::mediaFrameHeight(const Row& r) const {
    if (r.isFile) return kFileChip;            // fixed-height attachment chip
    const double w = mediaDisplayWidth(r);
    if (r.mediaW > 0 && r.mediaH > 0)
        return std::floor(w * r.mediaH / r.mediaW + 0.5);
    return contentWidth_ * (r.param / 100.0);
}

double BlockModel::estimatedHeight(const Row& r) const {
    switch (r.type) {
    case Heading: return kHeading + kPadV;
    case Media:   // 12px vertical pad + the transport toolbar for video. Matches
                  // the Editor cell delegate exactly, and media never measures
                  // back, so this IS the authoritative height (no scroll-in jump).
        return 12.0 + mediaFrameHeight(r)
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
                              const std::vector<Span>& spans, uint8_t taskState) const {
    QJsonObject o;
    if (type == Heading && level > 0) o.insert(QStringLiteral("level"), level);
    if (type == TaskListItem) o.insert(QStringLiteral("state"), taskState);
    if (type == Code && !lang.isEmpty()) o.insert(QStringLiteral("lang"), lang);
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
    if (!doc_.isOpen() || row < 0 || row >= static_cast<int>(ids_.size())) return;
    const Row& r = rows_[row];
    doc_.updateMeta(ids_[row], QString::fromLatin1(typeToString(r.type)),
                    attrsJson(r.type, r.level, r.lang, r.spans, r.taskState), r.depth);
}

// === Undo / redo: region-snapshot transactions ===========================
BlockModel::BlockSnap BlockModel::snapAt(int row) const {
    BlockSnap s;
    s.id = ids_[row]; s.rank = ranks_[row]; s.content = content_[row];
    s.type = rows_[row].type; s.level = rows_[row].level; s.spans = rows_[row].spans;
    s.taskState = rows_[row].taskState;
    s.depth = rows_[row].depth;
    s.lang = rows_[row].lang;
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
        heightById.insert(ids_[i], fenwick_.height(i));
        if (rows_[i].measured) measuredById.insert(ids_[i], fenwick_.height(i));
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
        r.taskState = s.taskState;
        r.depth = s.depth;
        r.param = static_cast<uint16_t>(std::max<int>(1, s.content.count(QLatin1Char('\n')) + 1));
        fillMediaMeta(r, s.content);   // media: dims/video/param from descriptor (else height ~= 0)
        rows_.insert(rows_.begin() + at, r);
        content_.insert(content_.begin() + at, s.content);
        ids_.insert(ids_.begin() + at, s.id);
        ranks_.insert(ranks_.begin() + at, s.rank);
        if (doc_.isOpen()) {
            const QString attrs = attrsJson(s.type, s.level, s.lang, s.spans, s.taskState);
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
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const bool replaced = (i >= lo && i < regionEnd);        // a restored snap → re-measure
        auto it = measuredById.constFind(ids_[i]);
        if (!replaced && rows_[i].measured && it != measuredById.constEnd()) {
            heights.push_back(it.value());                       // untouched → keep its real height
        } else {
            rows_[i].measured = false;                           // changed/new → re-measure
            auto hit = heightById.constFind(ids_[i]);
            heights.push_back(hit != heightById.constEnd()
                                  ? hit.value()                  // seed: pre-reset height beats a
                                  : estimatedHeight(rows_[i]));  // media-blind estimate; re-born → estimate
        }
    }
    fenwick_.reset(std::move(heights));
    endResetModel();
    ++layoutRevision_; ++contentRevision_;
    if (inkTouched) { ++inkRevision_; emit inkChanged(); }
    emit modelReset(); emit layoutChangedSpike(); emit contentChangedSpike();
    applying_ = false;
}

void BlockModel::applyPatches(const std::vector<UndoPatch>& ps, bool beforeSide) {
    applying_ = true;
    bool inkTouched = false;
    for (const UndoPatch& p : ps) {
        const BlockSnap& s = beforeSide ? p.before : p.after;
        if (p.row < 0 || p.row >= static_cast<int>(rows_.size())
            || ids_[p.row] != s.id) continue;   // safety net — never expected
        Row& r = rows_[p.row];
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
        // height change re-report through the normal chain.
        if (doc_.isOpen()) {
            doc_.updateContent(s.id, s.content);
            doc_.updateMeta(s.id, QString::fromLatin1(typeToString(s.type)),
                            attrsJson(s.type, s.level, s.lang, s.spans, s.taskState),
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
    auto snapEq = [](const BlockSnap& x, const BlockSnap& y) {
        if (x.id != y.id || x.rank != y.rank || x.type != y.type
            || x.level != y.level || x.taskState != y.taskState || x.depth != y.depth
            || x.content != y.content
            || x.spans.size() != y.spans.size()
            || x.ink != y.ink) return false;   // last: usually shared → O(1) equal
        for (size_t j = 0; j < x.spans.size(); ++j)
            if (x.spans[j].s != y.spans[j].s || x.spans[j].e != y.spans[j].e
                || x.spans[j].kind != y.spans[j].kind) return false;
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
    if (sparse) {
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
    const uint8_t k = spanKindFromString(kind);
    if (!k) return false;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end) return false;
    return spansCover(rows_[row].spans, start, end, k);
}

bool BlockModel::payloadSpanCovers(int row, int start, int end,
                                   int kind, const QString& value) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end) return false;
    std::vector<std::pair<int,int>> segs;
    for (const Span& sp : rows_[row].spans)
        if (sp.kind == static_cast<uint8_t>(kind) && sp.href == value
            && sp.e > start && sp.s < end)
            segs.emplace_back(sp.s, sp.e);
    std::sort(segs.begin(), segs.end());
    int cov = start;
    for (const auto& seg : segs) {
        if (seg.first > cov) break;          // gap before this segment
        cov = std::max(cov, seg.second);
        if (cov >= end) return true;
    }
    return cov >= end;
}

void BlockModel::setFormat(int row, int start, int end, const QString& kind, bool on) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const uint8_t k = spanKindFromString(kind);
    if (!k) return;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end) return;
    beginTxn(row, row);
    if (on) addSpan(rows_[row].spans, start, end, k);
    else    removeSpan(rows_[row].spans, start, end, k);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {});
    ++contentRevision_; emit contentChangedSpike();
    endTxn();
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
    if (!e.before.empty() || !e.after.empty())
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
    if (!e.before.empty() || !e.after.empty())
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
    if (a.size() > b.size()) {
        const int n = int(a.size() - b.size());
        return n == 1 ? tr("Insert block") : tr("Insert %1 blocks").arg(n);
    }
    if (a.size() < b.size()) {
        const int n = int(b.size() - a.size());
        return n == 1 ? tr("Delete block") : tr("Delete %1 blocks").arg(n);
    }
    bool contentDiff = false, inkDiff = false, spanDiff = false;
    bool metaDiff = false, rankDiff = false, commentAdded = false;
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
                    || x.spans[j].kind != y.spans[j].kind) { spanDiff = true; break; }
        }
    }
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
        if (e.coalesce == QLatin1String("del")) return tr("Delete text");
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
    Row r{}; r.type = Divider; r.param = 1;
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, QString());
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();
    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QStringLiteral("divider"), QString(), QString());
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
    mutateTable(row, [&](TableGrid& g){ g.setCellText(r, c, text); },
                QStringLiteral("tcell:%1:%2").arg(r).arg(c));
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

QVariantList BlockModel::tableCellSpans(int row, int r, int c) const {
    QVariantList out;
    if (rowAt(row).type != Table) return out;
    const QJsonArray a = gridFor(row).cellSpans(r, c);
    for (const QJsonValue& it : a) {
        const QJsonObject o = it.toObject();
        QVariantMap m;
        m.insert(QStringLiteral("s"), o.value(QStringLiteral("s")).toInt());
        m.insert(QStringLiteral("e"), o.value(QStringLiteral("e")).toInt());
        m.insert(QStringLiteral("k"), o.value(QStringLiteral("k")).toInt());
        if (o.contains(QStringLiteral("u"))) m.insert(QStringLiteral("u"), o.value(QStringLiteral("u")).toString());
        out.append(m);
    }
    return out;
}

bool BlockModel::tableCellHasFormat(int row, int r, int c, int start, int end, const QString& kind) const {
    if (rowAt(row).type != Table) return false;
    const uint8_t k = spanKindFromString(kind);
    if (!k) return false;
    const TableGrid& g = gridFor(row);
    const int len = g.cellText(r, c).size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end) return false;
    return spansCover(cellSpansFromJson(g.cellSpans(r, c)), start, end, k);
}

void BlockModel::tableSetCellFormat(int row, int r, int c, int start, int end, const QString& kind, bool on) {
    const uint8_t k = spanKindFromString(kind);
    if (!k) return;
    mutateTable(row, [&](TableGrid& g){
        const int len = g.cellText(r, c).size();
        const int s = std::clamp(start, 0, len), e = std::clamp(end, 0, len);
        if (s >= e) return;
        std::vector<Span> v = cellSpansFromJson(g.cellSpans(r, c));
        if (on) addSpan(v, s, e, k); else removeSpan(v, s, e, k);
        g.setCellSpans(r, c, cellSpansToJson(v));
    });
}

void BlockModel::tableClearCellFormat(int row, int r, int c, int start, int end) {
    mutateTable(row, [&](TableGrid& g){
        const int len = g.cellText(r, c).size();
        const int s = std::clamp(start, 0, len), e = std::clamp(end, 0, len);
        if (s >= e) return;
        std::vector<Span> kept;                       // drop any span overlapping [s,e)
        for (const Span& sp : cellSpansFromJson(g.cellSpans(r, c))) {
            if (sp.e <= s || sp.s >= e) { kept.push_back(sp); continue; }
            if (sp.s < s) kept.push_back({sp.s, s, sp.kind, sp.href});
            if (sp.e > e) kept.push_back({e, sp.e, sp.kind, sp.href});
        }
        g.setCellSpans(r, c, cellSpansToJson(kept));
    });
}

// Span-aware cell text edits: insert/delete text AND shift the cell's spans, so
// formatting stays glued to its characters as the user types (mirrors insertText/
// deleteRange for blocks). Coalesced per cell like tableSetCell.
void BlockModel::tableCellInsert(int row, int r, int c, int at, const QString& text) {
    if (text.isEmpty()) return;
    mutateTable(row, [&](TableGrid& g){
        QString t = g.cellText(r, c);
        const int p = std::clamp(at, 0, int(t.size()));
        g.setCellText(r, c, t.left(p) + text + t.mid(p));
        std::vector<Span> v = cellSpansFromJson(g.cellSpans(r, c));
        shiftSpansInsert(v, p, text.size());
        g.setCellSpans(r, c, cellSpansToJson(v));
    }, QStringLiteral("tcell:%1:%2").arg(r).arg(c));
}

void BlockModel::tableCellDelete(int row, int r, int c, int from, int to) {
    mutateTable(row, [&](TableGrid& g){
        QString t = g.cellText(r, c);
        const int f = std::clamp(from, 0, int(t.size())), e = std::clamp(to, 0, int(t.size()));
        if (f >= e) return;
        g.setCellText(r, c, t.left(f) + t.mid(e));
        std::vector<Span> v = cellSpansFromJson(g.cellSpans(r, c));
        shiftSpansDelete(v, f, e);
        g.setCellSpans(r, c, cellSpansToJson(v));
    }, QStringLiteral("tcell:%1:%2").arg(r).arg(c));
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
            for (int j = 0; j < src.cols(); ++j)
                g.setCellText(r + i, c + j, src.cellText(i, j));
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
            for (int c = C0; c <= C1; ++c) g.setCellText(r, c, QString());
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
// Contents wipe: text/spans/media/choice go, colours (formatting) stay.
void clearCellContents(TableGrid& g, int r, int c) {
    g.setCellText(r, c, QString());
    g.setCellSpans(r, c, QJsonArray());
    g.setCellMedia(r, c, QString());
    g.setCellChoice(r, c, QString());
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
            for (int c = 0; c < g.cols(); ++c) clearCellContents(g, r, c);
    });
}
void BlockModel::tableClearColumns(int row, const QVariantList& cols) {
    const std::vector<int> set = sortedIndexSet(cols);
    if (set.empty()) return;
    mutateTable(row, [&](TableGrid& g) {
        for (int c : set)
            for (int r = 0; r < g.rows(); ++r) clearCellContents(g, r, c);
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
    // Default canvas: square, 480 source px (ruling 2026-06-11). Strokes are
    // normalized [0,1] of that square — QCView's exact stroke schema, so the
    // one engine serializes/renders sketches and video notes alike.
    QJsonObject root;
    root.insert(QStringLiteral("kind"), QStringLiteral("sketch"));
    root.insert(QStringLiteral("w"), 480);
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
    Row r{}; r.type = Media;
    fillMediaMeta(r, json);                      // dims/kind from the descriptor
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, json);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), QString(), json);

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
    fenwick_.setHeight(static_cast<size_t>(row), estimatedHeight(rows_[row]));
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
    Row r{}; r.type = Table; r.param = static_cast<uint16_t>(nRows);
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, json);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), QString(), json);

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
    Row r{}; r.type = Table; r.param = static_cast<uint16_t>(g.rows());
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, json);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), QString(), json);

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
    Row r{}; r.type = Media; r.param = aspectParam;
    fillMediaMeta(r, json);   // dims/video/aspect-param from the descriptor (single source)
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, json);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), QString(), json);

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
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end || rows_[row].spans.empty()) return;
    beginTxn(row, row);
    removeSpan(rows_[row].spans, start, end, SpanBold);
    removeSpan(rows_[row].spans, start, end, SpanItalic);
    removeSpan(rows_[row].spans, start, end, SpanCode);
    removeSpan(rows_[row].spans, start, end, SpanStrike);
    removeSpan(rows_[row].spans, start, end, SpanUnderline);
    removeSpan(rows_[row].spans, start, end, SpanLink);
    removeSpan(rows_[row].spans, start, end, SpanFgColor);
    removeSpan(rows_[row].spans, start, end, SpanHighlight);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {});
    ++contentRevision_; emit contentChangedSpike();
    endTxn();
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
uint8_t BlockModel::spanKindFromString(const QString& s) {
    if (s == QLatin1String("bold"))      return SpanBold;
    if (s == QLatin1String("italic"))    return SpanItalic;
    if (s == QLatin1String("code"))      return SpanCode;
    if (s == QLatin1String("strike"))    return SpanStrike;
    if (s == QLatin1String("underline")) return SpanUnderline;
    if (s == QLatin1String("link"))      return SpanLink;
    if (s == QLatin1String("color"))     return SpanFgColor;
    if (s == QLatin1String("highlight")) return SpanHighlight;
    if (s == QLatin1String("comment"))   return SpanComment;
    if (s == QLatin1String("choice"))    return SpanChoice;
    return 0;
}
const char* BlockModel::spanKindToString(uint8_t k) {
    switch (k) {
    case SpanBold:      return "bold";
    case SpanItalic:    return "italic";
    case SpanCode:      return "code";
    case SpanStrike:    return "strike";
    case SpanUnderline: return "underline";
    case SpanLink:      return "link";
    case SpanFgColor:   return "color";
    case SpanHighlight: return "highlight";
    case SpanComment:   return "comment";
    case SpanChoice:    return "choice";
    default:            return "";
    }
}

// Does the union of same-kind spans fully cover [start,end)?
bool BlockModel::spansCover(const std::vector<Span>& v, int start, int end, uint8_t kind) {
    if (start >= end) return true;
    std::vector<Span> k;
    for (const Span& sp : v) if (sp.kind == kind) k.push_back(sp);
    std::sort(k.begin(), k.end(), [](const Span& a, const Span& b){ return a.s < b.s; });
    int cur = start;
    for (const Span& sp : k) {
        if (sp.s > cur) break;                 // gap before coverage reaches `cur`
        cur = std::max(cur, sp.e);
        if (cur >= end) return true;
    }
    return cur >= end;
}

// Add [start,end) of `kind`, then merge overlapping/adjacent same-kind spans.
void BlockModel::addSpan(std::vector<Span>& v, int start, int end, uint8_t kind) {
    if (start >= end) return;
    std::vector<Span> k, others;
    for (const Span& sp : v) (sp.kind == kind ? k : others).push_back(sp);
    k.push_back({start, end, kind});
    std::sort(k.begin(), k.end(), [](const Span& a, const Span& b){ return a.s < b.s; });
    std::vector<Span> merged;
    for (const Span& sp : k) {
        if (!merged.empty() && sp.s <= merged.back().e)
            merged.back().e = std::max(merged.back().e, sp.e);
        else
            merged.push_back(sp);
    }
    v = others;
    v.insert(v.end(), merged.begin(), merged.end());
}

// Add a payload (colour) run: a span carrying a value, where same-kind spans of
// a DIFFERENT value must not coexist over the same chars. Clear the kind's
// coverage in [start,end), add the run, then coalesce adjacent same-value spans.
void BlockModel::applyPayloadRun(std::vector<Span>& v, int start, int end,
                                 uint8_t kind, const QString& payload) {
    if (start >= end || payload.isEmpty()) return;
    removeSpan(v, start, end, kind);                       // run owns this range
    std::vector<Span> same, others;
    for (const Span& sp : v) (sp.kind == kind ? same : others).push_back(sp);
    same.push_back({start, end, kind, payload});
    std::sort(same.begin(), same.end(), [](const Span& a, const Span& b){ return a.s < b.s; });
    std::vector<Span> merged;
    for (const Span& sp : same) {
        if (!merged.empty() && sp.s <= merged.back().e && sp.href == merged.back().href)
            merged.back().e = std::max(merged.back().e, sp.e);
        else
            merged.push_back(sp);
    }
    v = others;
    v.insert(v.end(), merged.begin(), merged.end());
}

// Subtract [start,end) from same-kind spans (splitting where it lands inside).
void BlockModel::removeSpan(std::vector<Span>& v, int start, int end, uint8_t kind) {
    if (start >= end) return;
    std::vector<Span> out;
    for (const Span& sp : v) {
        if (sp.kind != kind || sp.e <= start || sp.s >= end) { out.push_back(sp); continue; }
        if (sp.s < start) out.push_back({sp.s, start, kind, sp.href});   // left remainder
        if (sp.e > end)   out.push_back({end, sp.e, kind, sp.href});     // right remainder
    }
    v = out;
}

// Offset bookkeeping: text inserted at `at` (len chars), or [from,to) deleted.
void BlockModel::shiftSpansInsert(std::vector<Span>& v, int at, int len) {
    for (Span& sp : v) {
        if (at <= sp.s)      { sp.s += len; sp.e += len; }   // wholly after the caret
        else if (at < sp.e)  { sp.e += len; }                // typed inside → grow (not at exact end)
    }
}
void BlockModel::shiftSpansDelete(std::vector<Span>& v, int from, int to) {
    const int len = to - from;
    if (len <= 0) return;
    std::vector<Span> out;
    for (const Span& sp : v) {
        if (sp.e <= from) { out.push_back(sp); continue; }                       // before cut
        if (sp.s >= to)   { out.push_back({sp.s - len, sp.e - len, sp.kind, sp.href}); continue; }  // after cut
        const int ns = std::min(sp.s, from);                 // surviving head + shifted tail collapse
        const int ne = (sp.e > to) ? sp.e - len : from;
        if (ne > ns) out.push_back({ns, ne, sp.kind, sp.href});
    }
    v = out;
}

QVariantList BlockModel::spansForRow(int row) const {
    QVariantList out;
    if (rows_.empty()) return out;
    for (const Span& sp : rowAt(row).spans) {
        QVariantMap m;
        m.insert(QStringLiteral("s"), sp.s);
        m.insert(QStringLiteral("e"), sp.e);
        m.insert(QStringLiteral("k"), sp.kind);
        if (spanHasPayload(sp.kind)) m.insert(QStringLiteral("u"), sp.href);
        out.append(m);
    }
    return out;
}

// A payload span (link/color/highlight) over [start,end): drop any same-kind span
// overlapping the range (they don't merge — each carries its own URL/color), then
// add the new one if a payload was given (empty = remove that kind here).
void BlockModel::setPayloadSpan(int row, int start, int end, uint8_t kind,
                                const QString& payload, const QString& coalesce) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
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
static QString sanitizeChoiceLabel(QString s) {
    static const QString bad = QStringLiteral("`*_~[]\\\n\r");
    QString out;
    out.reserve(s.size());
    for (const QChar ch : s)
        if (!bad.contains(ch)) out += ch;
    out = out.trimmed();
    return out.isEmpty() ? QStringLiteral("Option") : out;
}
static QString choiceLabelFor(const QJsonObject& payload, const QString& id) {
    for (const QJsonValue& v : payload.value(QStringLiteral("o")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("id")).toString() == id)
            return o.value(QStringLiteral("l")).toString();
    }
    return {};
}
static QString choiceColorFor(const QJsonObject& payload, const QString& id) {
    for (const QJsonValue& v : payload.value(QStringLiteral("o")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("id")).toString() == id)
            return o.value(QStringLiteral("c")).toString();
    }
    return {};
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
    const int len = content_[row].size();
    col = std::clamp(col, 0, len);
    for (const Span& sp : rows_[row].spans)   // never inside another chip
        if (sp.kind == SpanChoice && col > sp.s && col < sp.e) col = sp.e;
    // The default set (ruling 2026-08-20): the app's tri-state culture.
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
    const QString label = QStringLiteral("To do");

    beginTxn(row, row);
    shiftSpansInsert(rows_[row].spans, col, label.size());
    content_[row].insert(col, label);
    rows_[row].spans.push_back({col, col + int(label.size()), SpanChoice,
                                QString::fromUtf8(QJsonDocument(payload)
                                    .toJson(QJsonDocument::Compact))});
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return col;
}

QString BlockModel::choiceAt(int row, int col) const {
    if (rows_.empty()) return {};
    for (const Span& sp : rowAt(row).spans)
        if (sp.kind == SpanChoice && col >= sp.s && col < sp.e) return sp.href;
    return {};
}
QVariantList BlockModel::choiceRangeAt(int row, int col) const {
    if (rows_.empty()) return {};
    for (const Span& sp : rowAt(row).spans)
        if (sp.kind == SpanChoice && col >= sp.s && col < sp.e)
            return QVariantList{ sp.s, sp.e };
    return {};
}

// Swap the selected option: replace the label TEXT and the payload's "v" in
// ONE txn. Other spans on the row shift as if [s,e) was retyped.
void BlockModel::setChoiceSelected(int row, int spanStart, const QString& optionId) {
    Span* sp = choiceSpanAt(row, spanStart);
    if (!sp) return;
    QJsonObject payload = QJsonDocument::fromJson(sp->href.toUtf8()).object();
    if (payload.value(QStringLiteral("v")).toString() == optionId) return;   // no-op
    const QString label = choiceLabelFor(payload, optionId);
    if (label.isEmpty()) return;                            // unknown id
    payload.insert(QStringLiteral("v"), optionId);
    replaceChoiceText(row, spanStart, label, payload);
}

// Shared tail for label/payload rewrites: swap [s,e) text for `label`,
// shift every OTHER span, restore the chip span with its new bounds.
void BlockModel::replaceChoiceText(int row, int spanStart, const QString& label,
                                   const QJsonObject& payload) {
    Span* spp = choiceSpanAt(row, spanStart);
    if (!spp) return;
    const Span chip = *spp;
    beginTxn(row, row);
    std::vector<Span>& v = rows_[row].spans;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Span& s) {
        return s.kind == SpanChoice && s.s == chip.s;
    }), v.end());
    shiftSpansDelete(v, chip.s, chip.e);
    shiftSpansInsert(v, chip.s, label.size());
    content_[row] = content_[row].left(chip.s) + label + content_[row].mid(chip.e);
    v.push_back({chip.s, chip.s + int(label.size()), SpanChoice,
                 QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))});
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

QString BlockModel::choiceAddOption(int row, int spanStart, const QString& label,
                                    const QString& colorHex) {
    Span* sp = choiceSpanAt(row, spanStart);
    if (!sp) return {};
    const QString clean = sanitizeChoiceLabel(label);
    QJsonObject payload = QJsonDocument::fromJson(sp->href.toUtf8()).object();
    QJsonArray opts = payload.value(QStringLiteral("o")).toArray();
    QJsonObject o;
    const QString id = makeUlid();
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("l"), clean);
    if (!colorHex.isEmpty()) o.insert(QStringLiteral("c"), colorHex);
    opts.append(o);
    payload.insert(QStringLiteral("o"), opts);
    payload.insert(QStringLiteral("v"), id);   // quick-add implies intent
    replaceChoiceText(row, spanStart, clean, payload);
    return id;
}

// The options-editor commit: preserve ids, mint empty ones, sanitize labels;
// a deleted selected id falls back to the FIRST option (text follows —
// the text==label invariant); an empty set removes the chip.
void BlockModel::setChoiceOptions(int row, int spanStart, const QVariantList& options) {
    Span* sp = choiceSpanAt(row, spanStart);
    if (!sp) return;
    if (options.isEmpty()) { removeChoiceAt(row, spanStart); return; }
    QJsonObject payload = QJsonDocument::fromJson(sp->href.toUtf8()).object();
    QJsonArray opts;
    for (const QVariant& v : options) {
        const QVariantMap m = v.toMap();
        const QString label = sanitizeChoiceLabel(m.value(QStringLiteral("label")).toString());
        QString id = m.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) id = makeUlid();
        QJsonObject o;
        o.insert(QStringLiteral("id"), id);
        o.insert(QStringLiteral("l"), label);
        const QString c = m.value(QStringLiteral("color")).toString();
        if (!c.isEmpty()) o.insert(QStringLiteral("c"), c);
        opts.append(o);
    }
    QString sel = payload.value(QStringLiteral("v")).toString();
    payload.insert(QStringLiteral("o"), opts);
    QString label = choiceLabelFor(payload, sel);
    if (label.isEmpty()) {   // selected option was deleted → first option
        sel = opts.first().toObject().value(QStringLiteral("id")).toString();
        label = opts.first().toObject().value(QStringLiteral("l")).toString();
    }
    payload.insert(QStringLiteral("v"), sel);
    replaceChoiceText(row, spanStart, label, payload);
}

void BlockModel::removeChoiceAt(int row, int spanStart) {
    Span* sp = choiceSpanAt(row, spanStart);
    if (!sp) return;
    deleteRange(row, sp->s, row, sp->e);   // full cover — the span dies with it
}

QVariantList BlockModel::choiceRangesForRow(int row) const {
    QVariantList out;
    if (rows_.empty()) return out;
    row = clampRow(row);
    for (const Span& sp : rows_[row].spans) {
        if (sp.kind != SpanChoice || sp.e <= sp.s) continue;
        const QJsonObject payload = QJsonDocument::fromJson(sp.href.toUtf8()).object();
        QVariantMap m;
        m.insert(QStringLiteral("s"), sp.s);
        m.insert(QStringLiteral("e"), sp.e);
        m.insert(QStringLiteral("color"),
                 choiceColorFor(payload, payload.value(QStringLiteral("v")).toString()));
        out.append(m);
    }
    return out;
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
    for (const Span& sp : rows_[row].spans) {
        if (sp.kind != SpanComment || sp.e <= sp.s) continue;
        QVariantMap m;
        m.insert(QStringLiteral("s"), sp.s);
        m.insert(QStringLiteral("e"), sp.e);
        m.insert(QStringLiteral("id"), sp.href);
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
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const uint8_t k = spanKindFromString(kind);
    if (!k) return;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len);
    end   = std::clamp(end, 0, len);
    if (start >= end) return;
    beginTxn(row, row);
    std::vector<Span>& spans = rows_[row].spans;
    if (spansCover(spans, start, end, k)) removeSpan(spans, start, end, k);
    else                                  addSpan(spans, start, end, k);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

QVariant BlockModel::data(const QModelIndex& index, int role) const {
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
    const Row& r = rows_[row];
    switch (role) {
    case TypeRole:     return r.type;
    case ContentRole:  return textAt(row);
    case HeightRole:   return fenwick_.height(row);
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
    if (m != maxContentWidth_) {
        maxContentWidth_ = m;
        emit maxContentWidthChanged();
    }
}

void BlockModel::setMeasuredHeight(int row, qreal h) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || h <= 0.0) return;
    const double delta = fenwick_.setHeight(static_cast<size_t>(row), h);
    if (!rows_[row].measured) rows_[row].measured = true;
    if (delta != 0.0) {
        emit heightSettled(row, delta);
        bumpLayout();
    }
}

qreal BlockModel::mediaDisplayHeight(int row) const {
    const Row& r = rowAt(row);
    return (r.type == Media) ? mediaFrameHeight(r) : 0.0;
}
int BlockModel::mediaDispWidth(int row) const {
    const Row& r = rowAt(row);
    return (r.type == Media) ? int(mediaDisplayWidth(r) + 0.5) : 0;
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
    fenwick_.setHeight(static_cast<size_t>(row), estimatedHeight(rows_[row]));
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
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].type == Media)
            if (fenwick_.setHeight(i, estimatedHeight(rows_[i])) != 0.0) any = true;
    if (any) bumpLayout();
}

void BlockModel::setContent(int row, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    beginTxn(row, row);
    content_[row] = text;
    persistContent(row);                          // write-through to SQLite
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
    beginTxn(loRow, hiRow);

    // Merge surviving head of lo block with surviving tail of hi block.
    const QString loText = textAt(loRow);
    const QString hiText = textAt(hiRow);
    const int loClip = std::min<int>(loCol, loText.size());
    const int hiClip = std::min<int>(hiCol, hiText.size());
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
            if (i < first || i > last) hs.push_back(fenwick_.height(static_cast<size_t>(i)));
        rows_.erase(rows_.begin() + first, rows_.begin() + last + 1);
        content_.erase(content_.begin() + first, content_.begin() + last + 1);
        ids_.erase(ids_.begin() + first, ids_.begin() + last + 1);
        ranks_.erase(ranks_.begin() + first, ranks_.begin() + last + 1);
        fenwick_.reset(std::move(hs));
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
    beginTxn(row, row);
    const QString s = content_[row];
    col = std::clamp(col, 0, static_cast<int>(s.size()));
    content_[row] = s.left(col) + text + s.mid(col);
    persistContent(row);
    std::vector<Span>& spans = rows_[row].spans;
    if (!spans.empty())                              // keep existing span offsets aligned
        shiftSpansInsert(spans, col, text.size());
    const int e = col + text.size();
    if (marks) {                                     // armed typing attributes → span the new run
        if (marks & 1)  addSpan(spans, col, e, SpanBold);
        if (marks & 2)  addSpan(spans, col, e, SpanItalic);
        if (marks & 4)  addSpan(spans, col, e, SpanCode);
        if (marks & 8)  addSpan(spans, col, e, SpanStrike);
        if (marks & 16) addSpan(spans, col, e, SpanUnderline);
    }
    if (!fgColor.isEmpty()) applyPayloadRun(spans, col, e, SpanFgColor,   fgColor);
    if (!bgColor.isEmpty()) applyPayloadRun(spans, col, e, SpanHighlight, bgColor);
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
    QStringList segs;
    const QStringList lines = t.split(QLatin1Char('\n'));
    for (const QString& ln : lines)
        if (!ln.trimmed().isEmpty()) segs << ln;
    if (segs.isEmpty()) return {};

    // Parse one line → (type, level, clean text, spans): block-prefix then inline
    // markdown. A "```" line renders literally (fenced code not yet reconstructed).
    auto parseSeg = [&](const QString& seg, uint8_t& type, uint8_t& level, uint8_t& taskState,
                        QString& outText, std::vector<Span>& outSpans, bool& hadPrefix) {
        type = Paragraph; level = 0; taskState = 0; outText = seg; outSpans.clear(); hadPrefix = false;
        const bool fence = seg.startsWith(QLatin1String("```"));
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

    const int n = segs.size();
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
        struct NewBlk { QString id, rank, content; uint8_t type, level, taskState; std::vector<Span> spans; };
        std::vector<NewBlk> made;
        beginInsertRows({}, first, last);
        for (int k = 0; k < cnt; ++k) {
            const int at = first + k;
            uint8_t tj, lj, tsj; QString textj; std::vector<Span> spj; bool pfxj;
            parseSeg(segs[startSeg + k], tj, lj, tsj, textj, spj, pfxj);
            QString contentj = textj;
            std::vector<Span> spans = spj;
            if (k == cnt - 1) {                        // last block carries the tail
                caretRow = at; caretCol = textj.size();
                for (const Span& sp : rightS)
                    appendSpan(spans, sp.s + contentj.size(), sp.e + contentj.size(), sp.kind, sp.href);
                contentj += right;
            }
            Row r{}; r.type = tj; r.level = lj; r.taskState = tsj; r.param = 1; r.spans = spans;
            const QString newId = makeUlid();
            const QString newRank = rankBetween(prevRank, nextRank);
            prevRank = newRank;
            rows_.insert(rows_.begin() + at, r);
            content_.insert(content_.begin() + at, contentj);
            ids_.insert(ids_.begin() + at, newId);
            ranks_.insert(ranks_.begin() + at, newRank);
            fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
            made.push_back({newId, newRank, contentj, tj, lj, tsj, spans});
        }
        endInsertRows();
        if (doc_.isOpen())
            for (const NewBlk& b : made)
                doc_.appendBlock(b.id, b.rank, 0, QString::fromLatin1(typeToString(b.type)),
                                 attrsJson(b.type, b.level, QString(), b.spans, b.taskState), b.content);
    };

    if (opaque) {
        appendBlocksAfter(row, 0);                     // opaque block stays untouched
    } else {
        // ---- Block 0: merge seg[0] into the current block at the caret ----
        uint8_t t0, l0, ts0; QString text0; std::vector<Span> sp0; bool pfx0;
        parseSeg(segs[0], t0, l0, ts0, text0, sp0, pfx0);
        // Adopt the parsed block type only at a clean start (nothing to the left)
        // AND when the line carried a real prefix; else keep the block's type and
        // treat seg[0] as inline-styled text.
        if (left.isEmpty() && pfx0) { rows_[row].type = t0; rows_[row].level = l0; rows_[row].taskState = ts0; }
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
        rows_[row].param = 1;
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
std::pair<int,int> BlockModel::spliceSpecsAt(int gap, const std::vector<BlockSpec>& specs,
                                             bool allowReuseAnchorAbove) {
    const int n = static_cast<int>(rows_.size());
    if (specs.empty()) return { std::clamp(gap - 1, 0, std::max(0, n - 1)), 0 };
    gap = std::clamp(gap, 0, n);

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
            content = sp.text;
        }
    };

    const int anchor = gap - 1;   // the row above the gap, if any
    bool reuse = false;
    if (allowReuseAnchorAbove && anchor >= 0) {
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
        for (int k = 0; k < cnt; ++k) {
            const int at = first + k;
            const BlockSpec& sp = specs[startSpec + k];
            Row r; QString content; specRow(sp, r, content);
            const QString id = makeUlid();
            const QString rk = rankBetween(prevRank, nextRank); prevRank = rk;
            rows_.insert(rows_.begin() + at, r);
            content_.insert(content_.begin() + at, content);
            ids_.insert(ids_.begin() + at, id);
            ranks_.insert(ranks_.begin() + at, rk);
            fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
            made.push_back({id, rk, content, r});
            caretRow = at; caretCol = (sp.type == Table) ? 0 : content.size();
        }
        endInsertRows();
        if (doc_.isOpen())
            for (const NewBlk& b : made)
                doc_.appendBlock(b.id, b.rank, b.r.depth, QString::fromLatin1(typeToString(b.r.type)),
                                 attrsJson(b.r.type, b.r.level, b.r.lang, b.r.spans, b.r.taskState), b.content);
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
    fenwick_.setHeight(static_cast<size_t>(row), estimatedHeight(rows_[row]));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
}

void BlockModel::splitBlock(int row, int col) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
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
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, r.depth, QString::fromLatin1(typeToString(r.type)), QString(), right);
    if (!rightS.empty() || r.type != Paragraph) persistMeta(at);   // spans / list meta into attrs+depth

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::insertBlock(int row) {
    row = std::clamp(row, 0, static_cast<int>(rows_.size()));
    beginTxn(row, row - 1);                      // empty `before`; after = [row,row]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (row > 0) ? ranks_[row - 1] : QString(),
        (row < static_cast<int>(ranks_.size())) ? ranks_[row] : QString());

    beginInsertRows({}, row, row);
    Row r{}; r.type = Paragraph; r.param = 1;
    rows_.insert(rows_.begin() + row, r);
    content_.insert(content_.begin() + row, QString());
    ids_.insert(ids_.begin() + row, newId);
    ranks_.insert(ranks_.begin() + row, newRank);
    fenwick_.insert(static_cast<size_t>(row), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), QString(), QString());

    bumpLayout();
    ++contentRevision_;            // row→content mapping shifted: refresh content bindings
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::duplicateBlock(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
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
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)),
                         attrsJson(r.type, r.level, r.lang, r.spans, r.taskState), text);

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::removeBlock(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    beginTxn(row, row);                          // after = empty
    dropBlockInk(ids_[row]);   // DB side cascades with the block row (FK)
    if (doc_.isOpen()) doc_.deleteBlock(ids_[row]);
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    content_.erase(content_.begin() + row);
    ids_.erase(ids_.begin() + row);
    ranks_.erase(ranks_.begin() + row);
    fenwick_.erase(static_cast<size_t>(row));
    endRemoveRows();
    bumpLayout();
    ++contentRevision_;            // row→content mapping shifted: refresh content bindings
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::moveBlock(int from, int to) {
    const int n = static_cast<int>(rows_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    beginTxn(std::min(from, to), std::max(from, to));

    // Lift the block out (its content/type/spans travel with it).
    Row r = rows_[from];
    const QString id = ids_[from], content = content_[from];
    const double h = fenwick_.height(static_cast<size_t>(from));
    rows_.erase(rows_.begin() + from);
    content_.erase(content_.begin() + from);
    ids_.erase(ids_.begin() + from);
    ranks_.erase(ranks_.begin() + from);
    fenwick_.erase(static_cast<size_t>(from));

    // New fractional rank between the destination neighbours (reduced list).
    const int sz = static_cast<int>(ranks_.size());
    const QString prev = (to > 0)  ? ranks_[to - 1] : QString();
    const QString next = (to < sz) ? ranks_[to]     : QString();
    const QString newRank = rankBetween(prev, next);

    rows_.insert(rows_.begin() + to, r);
    content_.insert(content_.begin() + to, content);
    ids_.insert(ids_.begin() + to, id);
    ranks_.insert(ranks_.begin() + to, newRank);
    fenwick_.insert(static_cast<size_t>(to), h);

    if (doc_.isOpen()) doc_.updateRank(id, newRank);

    bumpLayout();                 // positions change; cells re-read yForRow/content
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}
