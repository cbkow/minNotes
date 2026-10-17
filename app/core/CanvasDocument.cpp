#include "CanvasDocument.h"
#include "BlockModel.h"
#include "Importer.h"
#include "MediaStore.h"
#include "PackageFormat.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>
#include <algorithm>
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
    reloadElements();
    if (pageIds().isEmpty()) {
        Document::Element p;
        p.id = makeUlid();
        p.kind = QStringLiteral("page");
        p.w = db_.pageWidth() + 2.0 * kPageInset;   // the 1.0 measure (760 when absent) sizes the page
        p.h = kMinPageHeight;                        // an empty page still reads as a page
        db_.insertElement(p);
        if (db_.canvasMeta(QStringLiteral("focusPage")).isEmpty())
            db_.setCanvasMeta(QStringLiteral("focusPage"), p.id);
    }
    reloadElements();
    db_.assignOrphanBlocks(firstPageId());
    db_.commit();
}

void CanvasDocument::finishOpen() {
    ensurePages();
    if (readOnly_) mn::convertV1(*this, firstPageId(), report_);
    reloadElements();
    clearUndo();
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
                        if (self) self->refreshMedia();
                    }, Qt::QueuedConnection);
            });
        }
        finishOpen();
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
    finishOpen();
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
    clearUndo();                     // a fresh canvas has no history
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
    {
        const QJsonObject c = QJsonDocument::fromJson(db_.canvasMeta(QStringLiteral("camera")).toUtf8()).object();
        camValid_ = c.contains(QStringLiteral("z")) && c.value(QStringLiteral("z")).toDouble() > 0;
        if (camValid_) { camX_ = c.value(QStringLiteral("x")).toDouble(); camY_ = c.value(QStringLiteral("y")).toDouble(); camZ_ = c.value(QStringLiteral("z")).toDouble(); }
        emit cameraChanged();
    }
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
    elements_.clear();
    indexById_.clear();
    titleCache_.clear();
    clearUndo();
    cleanupScratch();            // working copy is ephemeral — discard it on close
    mediaStore_.reset();
    docPath_.clear();
    untitled_ = false;
    readOnly_ = false;
    report_ = mn::ConversionReport{};
    focusPage_.clear();
    camValid_ = false;
    emit cameraChanged();
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
    persistViewMeta();
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
    persistViewMeta();
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
    persistViewMeta();
    db_.stampMeta();
    db_.checkpoint();
    QFile::remove(path);
    return db_.vacuumInto(path);
}

// ---------------------------------------------------------------------------
// Pages

QStringList CanvasDocument::pageIds() const {
    QStringList out;
    for (const Document::Element& e : elements_)
        if (e.kind == QLatin1String("page")) out << e.id;
    return out;
}

QString CanvasDocument::firstPageId() const {
    for (const Document::Element& e : elements_)
        if (e.kind == QLatin1String("page")) return e.id;
    return QString();
}

int CanvasDocument::nextZ() const {
    int z = 0;
    for (const Document::Element& e : elements_) z = std::max(z, e.z + 1);
    return z;
}

QString CanvasDocument::insertPageElement() {
    double right = 0.0;
    bool any = false;
    for (const Document::Element& e : elements_) {
        if (e.kind != QLatin1String("page")) continue;
        right = any ? std::max(right, e.x + e.w) : e.x + e.w;
        any = true;
    }
    return insertPageElementAt(QRectF(any ? right + kPageGap : 0.0, 0.0, kDefaultMeasure + 2.0 * kPageInset, kMinPageHeight));
}

QString CanvasDocument::insertPageElementAt(const QRectF& r) {
    Document::Element p;
    p.id = makeUlid();
    p.kind = QStringLiteral("page");
    p.x = r.x();
    p.y = r.y();
    p.w = std::clamp(r.width(), kMinPageMeasure + 2.0 * kPageInset, kMaxPageMeasure + 2.0 * kPageInset);
    p.h = std::max(r.height(), kMinPageHeight);
    p.z = nextZ();
    db_.insertElement(p);
    return p.id;
}

void CanvasDocument::seedPage(const QString& pageId) {
    // content is NOT NULL — bind a non-null empty string (a null QString would
    // violate the constraint and silently drop the block, leaving an empty page).
    db_.scoped(pageId).appendBlock(makeUlid(), BlockModel::rankBetween(QString(), QString()), 0,
                                   QStringLiteral("paragraph"), QString(), QStringLiteral(""));
}

QString CanvasDocument::addPage() { return addPageWithRect(QRectF()); }

QString CanvasDocument::addPageAt(qreal wx, qreal wy, qreal measure) {
    const double m = std::clamp(measure > 0 ? measure : double(kDefaultMeasure), kMinPageMeasure, kMaxPageMeasure);
    return addPageWithRect(QRectF(wx, wy, m + 2.0 * kPageInset, kMinPageHeight));
}

// A null rect = right of the other pages (the "+" tab); a rect = the canvas gesture's point.
QString CanvasDocument::addPageWithRect(const QRectF& r) {
    if (!db_.isOpen()) return QString();
    db_.begin();
    const QString id = r.isNull() ? insertPageElement() : insertPageElementAt(r);
    seedPage(id);
    db_.commit();
    reloadElements();
    UndoEntry e;
    e.id = id;
    e.after = snapshot(id, /*withBlocks*/true);
    e.label = tr("Add page");
    pushEntry(std::move(e), QString());
    noteChanged(QStringLiteral("page"), true, true, false);
    return id;
}

bool CanvasDocument::removePage(const QString& id) {
    if (!db_.isOpen()) return false;
    const QStringList ids = pageIds();
    if (ids.size() <= 1 || !ids.contains(id)) return false;   // a canvas always keeps one page
    UndoEntry e;
    e.id = id;
    e.before = snapshot(id, /*withBlocks*/true);
    e.label = tr("Remove page");
    applySnap(id, ElementSnap{});
    pushEntry(std::move(e), QString());
    noteChanged(QStringLiteral("page"), true, true, false);
    return true;
}

QString CanvasDocument::pageTitle(const QString& id) const {
    if (!db_.isOpen()) return QString();
    if (const BlockModel* m = pages_.value(id, nullptr)) {
        const QString t = m->pageTitleHint();
        return t.isEmpty() ? tr("Untitled page") : t;
    }
    const auto cached = titleCache_.constFind(id);
    if (cached != titleCache_.constEnd()) return cached.value();
    const Document page = db_.scoped(id);
    QString firstText;
    for (const Document::BlockMeta& bm : page.skinnyScan()) {
        if (bm.type == QLatin1String("heading")) {
            const QString t = page.contentFor(bm.id).section(QLatin1Char('\n'), 0, 0).trimmed();
            if (!t.isEmpty()) { firstText = t; break; }
        }
        if (firstText.isEmpty() && bm.type != QLatin1String("media") && bm.type != QLatin1String("split")
            && bm.type != QLatin1String("divider")) {
            const QString t = page.contentFor(bm.id).section(QLatin1Char('\n'), 0, 0).trimmed();
            if (!t.isEmpty()) firstText = t.left(60);
        }
    }
    const QString title = firstText.isEmpty() ? tr("Untitled page") : firstText;
    titleCache_.insert(id, title);
    return title;
}

QVariantList CanvasDocument::orphanThreads() const {
    QVariantList out;
    if (!db_.isOpen()) return out;
    for (const Document::CommentThread& t : db_.commentThreads()) {
        if (!db_.pagesMentioning(t.id).isEmpty()) continue;
        QVariantMap m;
        m.insert(QStringLiteral("id"), t.id);
        m.insert(QStringLiteral("resolved"), t.resolved);
        m.insert(QStringLiteral("created"), t.created);
        m.insert(QStringLiteral("row"), -1);
        m.insert(QStringLiteral("excerpt"), QString());
        out.append(m);
    }
    return out;
}

BlockModel* CanvasDocument::pageModel(const QString& id) {
    if (!db_.isOpen() || id.isEmpty()) return nullptr;
    if (BlockModel* m = pages_.value(id, nullptr)) return m;
    const auto e = element(id);
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

void CanvasDocument::refreshMedia() {
    for (BlockModel* m : pages_) m->refreshMedia();
}

int CanvasDocument::rewriteMediaElementSrcs(const QHash<QString, QString>& absToRel) {
    if (!db_.isOpen() || !mediaStore_ || absToRel.isEmpty()) return 0;
    int count = 0;
    db_.begin();
    for (const Document::Element& e : elements_) {
        if (e.kind != QLatin1String("media")) continue;
        QJsonObject root = QJsonDocument::fromJson(e.json.toUtf8()).object();
        QJsonObject media = root.value(QStringLiteral("media")).toObject();
        const QString url = mediaStore_->resolveUrl(media.value(QStringLiteral("src")));
        if (url.isEmpty() || !url.startsWith(QLatin1String("file:"))) continue;
        const auto it = absToRel.constFind(QDir::cleanPath(QUrl(url).toLocalFile()));
        if (it == absToRel.constEnd()) continue;
        media.insert(QStringLiteral("src"), it.value());
        root.insert(QStringLiteral("media"), media);
        db_.updateElementJson(e.id, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
        ++count;
    }
    db_.commit();
    if (count) { reloadElements(); noteChanged(QStringLiteral("media"), true, false, false); }
    return count;
}

// ---------------------------------------------------------------------------
// Elements: the mirror

void CanvasDocument::reloadElements() {
    titleCache_.clear();   // pages may have come or gone (undo of a removal restores rows)
    elements_ = db_.isOpen() ? db_.allElements() : std::vector<Document::Element>{};
    indexById_.clear();
    for (int i = 0; i < static_cast<int>(elements_.size()); ++i) indexById_.insert(elements_[size_t(i)].id, i);
}

std::optional<Document::Element> CanvasDocument::element(const QString& id) const {
    const int i = indexById_.value(id, -1);
    if (i < 0) return std::nullopt;
    return elements_[size_t(i)];
}

QString CanvasDocument::elementIdAt(int i) const {
    return i >= 0 && i < static_cast<int>(elements_.size()) ? elements_[size_t(i)].id : QString();
}
QString CanvasDocument::elementKind(const QString& id) const { const auto e = element(id); return e ? e->kind : QString(); }
QRectF CanvasDocument::elementRect(const QString& id) const { const auto e = element(id); return e ? QRectF(e->x, e->y, e->w, e->h) : QRectF(); }
QString CanvasDocument::elementJson(const QString& id) const { const auto e = element(id); return e ? e->json : QString(); }
int CanvasDocument::elementZ(const QString& id) const { const auto e = element(id); return e ? e->z : 0; }

double CanvasDocument::pageHeightCap(double elementWidth) {
    const double measure = std::max(kMinPageMeasure, elementWidth - 2.0 * kPageInset);
    return std::round(measure * 1.41421356237);
}

double CanvasDocument::clampPageHeight(double elementWidth, double h) {
    return std::clamp(h, kMinPageHeight, std::max(kMinPageHeight, pageHeightCap(elementWidth)));
}

QRectF CanvasDocument::cullRect(const Document::Element& e) {
    const double minH = e.kind == QLatin1String("page") ? 2.0 * kPageInset + 1.0 : 1.0;
    return QRectF(e.x, e.y, std::max(e.w, 1.0), std::max(e.h, minH));
}

QStringList CanvasDocument::visibleElements(qreal x0, qreal y0, qreal x1, qreal y1) const {
    const QRectF view(QPointF(std::min(x0, x1), std::min(y0, y1)), QPointF(std::max(x0, x1), std::max(y0, y1)));
    QStringList out;
    for (const Document::Element& e : elements_)
        if (cullRect(e).intersects(view)) out << e.id;
    return out;
}

QList<int> CanvasDocument::visibleElementIndices(qreal x0, qreal y0, qreal x1, qreal y1) const {
    const QRectF view(QPointF(std::min(x0, x1), std::min(y0, y1)), QPointF(std::max(x0, x1), std::max(y0, y1)));
    QList<int> out;
    for (int i = 0; i < static_cast<int>(elements_.size()); ++i) {
        if (cullRect(elements_[size_t(i)]).intersects(view)) out << i;
    }
    return out;
}

void CanvasDocument::cachePageHeight(const QString& id, qreal h) {
    const int i = indexById_.value(id, -1);
    if (i < 0 || !db_.isOpen()) return;
    Document::Element& e = elements_[size_t(i)];
    h = clampPageHeight(e.w, h);       // an empty page still reads as a page; a long one stops at a page length
    if (e.kind != QLatin1String("page") || std::abs(e.h - h) < 1.0) return;
    e.h = h;
    db_.updateElementRect(id, e.x, e.y, e.w, h);
    ++geometryRevision_;
    emit geometryChanged();
}

QVariantMap CanvasDocument::camera() const {
    QVariantMap m;
    m.insert(QStringLiteral("x"), camX_);
    m.insert(QStringLiteral("y"), camY_);
    m.insert(QStringLiteral("z"), camZ_);
    m.insert(QStringLiteral("valid"), camValid_);
    return m;
}

void CanvasDocument::setCamera(qreal x, qreal y, qreal z) {
    if (z <= 0) return;
    const bool was = camValid_;
    camValid_ = true;
    camX_ = x; camY_ = y; camZ_ = z;
    if (!was) emit cameraChanged();
}

void CanvasDocument::persistViewMeta() {
    db_.setCanvasMeta(QStringLiteral("focusPage"), focusPage_);
    if (camValid_) {
        QJsonObject c;
        c.insert(QStringLiteral("x"), camX_);
        c.insert(QStringLiteral("y"), camY_);
        c.insert(QStringLiteral("z"), camZ_);
        db_.setCanvasMeta(QStringLiteral("camera"), QString::fromUtf8(QJsonDocument(c).toJson(QJsonDocument::Compact)));
    }
}

QRectF CanvasDocument::canvasBounds() const {
    QRectF b;
    for (const Document::Element& e : elements_) {
        const QRectF r = cullRect(e);
        b = b.isNull() ? r : b.united(r);
    }
    return b;
}

QString CanvasDocument::mediaElementInk(const QString& id, int page) const {
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("media")) return QString();
    const QJsonObject ink = QJsonDocument::fromJson(e->json.toUtf8()).object().value(QStringLiteral("ink")).toObject();
    const QJsonValue v = ink.value(QString::number(page));
    return v.isObject() ? QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact)) : QString();
}

QString CanvasDocument::resolveMediaUrl(const QVariant& src) const {
    if (!mediaStore_ || src.isNull()) return QString();
    return mediaStore_->resolveUrlAsync(QJsonValue::fromVariant(src));
}
QString CanvasDocument::resolveMediaPath(const QVariant& src) const {
    const QString url = resolveMediaUrl(src);
    return url.isEmpty() ? QString() : QUrl(url).toLocalFile();
}
QString CanvasDocument::mediaPlaybackSource(const QVariant& src) const {
    if (!mediaStore_ || src.isNull()) return QString();
    return mediaStore_->playbackSourceFor(QJsonValue::fromVariant(src));
}
bool CanvasDocument::mediaExtracting(const QVariant& src) const {
    if (!mediaStore_ || src.isNull()) return false;
    return mediaStore_->extractionPending(src.toString());
}

// ---------------------------------------------------------------------------
// Elements: mutators

namespace {
QString kindLabel(const QString& kind) {
    if (kind == QLatin1String("page")) return CanvasDocument::tr("page");
    if (kind == QLatin1String("media")) return CanvasDocument::tr("media");
    if (kind == QLatin1String("ink")) return CanvasDocument::tr("ink");
    if (kind == QLatin1String("shape")) return CanvasDocument::tr("shape");
    if (kind == QLatin1String("connector")) return CanvasDocument::tr("connector");
    return kind;
}
}

void CanvasDocument::noteChanged(const QString& kind, bool set, bool geometry, bool ink) {
    markDirty();
    if (set) { ++elementsRevision_; emit elementsChanged(); }
    if (geometry) { ++geometryRevision_; emit geometryChanged(); }
    if (ink) { ++inkRevision_; emit inkChanged(); }
    if (kind == QLatin1String("page") && set) { ++pagesRevision_; emit pagesChanged(); }
}

QString CanvasDocument::addElement(const QString& kind, const QRectF& r, const QString& json, const QString& label) {
    if (!db_.isOpen()) return QString();
    Document::Element e;
    e.id = makeUlid();
    e.kind = kind;
    e.x = r.x(); e.y = r.y(); e.w = r.width(); e.h = r.height();
    e.z = nextZ();
    e.json = json.isEmpty() ? QStringLiteral("{}") : json;
    db_.begin();
    db_.insertElement(e);
    db_.commit();
    reloadElements();
    UndoEntry u;
    u.id = e.id;
    u.after = snapshot(e.id, false);
    u.label = label;
    pushEntry(std::move(u), QString());
    noteChanged(kind, true, true, kind == QLatin1String("ink"));
    return e.id;
}

QString CanvasDocument::addMediaElement(const QString& mediaDescriptorJson, const QRectF& rect) {
    QJsonObject o;
    o.insert(QStringLiteral("media"), QJsonDocument::fromJson(mediaDescriptorJson.toUtf8()).object());
    return addElement(QStringLiteral("media"), rect, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)), tr("Add media"));
}
QString CanvasDocument::addInkElement(const QString& inkJson, const QRectF& rect) {
    return addElement(QStringLiteral("ink"), rect, inkJson, tr("Draw"));
}
QString CanvasDocument::addShapeElement(const QString& shapeJson, const QRectF& rect) {
    return addElement(QStringLiteral("shape"), rect, shapeJson, tr("Add shape"));
}
QString CanvasDocument::addMediaElementFromUrl(const QString& fileUrl, qreal wx, qreal wy) {
    if (!db_.isOpen() || !mediaStore_) return QString();
    auto sized = [&](int w, int h) {   // the drop width, the media's aspect
        const double dw = kDropMediaWidth;
        const double dh = (w > 0 && h > 0) ? dw * h / w : dw * 0.75;
        return QRectF(wx, wy, dw, dh);
    };
    if (MediaStore::isVideoPath(fileUrl)) {
        const MediaStore::VideoRef ref = mediaStore_->importVideoFile(fileUrl);
        if (ref.ok()) return addMediaElement(MediaStore::videoDescriptorJson(ref), sized(ref.w, ref.h));
    }
    if (MediaStore::isPdfPath(fileUrl)) {
        const MediaStore::PdfRef ref = mediaStore_->importPdfFile(fileUrl);
        if (ref.ok()) return addMediaElement(MediaStore::pdfDescriptorJson(ref), sized(ref.w, ref.h));
    }
    {
        const MediaStore::ImageRef ref = mediaStore_->importFile(fileUrl, /*forceCopy*/false);
        if (ref.ok()) {
            const double dw = std::min<double>(kDropMediaWidth, ref.w);   // a small image stays its size
            return addMediaElement(MediaStore::imageDescriptorJson(ref), QRectF(wx, wy, dw, dw * ref.h / ref.w));
        }
    }
    if (!Importer::formatForPath(fileUrl).isEmpty() || mnpkg::isPackagePath(fileUrl)) {
        emit importFileRequested(fileUrl, wx, wy);
        return QString();
    }
    const QString path = fileUrl.startsWith(QLatin1String("file:")) ? QUrl(fileUrl).toLocalFile() : fileUrl;
    if (path.isEmpty()) return QString();
    return addMediaElement(MediaStore::fileDescriptorJson(path), QRectF(wx, wy, 260, 44));
}

QString CanvasDocument::addConnector(const QString& connectorJson) {
    return addElement(QStringLiteral("connector"), QRectF(), connectorJson, tr("Connect"));
}

bool CanvasDocument::setElementRect(const QString& id, const QRectF& rectIn, const QString& coalesce) {
    const auto cur = element(id);
    if (!cur) return false;
    QRectF rect = rectIn;
    if (cur->kind == QLatin1String("page")) {   // a page's measure stays readable: [400, 4000] + the insets; its height is the content's, within the cap
        rect.setWidth(std::clamp(rect.width(), kMinPageMeasure + 2.0 * kPageInset, kMaxPageMeasure + 2.0 * kPageInset));
        rect.setHeight(clampPageHeight(rect.width(), rect.height()));
    }
    if (QRectF(cur->x, cur->y, cur->w, cur->h) == rect) return true;
    UndoEntry u;
    u.id = id;
    u.before = snapshot(id, false);
    const bool resize = std::abs(rect.width() - cur->w) > 0.01 || std::abs(rect.height() - cur->h) > 0.01;
    u.label = (resize ? tr("Resize %1") : tr("Move %1")).arg(kindLabel(cur->kind));
    db_.updateElementRect(id, rect.x(), rect.y(), rect.width(), rect.height());
    reloadElements();
    u.after = snapshot(id, false);
    pushEntry(std::move(u), coalesce);
    noteChanged(cur->kind, false, true, false);
    return true;
}

bool CanvasDocument::setElementJson(const QString& id, const QString& json, const QString& coalesce) {
    const auto cur = element(id);
    if (!cur) return false;
    const QString next = json.isEmpty() ? QStringLiteral("{}") : json;
    if (cur->json == next) return true;
    UndoEntry u;
    u.id = id;
    u.before = snapshot(id, false);
    u.label = cur->kind == QLatin1String("ink") ? tr("Edit ink") : tr("Edit %1").arg(kindLabel(cur->kind));
    db_.updateElementJson(id, next);
    reloadElements();
    u.after = snapshot(id, false);
    pushEntry(std::move(u), coalesce);
    const bool ink = cur->kind == QLatin1String("ink")
                  || (cur->kind == QLatin1String("media")
                      && QJsonDocument::fromJson(cur->json.toUtf8()).object().value(QStringLiteral("ink"))
                             != QJsonDocument::fromJson(next.toUtf8()).object().value(QStringLiteral("ink")));
    noteChanged(cur->kind, true, false, ink);
    return true;
}

bool CanvasDocument::setElementZ(const QString& id, int z) {
    const auto cur = element(id);
    if (!cur) return false;
    if (cur->z == z) return true;
    UndoEntry u;
    u.id = id;
    u.before = snapshot(id, false);
    u.label = tr("Reorder %1").arg(kindLabel(cur->kind));
    db_.updateElementZ(id, z);
    reloadElements();
    u.after = snapshot(id, false);
    pushEntry(std::move(u), QString());
    noteChanged(cur->kind, true, false, false);
    return true;
}

bool CanvasDocument::bringToFront(const QString& id) {
    const auto cur = element(id);
    if (!cur) return false;
    const int top = nextZ() - 1;
    return cur->z == top ? true : setElementZ(id, top + 1);
}

bool CanvasDocument::sendToBack(const QString& id) {
    const auto cur = element(id);
    if (!cur) return false;
    int bottom = 0;
    for (const Document::Element& e : elements_) bottom = std::min(bottom, e.z);
    return cur->z == bottom && elements_.size() > 1 && elements_.front().id == id ? true : setElementZ(id, bottom - 1);
}

QString CanvasDocument::duplicateElement(const QString& id) {
    const auto cur = element(id);
    if (!cur || cur->kind == QLatin1String("page")) return QString();
    return addElement(cur->kind, QRectF(cur->x + 24, cur->y + 24, cur->w, cur->h), cur->json, tr("Duplicate %1").arg(kindLabel(cur->kind)));
}

bool CanvasDocument::removeElement(const QString& id) {
    const auto cur = element(id);
    if (!cur) return false;
    if (cur->kind == QLatin1String("page")) return removePage(id);
    UndoEntry u;
    u.id = id;
    u.before = snapshot(id, false);
    u.label = tr("Remove %1").arg(kindLabel(cur->kind));
    applySnap(id, ElementSnap{});
    pushEntry(std::move(u), QString());
    noteChanged(cur->kind, true, true, cur->kind == QLatin1String("ink"));
    return true;
}

bool CanvasDocument::setMediaElementInk(const QString& id, int page, const QString& envelope) {
    const auto cur = element(id);
    if (!cur || cur->kind != QLatin1String("media")) return false;
    QJsonObject root = QJsonDocument::fromJson(cur->json.toUtf8()).object();
    QJsonObject ink = root.value(QStringLiteral("ink")).toObject();
    const QString key = QString::number(page);
    const QJsonObject env = QJsonDocument::fromJson(envelope.toUtf8()).object();
    const bool empty = envelope.isEmpty() || env.value(QStringLiteral("shapes")).toArray().isEmpty();
    if (empty) ink.remove(key); else ink.insert(key, env);
    if (ink.isEmpty()) root.remove(QStringLiteral("ink")); else root.insert(QStringLiteral("ink"), ink);
    return setElementJson(id, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)), QString());
}

// ---------------------------------------------------------------------------
// The canvas undo

CanvasDocument::ElementSnap CanvasDocument::snapshot(const QString& id, bool withBlocks) const {
    ElementSnap s;
    const auto e = element(id);
    if (!e) return s;
    s.present = true;
    s.el = *e;
    if (withBlocks && e->kind == QLatin1String("page")) s.blocks = db_.rawBlocksOfPage(id);
    return s;
}

void CanvasDocument::detachPageModel(const QString& id) {
    if (BlockModel* m = pages_.value(id, nullptr)) {
        m->detachPage();                                        // erases itself from pages_
        if (m->parent() == this) m->deleteLater();
    }
}

// Make the store match `s` for element `id`: absent → delete (a page's blocks cascade, its model
// detaches); present → update in place, or re-insert with its raw blocks when it is gone.
void CanvasDocument::applySnap(const QString& id, const ElementSnap& s) {
    const auto cur = element(id);
    db_.begin();
    if (!s.present) {
        if (cur) {
            if (cur->kind == QLatin1String("page")) {
                if (pageIds().size() <= 1) { db_.commit(); return; }   // never the last page
                detachPageModel(id);
            }
            db_.deleteElement(id);
        }
    } else if (cur) {
        if (cur->x != s.el.x || cur->y != s.el.y || cur->w != s.el.w || cur->h != s.el.h)
            db_.updateElementRect(id, s.el.x, s.el.y, s.el.w, s.el.h);
        if (cur->json != s.el.json) db_.updateElementJson(id, s.el.json);
        if (cur->z != s.el.z) db_.updateElementZ(id, s.el.z);
        if (cur->parentId != s.el.parentId) db_.updateElementParent(id, s.el.parentId);
    } else {
        db_.insertElement(s.el);
        if (!s.blocks.empty()) db_.restoreRawBlocks(s.blocks);
    }
    db_.commit();
    reloadElements();
    if (!focusPage_.isEmpty() && !indexById_.contains(focusPage_)) {
        focusPage_ = firstPageId();
        emit focusChanged();
    }
}

void CanvasDocument::beginBatch(const QString& label) {
    if (batchDepth_++ == 0) { batch_ = UndoEntry{}; batch_.label = label; }
}

void CanvasDocument::endBatch() {
    if (batchDepth_ <= 0) return;
    if (--batchDepth_ > 0) return;
    UndoEntry b = std::move(batch_);
    batch_ = UndoEntry{};
    if (b.parts.empty()) return;
    if (b.parts.size() == 1) {   // a batch of one part is that part — with its own key, so a run of nudges still coalesces
        UndoEntry one = std::move(b.parts.front());
        const QString key = one.coalesce;
        pushEntry(std::move(one), key);
        return;
    }
    b.ts = QDateTime::currentMSecsSinceEpoch();
    b.parent = undoCur_;
    undo_.push_back(std::move(b));
    undoCur_ = static_cast<int>(undo_.size()) - 1;
    ++undoRev_;
    emit undoStackChanged();
}

void CanvasDocument::pushEntry(UndoEntry e, const QString& coalesce) {
    e.ts = QDateTime::currentMSecsSinceEpoch();
    e.coalesce = coalesce;
    if (batchDepth_ > 0) {   // inside a batch: a run on the same element folds into its part
        if (!coalesce.isEmpty())
            for (UndoEntry& part : batch_.parts)
                if (part.coalesce == coalesce && part.id == e.id) { part.after = std::move(e.after); return; }
        batch_.parts.push_back(std::move(e));
        return;
    }
    // A live run (drag) folds into the current entry: same key, same element, nothing to redo —
    // the entry keeps its `before`, takes the new `after` (BlockModel::endTxn's rule).
    if (!coalesce.isEmpty() && undoCur_ >= 0 && !canRedo()) {
        UndoEntry& prev = undo_[size_t(undoCur_)];
        if (prev.coalesce == coalesce && prev.id == e.id) {
            prev.after = std::move(e.after);
            prev.label = e.label;
            ++undoRev_;
            emit undoStackChanged();
            return;
        }
    }
    e.parent = undoCur_;
    undo_.push_back(std::move(e));
    undoCur_ = static_cast<int>(undo_.size()) - 1;
    ++undoRev_;
    emit undoStackChanged();
}

bool CanvasDocument::canRedo() const {
    for (int i = static_cast<int>(undo_.size()) - 1; i >= 0; --i)
        if (undo_[size_t(i)].parent == undoCur_) return true;
    return false;
}

void CanvasDocument::applyEntry(const UndoEntry& e, bool before) {
    if (e.parts.empty()) { applySnap(e.id, before ? e.before : e.after); return; }
    if (before) for (size_t i = e.parts.size(); i-- > 0;) applySnap(e.parts[i].id, e.parts[i].before);
    else        for (const UndoEntry& p : e.parts) applySnap(p.id, p.after);
}

void CanvasDocument::undo() {
    if (undoCur_ < 0) return;
    if (batchDepth_ > 0) { batchDepth_ = 0; batch_ = UndoEntry{}; }   // an undo mid-gesture ends the gesture's batch unrecorded
    const UndoEntry e = undo_[size_t(undoCur_)];
    applyEntry(e, true);
    undoCur_ = e.parent;
    ++undoRev_;
    emit undoStackChanged();
    const UndoEntry& k = e.parts.empty() ? e : e.parts.front();
    noteChanged(k.before.present ? k.before.el.kind : k.after.el.kind, true, true, true);
}

void CanvasDocument::redo() {
    int child = -1;
    for (int i = static_cast<int>(undo_.size()) - 1; i >= 0; --i)
        if (undo_[size_t(i)].parent == undoCur_) { child = i; break; }
    if (child < 0) return;
    const UndoEntry e = undo_[size_t(child)];
    applyEntry(e, false);
    undoCur_ = child;
    ++undoRev_;
    emit undoStackChanged();
    const UndoEntry& k = e.parts.empty() ? e : e.parts.front();
    noteChanged(k.after.present ? k.after.el.kind : k.before.el.kind, true, true, true);
}

void CanvasDocument::clearUndo() {
    const bool had = !undo_.empty() || undoCur_ >= 0;
    undo_.clear();
    batchDepth_ = 0;
    batch_ = UndoEntry{};
    undoCur_ = -1;
    if (had) { ++undoRev_; emit undoStackChanged(); }
}

QVariantList CanvasDocument::undoHistory() const {
    std::vector<int> path;
    for (int n = undoCur_; n >= 0; n = undo_[size_t(n)].parent) path.push_back(n);
    std::reverse(path.begin(), path.end());
    for (int n = undoCur_;;) {
        int child = -1;
        for (int i = static_cast<int>(undo_.size()) - 1; i >= 0; --i)
            if (undo_[size_t(i)].parent == n) { child = i; break; }
        if (child < 0) break;
        path.push_back(child);
        n = child;
    }
    QVariantList out;
    constexpr size_t kMax = 200;
    size_t first = 0;
    if (path.size() > kMax) {
        first = path.size() - kMax;
        QVariantMap m;
        m.insert(QStringLiteral("idx"), -2);
        m.insert(QStringLiteral("label"), tr("… %1 earlier steps").arg(qint64(first)));
        m.insert(QStringLiteral("ts"), 0);
        m.insert(QStringLiteral("current"), false);
        m.insert(QStringLiteral("future"), false);
        out.push_back(m);
    } else {
        QVariantMap m;
        m.insert(QStringLiteral("idx"), -1);
        m.insert(QStringLiteral("label"), tr("Opened"));
        m.insert(QStringLiteral("ts"), 0);
        m.insert(QStringLiteral("current"), undoCur_ == -1);
        m.insert(QStringLiteral("future"), false);
        out.push_back(m);
    }
    bool past = undoCur_ >= 0;
    for (size_t i = first; i < path.size(); ++i) {
        const int idx = path[i];
        const UndoEntry& e = undo_[size_t(idx)];
        QVariantMap m;
        m.insert(QStringLiteral("idx"), idx);
        m.insert(QStringLiteral("label"), e.label);
        m.insert(QStringLiteral("ts"), e.ts);
        const bool cur = idx == undoCur_;
        m.insert(QStringLiteral("current"), cur);
        m.insert(QStringLiteral("future"), !past && !cur);
        if (cur) past = false;
        out.push_back(m);
    }
    return out;
}

void CanvasDocument::undoJumpTo(int target) {
    if (target == undoCur_ || target < -1 || target >= static_cast<int>(undo_.size())) return;
    int n = undoCur_;
    while (n >= 0 && n != target) n = undo_[size_t(n)].parent;
    if (n == target) {
        while (undoCur_ != target) undo();
        return;
    }
    int prev = -2;
    while (undoCur_ != target && undoCur_ != prev) { prev = undoCur_; redo(); }
}
