#include "CanvasDocument.h"
#include "BlockModel.h"
#include "MediaStore.h"
#include "PackageFormat.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <QUrl>

namespace {
QString g_scratchRoot;   // set once by main() to this session's subdir

// Recursively copy <src>/.minnotes → <dst>/.minnotes (pasted media). Referenced
// files live at absolute paths and need no copy. No-op if src == dst.
void copyDirRecursive(const QString& src, const QString& dst) {
    QDir sd(src);
    if (!sd.exists()) return;
    QDir().mkpath(dst);
    for (const QFileInfo& fi : sd.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString d = dst + QLatin1Char('/') + fi.fileName();
        if (fi.isDir()) copyDirRecursive(fi.absoluteFilePath(), d);
        else { QFile::remove(d); QFile::copy(fi.absoluteFilePath(), d); }
    }
}
void copyMediaSidecar(const QString& srcDocDir, const QString& dstDocDir) {
    if (srcDocDir == dstDocDir) return;
    copyDirRecursive(srcDocDir + QStringLiteral("/.minnotes"),
                     dstDocDir + QStringLiteral("/.minnotes"));
}
QString localPath(const QString& pathOrUrl) {
    return pathOrUrl.startsWith(QLatin1String("file:")) ? QUrl(pathOrUrl).toLocalFile() : pathOrUrl;
}
}

CanvasDocument::CanvasDocument(QObject* parent) : QObject(parent) {}

CanvasDocument::~CanvasDocument() {
    closeCanvas();
}

void CanvasDocument::setScratchRoot(const QString& dir) { g_scratchRoot = dir; }

QString CanvasDocument::scratchDir() {
    if (!g_scratchRoot.isEmpty()) return g_scratchRoot;
    // Fallback (e.g. unit tests that construct models without main()).
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
         + QStringLiteral("/scratch");
}

QString CanvasDocument::newScratchPath() {
    const QString dir = scratchDir();
    QDir().mkpath(dir);
    return dir + QStringLiteral("/work-") + makeUlid() + QStringLiteral(".mnd");
}

void CanvasDocument::cleanupScratch() {
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

QString CanvasDocument::mediaAnchorDir() const {
    return pkgDir_.isEmpty() ? QFileInfo(docPath_).absolutePath() : pkgDir_;
}

QString CanvasDocument::documentName() const {
    if (!documentOpen()) return QString();
    // Package views and converted 1.0 files are untitled (snapshot semantics: Save → Save As)
    // but keep the file's name — the tab should say what you're looking at.
    if (untitled_ && pkgDir_.isEmpty() && !readOnly_) return QStringLiteral("Untitled");
    return QFileInfo(docPath_).completeBaseName();   // basename without .mnd/.mnpkg
}

void CanvasDocument::recordOriginalStat() {
    QFileInfo fi(docPath_);
    if (!untitled_ && fi.exists()) {
        origMtime_ = fi.lastModified().toMSecsSinceEpoch();
        origSize_  = fi.size();
    } else { origMtime_ = -1; origSize_ = -1; }
}

bool CanvasDocument::externalChangeDetected() const {
    if (untitled_ || origMtime_ < 0) return false;   // no baseline → nothing to conflict with
    QFileInfo fi(docPath_);
    if (!fi.exists()) return false;                  // original gone → recreate, nothing to clobber
    return fi.lastModified().toMSecsSinceEpoch() != origMtime_ || fi.size() != origSize_;
}

void CanvasDocument::markDirty() {
    if (dirty_) return;
    dirty_ = true;
    emit dirtyChanged();
}

void CanvasDocument::setSaveState(int s) {
    if (saveState_ == s) return;
    saveState_ = s;
    emit saveStateChanged();
}

// ---------------------------------------------------------------------------
// Open / new / close

// The 2.0 gate. A file stamped "mnd2" at this schema or below opens; a newer one is refused (its
// fields would be lost on save); a 1.0 file ("mnd" v1) is ADOPTED on the working copy — the canvas
// schema, one page element holding every block — and opens READ-ONLY (Save routes to Save As, which
// writes a 2.0 file; the original is never touched). Anything else is refused with a reason.
bool CanvasDocument::acceptOpenedFormat() {
    const QString f = db_.format();
    const int v = db_.schemaVersion();
    if (f == QLatin1String(Document::kFormat)) {
        if (v > Document::kSchemaVersion) {
            lastOpenError_ = tr("This document was made with a newer minNotes and can't be opened.");
            db_.close();
            cleanupScratch();
            return false;
        }
        return true;
    }
    if (f == QLatin1String(Document::kFormatV1) && v == 1) {
        readOnly_ = true;
        return true;
    }
    lastOpenError_ = v > 0
        ? tr("This document was made with an earlier minNotes and can't be opened.")
        : tr("This file isn't a minNotes document.");
    db_.close();
    cleanupScratch();
    return false;
}

// Every open ends here: the canvas tables exist, at least one page element exists, and no block
// is page-less (a 1.0 file's rows, or rows a tool wrote by hand, all land on the first page).
void CanvasDocument::ensurePages() {
    db_.begin();
    db_.ensureCanvasSchema();
    if (pageIds().isEmpty()) {
        Document::Element p;
        p.id = makeUlid();
        p.kind = QStringLiteral("page");
        p.w = db_.pageWidth() + 2.0 * kPageInset;   // the 1.0 measure (760 when absent) sizes the page
        db_.insertElement(p);
        if (db_.canvasMeta(QStringLiteral("focusPage")).isEmpty())
            db_.setCanvasMeta(QStringLiteral("focusPage"), p.id);
    }
    db_.assignOrphanBlocks(firstPageId());
    db_.commit();
}

bool CanvasDocument::loadFile(const QString& path, bool untitled) {
    // We never run SQLite on the document's real location (network filesystems
    // can't back WAL/-shm or honour locking — see the plan). Stage a LOCAL working
    // copy and open THAT; the original is only ever read (byte copy) and written
    // (atomic replace on save). `path` stays the identity + media anchor.
    closeCanvas();
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
        // The clean break (R-I6 6b): a package from an earlier minNotes —
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
            qWarning() << "CanvasDocument: package has no readable document.mnd" << path;
            cleanupScratch();
            return false;
        }
        QFile::setPermissions(scratchPath_, QFile::ReadOwner | QFile::WriteOwner
                                          | QFile::ReadUser  | QFile::WriteUser);
        if (!db_.open(scratchPath_)) {
            qWarning() << "CanvasDocument: cannot open package working copy" << scratchPath_;
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
        // A background extraction landing bumps every page's contentRevision (queued to
        // the GUI thread) so display bindings re-resolve and reveal.
        {
            QPointer<CanvasDocument> self(this);
            mediaStore_->setLazyNotify([self] {
                if (self)
                    QMetaObject::invokeMethod(self, [self] {
                        if (!self) return;
                        for (BlockModel* m : self->pages_) m->refreshMedia();
                    }, Qt::QueuedConnection);
            });
        }
        ensurePages();
        recordOriginalStat();
        dirty_ = false;
        setSaveState(SaveClean);
        return true;
    }

    scratchPath_ = newScratchPath();
    if (!untitled && QFileInfo::exists(path)) {
        QFile::remove(scratchPath_);
        if (!QFile::copy(path, scratchPath_)) {
            qWarning() << "CanvasDocument: cannot stage working copy of" << path;
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
    if (!db_.open(scratchPath_)) {
        qWarning() << "CanvasDocument: cannot open working copy" << scratchPath_;
        cleanupScratch();
        return false;
    }
    if (!untitled && QFileInfo::exists(path) && !acceptOpenedFormat()) return false;
    docPath_ = path;
    untitled_ = untitled || readOnly_;   // a converted 1.0 file saves only through Save As
    mediaStore_ = std::make_unique<MediaStore>(path);   // media anchored to the ORIGINAL folder
    ensurePages();
    recordOriginalStat();
    dirty_ = false;
    setSaveState(SaveClean);
    return true;
}

bool CanvasDocument::newCanvas() {
    // Identity path for the untitled file — the media anchor + Save-As source dir.
    // The bytes live in the working copy (scratchPath_), not at this path.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/untitled-") + makeUlid() + QStringLiteral(".mnd");
    if (!loadFile(path, /*untitled*/true)) return false;
    // ensurePages() made the one page element; give it its empty paragraph and focus it
    // (ruling 6: a new canvas opens on its page so typing works at once).
    const QString page = firstPageId();
    db_.begin();
    seedPage(page);
    db_.commit();
    focusPage_ = page;
    dirty_ = false;                  // a pristine empty file isn't dirty (still untitled)
    setSaveState(SaveClean);
    ++pagesRevision_;
    emit pagesChanged();
    emit focusChanged();
    emit documentChanged();
    emit dirtyChanged();
    return true;
}

bool CanvasDocument::open(const QString& pathOrUrl) {
    const QString path = localPath(pathOrUrl);
    if (path.isEmpty() || !loadFile(path, /*untitled*/false)) return false;
    // The persisted focus page (if it still exists); otherwise the canvas is in view.
    const QString f = db_.canvasMeta(QStringLiteral("focusPage"));
    focusPage_ = pageIds().contains(f) ? f : QString();
    ++pagesRevision_;
    emit pagesChanged();
    emit focusChanged();
    emit documentChanged();
    emit dirtyChanged();
    return true;
}

void CanvasDocument::detachAllPages() {
    const QList<BlockModel*> models = pages_.values();   // detachPage() erases from pages_
    for (BlockModel* m : models) {
        m->detachPage();
        if (m->parent() == this) m->deleteLater();
    }
    pages_.clear();
}

void CanvasDocument::closeCanvas() {
    const bool wasOpen = db_.isOpen();
    detachAllPages();
    db_.close();
    cleanupScratch();            // working copy is ephemeral — discard it on close
    mediaStore_.reset();
    docPath_.clear();
    untitled_ = false;
    readOnly_ = false;
    focusPage_.clear();
    dirty_ = false;
    setSaveState(SaveClean);
    if (wasOpen) {
        ++pagesRevision_;
        emit pagesChanged();
        emit focusChanged();
        emit documentChanged();
        emit dirtyChanged();
    }
}

// ---------------------------------------------------------------------------
// Save

// Write the working copy back to the original. checkpoint → VACUUM INTO a sibling
// temp (a clean, compacted, rollback-stamped image — also normalises away any WAL
// stamp) → atomic replace over the original. The working copy keeps the live data
// if any step fails, so nothing is lost.
bool CanvasDocument::writeBackToOriginal() {
    setSaveState(SaveSaving);
    db_.setCanvasMeta(QStringLiteral("focusPage"), focusPage_);
    db_.stampMeta();     // saved files record schema_version/app_version/modified
    db_.checkpoint();
    const QString dir = QFileInfo(docPath_).absolutePath();
    QDir().mkpath(dir);
    const QString tmp = dir + QStringLiteral("/.mn-save-") + makeUlid() + QStringLiteral(".tmp");
    QFile::remove(tmp);
    if (!db_.vacuumInto(tmp)) {
        qWarning() << "CanvasDocument: save VACUUM INTO failed" << tmp;
        QFile::remove(tmp);
        setSaveState(SaveFailed);
        return false;
    }
    // A legacy WAL-stamped original could have an orphaned -wal/-shm that would
    // shadow our fresh rollback file on the next open — drop them.
    QFile::remove(docPath_ + QStringLiteral("-wal"));
    QFile::remove(docPath_ + QStringLiteral("-shm"));
    if (!mnpkg::atomicReplace(tmp, docPath_)) {
        qWarning() << "CanvasDocument: save atomic replace failed" << docPath_;
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

bool CanvasDocument::save() {
    if (untitled_) return false;                       // no chosen path (or read-only) → caller invokes Save As
    if (externalChangeDetected()) {                    // changed on disk since we opened it
        setSaveState(SaveConflict);
        return false;                                  // caller resolves (Overwrite / Save As copy)
    }
    return writeBackToOriginal();
}

bool CanvasDocument::overwriteSave() {
    if (untitled_) return false;
    return writeBackToOriginal();                      // bypass the conflict check ("Overwrite")
}

bool CanvasDocument::saveAs(const QString& pathOrUrl) {
    if (!db_.isOpen()) return false;
    QString path = localPath(pathOrUrl);
    if (path.isEmpty()) return false;
    if (!path.endsWith(QLatin1String(".mnd"), Qt::CaseInsensitive)) path += QStringLiteral(".mnd");
    const QString srcMediaDir = mediaAnchorDir();   // package docs: the extraction dir
    const QString dstDir = QFileInfo(path).absolutePath();
    QDir().mkpath(dstDir);
    setSaveState(SaveSaving);
    // Flush the WAL, then write a clean copy to the chosen location (temp + atomic
    // replace, matching the in-place save path).
    db_.setCanvasMeta(QStringLiteral("focusPage"), focusPage_);
    db_.stampMeta();     // saved files record schema_version/app_version/modified (a 1.0 file becomes 2.0 here)
    db_.checkpoint();
    const QString tmp = dstDir + QStringLiteral("/.mn-save-") + makeUlid() + QStringLiteral(".tmp");
    QFile::remove(tmp);
    if (!db_.vacuumInto(tmp)) {
        qWarning() << "CanvasDocument: Save As VACUUM INTO failed" << tmp;
        QFile::remove(tmp);
        setSaveState(SaveFailed);
        return false;
    }
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
    if (!mnpkg::atomicReplace(tmp, path)) {
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
    readOnly_ = false;   // the saved file is a 2.0 file
    mediaStore_ = std::make_unique<MediaStore>(path);
    recordOriginalStat();
    dirty_ = false;
    setSaveState(SaveClean);
    emit documentChanged();
    emit dirtyChanged();
    return true;
}

bool CanvasDocument::snapshotTo(const QString& path) {
    if (!db_.isOpen()) return false;
    db_.setCanvasMeta(QStringLiteral("focusPage"), focusPage_);
    db_.stampMeta();
    db_.checkpoint();
    QFile::remove(path);
    return db_.vacuumInto(path);
}

// ---------------------------------------------------------------------------
// Pages

QStringList CanvasDocument::pageIds() const {
    QStringList out;
    if (!db_.isOpen()) return out;
    for (const Document::Element& e : db_.allElements())
        if (e.kind == QLatin1String("page")) out << e.id;
    return out;
}

QString CanvasDocument::firstPageId() const {
    const QStringList ids = pageIds();
    return ids.isEmpty() ? QString() : ids.first();
}

QString CanvasDocument::insertPageElement() {
    double right = 0.0;
    int z = 0;
    bool any = false;
    for (const Document::Element& e : db_.allElements()) {
        if (e.kind != QLatin1String("page")) continue;
        right = any ? std::max(right, e.x + e.w) : e.x + e.w;
        z = std::max(z, e.z + 1);
        any = true;
    }
    Document::Element p;
    p.id = makeUlid();
    p.kind = QStringLiteral("page");
    p.x = any ? right + kPageGap : 0.0;
    p.y = 0.0;
    p.w = kDefaultMeasure + 2.0 * kPageInset;
    p.h = 0.0;
    p.z = z;
    db_.insertElement(p);
    return p.id;
}

void CanvasDocument::seedPage(const QString& pageId) {
    // content is NOT NULL — bind a non-null empty string (a null QString would
    // violate the constraint and silently drop the block, leaving an empty page).
    db_.scoped(pageId).appendBlock(makeUlid(), BlockModel::rankBetween(QString(), QString()), 0,
                                   QStringLiteral("paragraph"), QString(), QStringLiteral(""));
}

QString CanvasDocument::addPage() {
    if (!db_.isOpen()) return QString();
    db_.begin();
    const QString id = insertPageElement();
    seedPage(id);
    db_.commit();
    markDirty();
    ++pagesRevision_;
    emit pagesChanged();
    return id;
}

bool CanvasDocument::removePage(const QString& id) {
    if (!db_.isOpen()) return false;
    const QStringList ids = pageIds();
    if (ids.size() <= 1 || !ids.contains(id)) return false;   // a canvas always keeps one page
    if (BlockModel* m = pages_.value(id, nullptr)) {
        m->detachPage();                                        // erases itself from pages_
        if (m->parent() == this) m->deleteLater();
    }
    db_.deleteElement(id);                                      // the page's blocks cascade
    if (focusPage_ == id) {
        focusPage_ = firstPageId();
        emit focusChanged();
    }
    markDirty();
    ++pagesRevision_;
    emit pagesChanged();
    return true;
}

QString CanvasDocument::pageTitle(const QString& id) const {
    if (!db_.isOpen()) return QString();
    const Document page = db_.scoped(id);
    QString firstText;
    for (const Document::BlockMeta& bm : page.skinnyScan()) {
        if (bm.type == QLatin1String("heading")) {
            const QString t = page.contentFor(bm.id).section(QLatin1Char('\n'), 0, 0).trimmed();
            if (!t.isEmpty()) return t;
        }
        if (firstText.isEmpty() && bm.type != QLatin1String("media") && bm.type != QLatin1String("split")
            && bm.type != QLatin1String("divider")) {
            const QString t = page.contentFor(bm.id).section(QLatin1Char('\n'), 0, 0).trimmed();
            if (!t.isEmpty()) firstText = t.left(60);
        }
    }
    return firstText.isEmpty() ? tr("Untitled page") : firstText;
}

BlockModel* CanvasDocument::pageModel(const QString& id) {
    if (!db_.isOpen() || id.isEmpty()) return nullptr;
    if (BlockModel* m = pages_.value(id, nullptr)) return m;
    const auto e = db_.element(id);
    if (!e || e->kind != QLatin1String("page")) return nullptr;
    auto* m = new BlockModel(this);
    m->attachPage(this, id);   // registers itself via adoptPageModel
    return m;
}

void CanvasDocument::adoptPageModel(BlockModel* m, const QString& id) {
    if (!m || id.isEmpty()) return;
    pages_.insert(id, m);
}

void CanvasDocument::forgetPageModel(BlockModel* m) {
    for (auto it = pages_.begin(); it != pages_.end();) {
        if (it.value() == m) it = pages_.erase(it);
        else ++it;
    }
}

void CanvasDocument::setFocusPage(const QString& id) {
    const QString next = (id.isEmpty() || pageIds().contains(id)) ? id : focusPage_;
    if (next == focusPage_) return;
    focusPage_ = next;
    emit focusChanged();
}

QString CanvasDocument::focusedPageId() const {
    return focusPage_.isEmpty() ? firstPageId() : focusPage_;
}

BlockModel* CanvasDocument::focusedModel() {
    return pageModel(focusedPageId());
}
