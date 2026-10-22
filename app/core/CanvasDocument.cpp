#include "CanvasDocument.h"
#include "BlockModel.h"
#include "CanvasLayout.h"
#include "DocumentMerger.h"
#include "Importer.h"
#include "MediaStore.h"
#include "FileLinks.h"
#include "PackageFormat.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QColor>
#include <QJsonObject>
#include <QVariantMap>
#include <algorithm>
#include <QPointer>
#include <QJSValue>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QImage>
#include <QTextDocumentFragment>
#include <QTextBlock>
#include <QTextDocument>
#include <QRegularExpression>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkAccessManager>

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
    livePage_.clear();
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
    livePage_.clear();
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
    if (!livePage_.isEmpty()) { livePage_.clear(); emit liveChanged(); }
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
    const bool wasLive = !livePage_.isEmpty();
    if (next == focusPage_ && !wasLive) return;
    livePage_.clear();                // a drill-in (or a return to the canvas) ends liveness
    focusPage_ = next;
    if (wasLive) emit liveChanged();
    emit focusChanged();
}
void CanvasDocument::setLivePage(const QString& id) {
    const QString next = (id.isEmpty() || pageIds().contains(id)) ? id : livePage_;
    if (next == livePage_) return;
    livePage_ = next;
    emit liveChanged();
}

QString CanvasDocument::focusedPageId() const {
    return focusPage_.isEmpty() ? firstPageId() : focusPage_;
}

BlockModel* CanvasDocument::focusedModel() {
    return pageModel(livePage_.isEmpty() ? focusedPageId() : livePage_);
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

double CanvasDocument::pageHeightCap(double elementWidth) {   // AUTO height stops at a SQUARE card (1:1, walk 7c; was √2 — a page length)
    const double measure = std::max(kMinPageMeasure, elementWidth - 2.0 * kPageInset);
    return std::round(measure);
}

static bool jsonFixedH(const QString& json) {
    return QJsonDocument::fromJson(json.toUtf8()).object().value(QStringLiteral("fixedH")).toBool(false);
}
bool CanvasDocument::pageHeightFixed(const QString& id) const {
    const auto e = element(id);
    return e && e->kind == QLatin1String("page") && jsonFixedH(e->json);
}
bool CanvasDocument::fitPageHeight(const QString& id) {
    const auto cur = element(id);
    if (!cur || cur->kind != QLatin1String("page") || !db_.isOpen()) return false;
    QJsonObject root = QJsonDocument::fromJson(cur->json.toUtf8()).object();
    root.remove(QStringLiteral("fixedH"));
    BlockModel* m = pageModel(id);
    const double h = clampPageHeight(cur->w, (m ? m->totalHeight() : 0.0) + 2.0 * kPageInset);
    UndoEntry u;
    u.id = id;
    u.before = snapshot(id, false);
    u.label = tr("Fit page height");
    db_.begin();
    db_.updateElementJson(id, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    db_.updateElementRect(id, cur->x, cur->y, cur->w, h);
    db_.commit();
    reloadElements();
    u.after = snapshot(id, false);
    pushEntry(std::move(u), QString());
    noteChanged(QStringLiteral("page"), true, true, false);
    return true;
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
    h = clampPageHeight(e.w, h);       // an empty page still reads as a page; a long one stops at the square cap
    if (e.kind != QLatin1String("page") || std::abs(e.h - h) < 1.0) return;
    if (jsonFixedH(e.json)) return;    // a hand-sized page keeps its height (fitPageHeight returns it to auto)
    e.h = h;
    db_.updateElementRect(id, e.x, e.y, e.w, h);
    ++geometryRevision_;
    emit geometryChanged();
}

void CanvasDocument::cachePageView(const QString& id, qreal scrollY) {
    const int i = indexById_.value(id, -1);
    if (i < 0 || !db_.isOpen()) return;
    Document::Element& e = elements_[size_t(i)];
    if (e.kind != QLatin1String("page")) return;
    QJsonObject root = QJsonDocument::fromJson(e.json.toUtf8()).object();
    QJsonObject view = root.value(QStringLiteral("view")).toObject();
    const qreal y = std::max<qreal>(0, std::round(scrollY));
    if (std::abs(view.value(QStringLiteral("scrollY")).toDouble() - y) < 0.5) return;
    view.insert(QStringLiteral("scrollY"), y);
    root.insert(QStringLiteral("view"), view);
    e.json = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    db_.updateElementJson(id, e.json);
}
qreal CanvasDocument::pageScrollY(const QString& id) const {
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("page")) return 0;
    return QJsonDocument::fromJson(e->json.toUtf8()).object().value(QStringLiteral("view")).toObject().value(QStringLiteral("scrollY")).toDouble();
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

// A descriptor `src` handed over from QML: a plain string, or a {vol, rel} PORTABLE REF — which arrives as a wrapped
// JS value, not a QVariantMap, so QJsonValue::fromVariant() alone made it undefined and every network-share
// reference on the canvas resolved to "" (the 2026-10-21 "not collected files don't render on the canvas" report;
// the page never crosses this boundary). Unwrap first.
static QJsonValue srcFromQml(const QVariant& src) {
    if (src.userType() == qMetaTypeId<QJSValue>()) return QJsonValue::fromVariant(src.value<QJSValue>().toVariant());
    return QJsonValue::fromVariant(src);
}
QString CanvasDocument::resolveMediaUrl(const QVariant& src) const {
    if (!mediaStore_ || src.isNull()) return QString();
    return mediaStore_->resolveUrlAsync(srcFromQml(src));
}
QString CanvasDocument::resolveMediaPath(const QVariant& src) const {
    const QString url = resolveMediaUrl(src);
    return url.isEmpty() ? QString() : QUrl(url).toLocalFile();
}
QString CanvasDocument::mediaPlaybackSource(const QVariant& src) const {
    if (!mediaStore_ || src.isNull()) return QString();
    return mediaStore_->playbackSourceFor(srcFromQml(src));
}
QString CanvasDocument::elementLocalPath(const QString& id) const {
    if (!mediaStore_) return QString();
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("media")) return QString();
    const QJsonValue src = QJsonDocument::fromJson(e->json.toUtf8()).object()
                               .value(QStringLiteral("media")).toObject().value(QStringLiteral("src"));
    if (src.isUndefined() || src.isNull()) return QString();
    const QString url = mediaStore_->resolveUrl(src);
    if (url.isEmpty()) return QString();
    const QUrl u(url);
    return u.isLocalFile() ? u.toLocalFile() : QString();
}
static QJsonObject mediaDescriptorOf(const Document::Element& e);   // (defined with the media helpers below)
QString CanvasDocument::elementSourcePath(const QString& id) const {
    if (!mediaStore_) return QString();
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("media")) return QString();
    const QString src = mediaStore_->previewSourcePath(mediaDescriptorOf(*e));
    return src.isEmpty() ? elementLocalPath(id) : src;
}
QString CanvasDocument::elementPreviewFormat(const QString& id) const {
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("media")) return QString();
    return mediaDescriptorOf(*e).value(QStringLiteral("preview")).toObject().value(QStringLiteral("format")).toString();
}
bool CanvasDocument::elementPreviewStale(const QString& id) const {
    const auto e = element(id);
    if (!e || !mediaStore_ || e->kind != QLatin1String("media")) return false;
    return mediaStore_->previewStale(mediaDescriptorOf(*e));
}
bool CanvasDocument::refreshElementPreview(const QString& id) {
    const auto e = element(id);
    if (!e || !mediaStore_ || e->kind != QLatin1String("media")) return false;
    const QString fresh = mediaStore_->refreshedPreviewJson(mediaDescriptorOf(*e));
    if (fresh.isEmpty()) return false;
    QJsonObject root = QJsonDocument::fromJson(e->json.toUtf8()).object();
    root.insert(QStringLiteral("media"), QJsonDocument::fromJson(fresh.toUtf8()).object());
    return setElementJson(id, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}
QString CanvasDocument::ufbLink(const QString& id) const { return mn::ufbLinkFor(elementSourcePath(id)); }
void CanvasDocument::openElement(const QString& id) const { mn::openWithDefaultApp(elementSourcePath(id)); }   // a preview opens its ORIGINAL
void CanvasDocument::openElementInUfb(const QString& id) const { mn::openInUfb(elementSourcePath(id)); }
void CanvasDocument::revealElement(const QString& id) const { mn::revealInFileManager(elementSourcePath(id)); }
static QJsonObject mediaDescriptorOf(const Document::Element& e) {
    return QJsonDocument::fromJson(e.json.toUtf8()).object().value(QStringLiteral("media")).toObject();
}
QStringList CanvasDocument::fullFrameMediaIds() const {
    QStringList out;
    for (const auto& e : elements_) {
        if (e.kind != QLatin1String("media")) continue;
        const QString k = mediaDescriptorOf(e).value(QStringLiteral("kind")).toString();
        if (k == QLatin1String("video") || k == QLatin1String("pdf")) out << e.id;
    }
    return out;
}
QString CanvasDocument::mediaElementKind(const QString& id) const {
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("media")) return QString();
    return mediaDescriptorOf(*e).value(QStringLiteral("kind")).toString(QStringLiteral("image"));
}
QString CanvasDocument::mediaElementName(const QString& id) const {
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("media")) return QString();
    const QJsonObject d = mediaDescriptorOf(*e);
    const QString n = d.value(QStringLiteral("name")).toString();
    if (!n.isEmpty()) return n;
    const QJsonValue s = d.value(QStringLiteral("src"));
    QString path = s.isString() ? s.toString() : QString();
    if (s.isObject()) { const QJsonArray rel = s.toObject().value(QStringLiteral("rel")).toArray(); if (!rel.isEmpty()) path = rel.last().toString(); }
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}
QString CanvasDocument::mediaAnchorPath(const QVariant& src) const {
    if (!mediaStore_ || src.isNull()) return QString();
    return mediaStore_->anchorPathFor(srcFromQml(src));
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
    if (kind == QLatin1String("group")) return CanvasDocument::tr("frame");
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

QString CanvasDocument::addElement(const QString& kind, const QRectF& r, const QString& json, const QString& label, const QString& parentId) {
    if (!db_.isOpen()) return QString();
    Document::Element e;
    e.id = makeUlid();
    e.kind = kind;
    e.x = r.x(); e.y = r.y(); e.w = r.width(); e.h = r.height();
    e.z = nextZ();
    e.parentId = parentId;
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
    const QString path = fileUrl.startsWith(QLatin1String("file:")) ? QUrl(fileUrl).toLocalFile() : fileUrl;
    if (!path.isEmpty() && QFileInfo(path).isDir())   // a FOLDER element (phase A6): name, count, a few entries, its path
        return addMediaElement(MediaStore::folderDescriptorJson(path), QRectF(wx, wy, kFolderCardW, kFolderCardH));
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
    if (MediaStore::isAudioPath(fileUrl)) {   // phase A9: an audio element (name, duration, path; inline play)
        const MediaStore::AudioRef ref = mediaStore_->importAudioFile(fileUrl);
        if (ref.ok()) return addMediaElement(MediaStore::audioDescriptorJson(ref), QRectF(wx, wy, kAudioCardW, kAudioCardH));
    }
    if (MediaStore::isPreviewablePath(fileUrl)) {   // the preview pass (2026-10-21): PSD / HEIC / RAW / HDR as a rendered image
        const MediaStore::PreviewRef ref = mediaStore_->importPreview(fileUrl);
        if (ref.ok()) {
            const double dw = std::min<double>(kDropMediaWidth, ref.img.w);
            return addMediaElement(MediaStore::previewDescriptorJson(ref), QRectF(wx, wy, dw, dw * ref.img.h / ref.img.w));
        }
    }
    {
        const MediaStore::ImageRef ref = mediaStore_->importFile(fileUrl, /*forceCopy*/false);
        if (ref.ok()) {
            const double dw = std::min<double>(kDropMediaWidth, ref.w);   // a small image stays its size
            return addMediaElement(MediaStore::imageDescriptorJson(ref), QRectF(wx, wy, dw, dw * ref.h / ref.w));
        }
    }
    if (path.endsWith(QLatin1String(".mnd"), Qt::CaseInsensitive) || mnpkg::isPackagePath(path)) {   // another minNotes file: its pages come IN (no link)
        const QStringList ids = copyPagesFrom(fileUrl, wx, wy);
        return ids.isEmpty() ? QString() : ids.first();
    }
    const QString fmt = Importer::formatForPath(fileUrl);
    if (fmt == QLatin1String("enex") || fmt == QLatin1String("notion")) { emit importMultiDocRequested(fileUrl, wx, wy); return QString(); }
    if (!fmt.isEmpty()) {
        emit importFileRequested(fileUrl, wx, wy);
        return QString();
    }
    if (path.isEmpty()) return QString();
    return addMediaElement(MediaStore::fileDescriptorJson(path), QRectF(wx, wy, kFileCardW, kFileCardH));
}
QStringList CanvasDocument::addElementsFromUrls(const QStringList& fileUrls, qreal wx, qreal wy) {
    QStringList ids;
    if (!db_.isOpen() || fileUrls.isEmpty()) return ids;
    if (fileUrls.size() == 1) { const QString id = addMediaElementFromUrl(fileUrls.first(), wx, wy); if (!id.isEmpty()) ids << id; return ids; }
    // Create first (at the drop point), then lay the batch out as a grid from each element's real size —
    // importables get a provisional page cell and are REQUESTED at that cell (Main.qml makes the page later,
    // its own history row). All inside one batch: the creations + the rect moves fold into one row.
    struct Slot { QString id; QString importUrl; double w, h; };
    std::vector<Slot> drops;
    beginBatch(tr("Drop %n files", nullptr, fileUrls.size()));
    for (const QString& url : fileUrls) {
        const QString path = url.startsWith(QLatin1String("file:")) ? QUrl(url).toLocalFile() : url;
        const bool pagesIn = !path.isEmpty() && !QFileInfo(path).isDir()
                             && (path.endsWith(QLatin1String(".mnd"), Qt::CaseInsensitive) || mnpkg::isPackagePath(path)
                                 || (!MediaStore::isVideoPath(path) && !MediaStore::isPdfPath(path) && !Importer::formatForPath(path).isEmpty()));
        if (pagesIn) { drops.push_back({QString(), url, kDefaultMeasure + 2 * kPageInset, kMinPageHeight}); continue; }   // a page-sized cell; made after the batch
        const QString id = addMediaElementFromUrl(url, wx, wy);
        if (id.isEmpty()) continue;
        const QRectF r = elementRect(id);
        drops.push_back({id, QString(), r.width(), r.height()});
        ids << id;
    }
    double cardW = 0;
    std::vector<double> heights;
    for (const Slot& s : drops) { cardW = std::max(cardW, s.w); heights.push_back(s.h); }
    const std::vector<QRectF> cells = mn::defaultCardLayout(heights, cardW, kDropGridGap);
    for (size_t i = 0; i < drops.size() && i < cells.size(); ++i) {
        const QPointF at(wx + cells[i].x(), wy + cells[i].y());
        if (!drops[i].id.isEmpty()) setElementRect(drops[i].id, QRectF(at, QSizeF(drops[i].w, drops[i].h)));
    }
    endBatch();
    for (size_t i = 0; i < drops.size() && i < cells.size(); ++i) {   // after the batch: the pages are their own rows
        if (drops[i].importUrl.isEmpty()) continue;
        const QString u = drops[i].importUrl, p = u.startsWith(QLatin1String("file:")) ? QUrl(u).toLocalFile() : u;
        const QString fmt = Importer::formatForPath(p);
        if (p.endsWith(QLatin1String(".mnd"), Qt::CaseInsensitive) || mnpkg::isPackagePath(p)) ids << copyPagesFrom(u, wx + cells[i].x(), wy + cells[i].y());
        else if (fmt == QLatin1String("enex") || fmt == QLatin1String("notion")) emit importMultiDocRequested(u, wx + cells[i].x(), wy + cells[i].y());
        else emit importFileRequested(u, wx + cells[i].x(), wy + cells[i].y());
    }
    return ids;
}
QStringList CanvasDocument::copyPagesFrom(const QString& fileUrl, qreal wx, qreal wy) {
    QStringList made;
    if (!db_.isOpen() || !mediaStore_) return made;
    const QString path = fileUrl.startsWith(QLatin1String("file:")) ? QUrl(fileUrl).toLocalFile() : fileUrl;
    CanvasDocument src;
    if (!QFileInfo::exists(path) || !src.open(path)) {   // (open() would CREATE a missing .mnd — the 1.0 behaviour)
        emit importNotice(tr("Couldn't open %1").arg(QFileInfo(path).fileName()));
        return made;
    }
    const QRectF b = src.canvasBounds();
    const QPointF off(wx - b.x(), wy - b.y());
    // Sidecar media of the source's non-page elements is re-homed into THIS store (content-addressed);
    // in-place references (absolute / {vol,rel}) copy as they are.
    QHash<QString, QString> rehomed;
    auto rehome = [&](const QJsonValue& v) -> QJsonValue {
        if (!v.isString() || !v.toString().startsWith(QLatin1String(".minnotes/"))) return v;
        const QString rel = v.toString();
        if (rehomed.contains(rel)) return rehomed.value(rel);
        QFile f(QUrl(src.mediaStore()->resolveUrl(rel)).toLocalFile());   // blocking: a packaged source extracts
        if (!f.open(QIODevice::ReadOnly)) return v;
        const QString out = mediaStore_->importBytes(f.readAll(), QFileInfo(rel).fileName());
        if (out.isEmpty()) return v;
        rehomed.insert(rel, out);
        return out;
    };
    int pages = 0;
    beginBatch(tr("Copy in %1").arg(src.documentName()));
    for (const Document::Element& e : src.elements()) {   // z order
        const QRectF r(e.x + off.x(), e.y + off.y(), e.w, e.h);
        if (e.kind == QLatin1String("page")) {
            const QString id = addPageWithRect(r);
            if (id.isEmpty()) continue;
            BlockModel* dm = pageModel(id);
            BlockModel* sm = src.pageModel(e.id);
            if (dm && sm && sm->rowCountQml() > 0) {
                int first = -1, last = -1; QString err;
                DocumentMerger::mergeDocuments(sm, dm, 0, &first, &last, &err);
                // The seeded blank paragraph goes once the page has real content — it sits OUTSIDE the
                // merged range (gap 0 inserts before it, so it is the last row).
                if (dm->rowCountQml() > 1 && first >= 0)
                    for (int r = dm->rowCountQml() - 1; r >= 0; --r)
                        if ((r < first || r > last) && dm->typeForRow(r) == BlockModel::Paragraph && dm->contentForRow(r).isEmpty()) { dm->removeBlock(r); break; }
            }
            if (e.json != QLatin1String("{}") && !e.json.isEmpty()) setElementJson(id, e.json);   // title override / view
            made << id; ++pages;
        } else {
            QString json = e.json;
            if (e.kind == QLatin1String("media")) {
                QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
                QJsonObject md = root.value(QStringLiteral("media")).toObject();
                md.insert(QStringLiteral("src"), rehome(md.value(QStringLiteral("src"))));
                if (md.contains(QStringLiteral("icon"))) md.insert(QStringLiteral("icon"), rehome(md.value(QStringLiteral("icon"))));
                root.insert(QStringLiteral("media"), md);
                json = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
            }
            const QString id = addElement(e.kind, r, json, tr("Copy in"));
            if (!id.isEmpty()) made << id;
        }
    }
    endBatch();
    const QString name = src.documentName();
    src.closeCanvas();
    emit importNotice(tr("Copied %n page(s) from %1", nullptr, pages).arg(name));
    if (!made.isEmpty()) emit elementsAdded(made);
    return made;
}
QStringList CanvasDocument::spreadFolder(const QString& id) {
    const QString dir = elementLocalPath(id);
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) return {};
    const QStringList names = QDir(dir).entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                                                  QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    QStringList urls;
    for (int i = 0; i < names.size() && i < kSpreadCap; ++i) urls << QUrl::fromLocalFile(QDir(dir).filePath(names.at(i))).toString();
    const QRectF r = elementRect(id);
    return addElementsFromUrls(urls, r.x(), r.y() + r.height() + kDropGridGap);
}
bool CanvasDocument::folderIsStream(const QString& id) const {
    if (mediaElementKind(id) != QLatin1String("folder")) return false;
    const QString dir = elementLocalPath(id);
    return !dir.isEmpty() && QFileInfo::exists(dir + QStringLiteral("/notes.json"));
}
void CanvasDocument::rescanFolder(const QString& id) {
    const QString dir = elementLocalPath(id);
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) return;
    patchMediaDescriptor(id, MediaStore::folderDescriptorJson(dir));
}
QNetworkAccessManager* CanvasDocument::net() {
    if (!net_) net_ = new QNetworkAccessManager(this);
    return net_;
}
static QNetworkRequest webRequest(const QUrl& u) {
    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("minNotes"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(8000);
    return req;
}
static QJsonObject mediaOfElementJson(const QString& json) {
    return QJsonDocument::fromJson(json.toUtf8()).object().value(QStringLiteral("media")).toObject();
}
QString CanvasDocument::addBookmarkElement(const QString& url, qreal wx, qreal wy) {
    if (!db_.isOpen()) return QString();
    const QUrl u(url.trimmed());
    if (!u.isValid() || (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https"))) return QString();
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("bookmark"));
    o.insert(QStringLiteral("src"), u.toString());
    o.insert(QStringLiteral("host"), u.host());
    o.insert(QStringLiteral("title"), u.host());
    o.insert(QStringLiteral("icon"), QString());
    o.insert(QStringLiteral("fetched"), false);
    const QString id = addMediaElement(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)), QRectF(wx, wy, kBookmarkCardW, kBookmarkCardH));
    if (!id.isEmpty()) fetchBookmarkMeta(id);
    return id;
}
QVariantMap CanvasDocument::bookmarkMetaFromHtml(const QString& html, const QString& baseUrl) {
    QVariantMap out;
    const QUrl base(baseUrl);
    QString title;
    const QRegularExpression titleRe(QStringLiteral("<title[^>]*>(.*?)</title>"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    if (const auto m = titleRe.match(html); m.hasMatch()) title = QTextDocumentFragment::fromHtml(m.captured(1)).toPlainText().simplified();
    QString icon;
    const QRegularExpression linkRe(QStringLiteral(R"(<link\b[^>]*>)"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression relRe(QStringLiteral(R"(\brel\s*=\s*["']([^"']*)["'])"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression hrefRe(QStringLiteral(R"(\bhref\s*=\s*["']([^"']*)["'])"), QRegularExpression::CaseInsensitiveOption);
    int best = 99;
    for (auto it = linkRe.globalMatch(html); it.hasNext();) {
        const QString tag = it.next().captured(0);
        const auto rm = relRe.match(tag), hm = hrefRe.match(tag);
        if (!rm.hasMatch() || !hm.hasMatch()) continue;
        const QString rel = rm.captured(1).toLower();
        int rank = rel == QLatin1String("icon") ? 0 : rel == QLatin1String("shortcut icon") ? 1 : rel.contains(QLatin1String("apple-touch-icon")) ? 2 : -1;
        if (rank < 0 || rank >= best) continue;
        best = rank; icon = hm.captured(1);
    }
    QUrl iconUrl = icon.isEmpty() ? QUrl(base.scheme() + QStringLiteral("://") + base.authority() + QStringLiteral("/favicon.ico")) : base.resolved(QUrl(icon));
    out.insert(QStringLiteral("title"), title);
    out.insert(QStringLiteral("iconUrl"), iconUrl.toString());
    return out;
}
void CanvasDocument::fetchBookmarkMeta(const QString& id) {
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("media")) return;
    const QJsonObject d = mediaOfElementJson(e->json);
    if (d.value(QStringLiteral("kind")).toString() != QLatin1String("bookmark")) return;
    const QString url = d.value(QStringLiteral("src")).toString();
    QPointer<CanvasDocument> self(this);
    QNetworkReply* reply = net()->get(webRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [self, reply, id, url]() {
        reply->deleteLater();
        if (!self || !self->db_.isOpen()) return;
        const auto cur = self->element(id);
        if (!cur || reply->error() != QNetworkReply::NoError) return;
        const QVariantMap meta = bookmarkMetaFromHtml(QString::fromUtf8(reply->read(256 * 1024)), url);
        QJsonObject d = mediaOfElementJson(cur->json);
        const QString title = meta.value(QStringLiteral("title")).toString();
        if (!title.isEmpty()) d.insert(QStringLiteral("title"), title);
        d.insert(QStringLiteral("fetched"), true);
        self->patchMediaDescriptor(id, QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact)));
        const QString iconUrl = meta.value(QStringLiteral("iconUrl")).toString();
        if (iconUrl.isEmpty()) return;
        QNetworkReply* ir = self->net()->get(webRequest(QUrl(iconUrl)));
        QObject::connect(ir, &QNetworkReply::finished, self.data(), [self, ir, id]() {
            ir->deleteLater();
            if (!self || !self->db_.isOpen() || !self->mediaStore_) return;
            const auto cur = self->element(id);
            if (!cur || ir->error() != QNetworkReply::NoError) return;
            QImage img;
            if (!img.loadFromData(ir->read(512 * 1024)) || img.isNull()) return;
            const MediaStore::ImageRef ref = self->mediaStore_->importImage(img);
            if (!ref.ok()) return;
            QJsonObject d = mediaOfElementJson(cur->json);
            d.insert(QStringLiteral("icon"), ref.src);
            self->patchMediaDescriptor(id, QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact)));
        });
    });
}
QString CanvasDocument::addRemoteImageElement(const QString& url, qreal wx, qreal wy) {
    if (!db_.isOpen()) return QString();
    const QString id = addMediaElement(MediaStore::remoteImageDescriptorJson(url.trimmed()), QRectF(wx, wy, kDropMediaWidth, 300));
    if (!id.isEmpty()) localizeRemoteMedia(id);
    return id;
}
void CanvasDocument::localizeRemoteMedia(const QString& id) {
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("media")) return;
    const QString src = mediaOfElementJson(e->json).value(QStringLiteral("src")).toString();
    if (!src.startsWith(QLatin1String("http"))) return;
    QPointer<CanvasDocument> self(this);
    QNetworkReply* reply = net()->get(webRequest(QUrl(src)));
    connect(reply, &QNetworkReply::finished, this, [self, reply, id]() {
        reply->deleteLater();
        if (!self || !self->db_.isOpen() || reply->error() != QNetworkReply::NoError) return;
        QImage img;
        if (!img.loadFromData(reply->readAll()) || img.isNull()) return;
        self->applyLocalizedImage(id, img);
    });
}
bool CanvasDocument::applyLocalizedImage(const QString& id, const QImage& img) {
    if (!db_.isOpen() || !mediaStore_ || img.isNull()) return false;
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("media")) return false;
    const MediaStore::ImageRef ref = mediaStore_->importImage(img);
    if (!ref.ok()) return false;
    QJsonObject d = mediaOfElementJson(e->json);
    d.insert(QStringLiteral("src"), ref.src);
    d.insert(QStringLiteral("w"), ref.w);
    d.insert(QStringLiteral("h"), ref.h);
    patchMediaDescriptor(id, QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact)));
    // The rect keeps its width and takes the real aspect — a cache fact like the descriptor, no undo row.
    const double w = e->w > 0 ? e->w : kDropMediaWidth, h = std::round(w * ref.h / double(ref.w));
    db_.begin(); db_.updateElementRect(id, e->x, e->y, w, h); db_.commit();
    reloadElements();
    noteChanged(QStringLiteral("media"), false, true, false);
    return true;
}
QString CanvasDocument::addClipboardImageElement(qreal wx, qreal wy) {
    if (!db_.isOpen() || !mediaStore_) return QString();
    const MediaStore::ImageRef ref = mediaStore_->importClipboardImage();
    if (!ref.ok()) return QString();
    const double dw = std::min<double>(kDropMediaWidth, ref.w);
    return addMediaElement(MediaStore::imageDescriptorJson(ref), QRectF(wx, wy, dw, dw * ref.h / ref.w));
}
QString CanvasDocument::bareRemoteImageSrc(const QString& html) {
    if (html.isEmpty()) return QString();
    QTextDocument doc;
    doc.setHtml(html);
    QString remote; int images = 0;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid()) continue;
            const QTextCharFormat cf = frag.charFormat();
            if (cf.isImageFormat()) {
                ++images;
                const QString src = cf.toImageFormat().name();
                if (src.startsWith(QLatin1String("http://")) || src.startsWith(QLatin1String("https://"))) remote = src;
            } else if (!frag.text().trimmed().isEmpty() && frag.text() != QString(QChar::ObjectReplacementCharacter)) {
                return QString();
            }
        }
    }
    return images == 1 ? remote : QString();
}
void CanvasDocument::patchMediaDescriptor(const QString& id, const QString& descriptorJson) {
    if (!db_.isOpen()) return;
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("media")) return;
    QJsonObject root = QJsonDocument::fromJson(e->json.toUtf8()).object();
    root.insert(QStringLiteral("media"), QJsonDocument::fromJson(descriptorJson.toUtf8()).object());
    db_.begin();
    db_.updateElementJson(id, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    db_.commit();
    reloadElements();
    markDirty();
    noteChanged(QStringLiteral("media"), true, false, false);
}

QString CanvasDocument::addConnector(const QString& connectorJson) {
    return addElement(QStringLiteral("connector"), QRectF(), connectorJson, tr("Connect"));
}

bool CanvasDocument::setElementRect(const QString& id, const QRectF& rectIn, const QString& coalesce) {
    const auto cur = element(id);
    if (!cur) return false;
    QRectF rect = rectIn;
    bool pinHeight = false;
    if (cur->kind == QLatin1String("page")) {   // a page's measure stays readable: [400, 4000] + the insets
        rect.setWidth(std::clamp(rect.width(), kMinPageMeasure + 2.0 * kPageInset, kMaxPageMeasure + 2.0 * kPageInset));
        const bool fixed = jsonFixedH(cur->json);
        if (std::abs(rect.height() - cur->h) > 0.5 || fixed) {   // a NEW height (a top/bottom handle) pins the page to a fixed height (walk 7c)
            rect.setHeight(std::clamp(rect.height(), kMinFixedPageHeight, kMaxFixedPageHeight));
            pinHeight = !fixed;
        } else rect.setHeight(clampPageHeight(rect.width(), rect.height()));   // auto: the content's height within the square cap
    }
    if (QRectF(cur->x, cur->y, cur->w, cur->h) == rect && !pinHeight) return true;
    UndoEntry u;
    u.id = id;
    u.before = snapshot(id, false);
    const bool resize = std::abs(rect.width() - cur->w) > 0.01 || std::abs(rect.height() - cur->h) > 0.01;
    u.label = (resize ? tr("Resize %1") : tr("Move %1")).arg(kindLabel(cur->kind));
    if (pinHeight) {
        QJsonObject root = QJsonDocument::fromJson(cur->json.toUtf8()).object();
        root.insert(QStringLiteral("fixedH"), true);
        db_.updateElementJson(id, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    }
    db_.updateElementRect(id, rect.x(), rect.y(), rect.width(), rect.height());
    reloadElements();
    u.after = snapshot(id, false);
    pushEntry(std::move(u), coalesce);
    noteChanged(cur->kind, false, true, false);
    if (!cur->parentId.isEmpty()) hugFrame(cur->parentId, coalesce.isEmpty() ? QString() : QStringLiteral("hug:") + cur->parentId);
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

QVariantList CanvasDocument::layerRows() const {
    QVariantList rows;
    const int n = static_cast<int>(elements_.size());
    for (int i = n - 1; i >= 0;) {
        const Document::Element& e = elements_[size_t(i)];
        QVariantMap r;
        if (e.kind == QLatin1String("ink")) {
            QStringList ids;
            int j = i;
            while (j >= 0 && elements_[size_t(j)].kind == QLatin1String("ink") && elements_[size_t(j)].parentId == e.parentId) { ids.prepend(elements_[size_t(j)].id); --j; }
            r.insert(QStringLiteral("kind"), QStringLiteral("ink"));
            r.insert(QStringLiteral("name"), tr("Ink"));
            r.insert(QStringLiteral("ids"), ids);
            r.insert(QStringLiteral("count"), ids.size());
            r.insert(QStringLiteral("id"), ids.last());
            i = j;
        } else {
            QString kind = e.kind, name;
            if (e.kind == QLatin1String("page")) { name = pageTitle(e.id); }
            else if (e.kind == QLatin1String("media")) {
                kind = mediaElementKind(e.id);
                if (kind.isEmpty()) kind = QStringLiteral("file");
                name = mediaElementName(e.id);
                if (name.isEmpty()) name = kind == QLatin1String("bookmark") ? tr("Link") : kind == QLatin1String("folder") ? tr("Folder") : tr("File");
            } else if (e.kind == QLatin1String("shape")) {
                const QJsonObject spec = QJsonDocument::fromJson(e.json.toUtf8()).object();
                const QString shape = spec.value(QStringLiteral("shape")).toString();
                if (shape == QLatin1String("text") || shape.isEmpty()) {
                    kind = QStringLiteral("text");
                    name = spec.value(QStringLiteral("text")).toString().section(QLatin1Char('\n'), 0, 0).simplified();
                    if (name.isEmpty()) name = tr("Text");
                } else {
                    kind = QStringLiteral("shape");
                    name = shape == QLatin1String("rect") ? tr("Rectangle") : shape == QLatin1String("oval") ? tr("Oval")
                         : shape == QLatin1String("line") ? tr("Line") : shape == QLatin1String("arrow") ? tr("Arrow") : shape;
                }
            } else if (e.kind == QLatin1String("connector")) { name = tr("Connector"); }
            else if (e.kind == QLatin1String("group")) { kind = QStringLiteral("frame"); name = frameTitle(e.id); if (name.isEmpty()) name = tr("Frame"); }
            else { name = e.kind; }
            r.insert(QStringLiteral("kind"), kind);
            r.insert(QStringLiteral("name"), name);
            r.insert(QStringLiteral("ids"), QStringList{e.id});
            r.insert(QStringLiteral("count"), 1);
            r.insert(QStringLiteral("id"), e.id);
            --i;
        }
        r.insert(QStringLiteral("parent"), e.parentId);
        r.insert(QStringLiteral("depth"), e.parentId.isEmpty() ? 0 : 1);
        rows.push_back(r);
    }
    // The tree (frames, 2026-10-22): a frame's children sit ABOVE it in z, so front-most-first lists them before their
    // frame — move each frame row up to just before its first child, where a tree reads naturally.
    for (int fi = 0; fi < rows.size(); ++fi) {
        const QVariantMap fr = rows[fi].toMap();
        if (fr.value(QStringLiteral("kind")).toString() != QLatin1String("frame")) continue;
        const QString fid = fr.value(QStringLiteral("id")).toString();
        int first = -1;
        for (int ci = 0; ci < fi; ++ci) if (rows[ci].toMap().value(QStringLiteral("parent")).toString() == fid) { first = ci; break; }
        if (first < 0) continue;
        const QVariant row = rows[fi];
        rows.removeAt(fi);
        rows.insert(first, row);
    }
    return rows;
}

bool CanvasDocument::applyOrder(const std::vector<QString>& order, const QString& label) {
    std::vector<UndoEntry> parts;
    for (int z = 0; z < static_cast<int>(order.size()); ++z) {
        const auto cur = element(order[size_t(z)]);
        if (!cur || cur->z == z) continue;
        UndoEntry u;
        u.id = cur->id;
        u.before = snapshot(cur->id, false);
        u.label = tr("Reorder %1").arg(kindLabel(cur->kind));
        parts.push_back(std::move(u));
        db_.updateElementZ(cur->id, z);
    }
    if (parts.empty()) return true;   // already in that order
    reloadElements();
    beginBatch(label);
    for (UndoEntry& u : parts) { u.after = snapshot(u.id, false); pushEntry(std::move(u), QString()); }
    endBatch();
    noteChanged(QString(), true, false, false);
    return true;
}

bool CanvasDocument::placeAbove(const QStringList& ids, const QString& anchorId) {
    if (!db_.isOpen() || ids.isEmpty()) return false;
    const QSet<QString> moving(ids.begin(), ids.end());
    if (moving.contains(anchorId)) return false;
    std::vector<QString> block, rest;
    for (const Document::Element& e : elements_) (moving.contains(e.id) ? block : rest).push_back(e.id);
    if (block.empty()) return false;
    std::vector<QString> order;
    order.reserve(elements_.size());
    if (anchorId.isEmpty()) { order = block; order.insert(order.end(), rest.begin(), rest.end()); }
    else {
        bool placed = false;
        for (const QString& id : rest) {
            order.push_back(id);
            if (id == anchorId) { order.insert(order.end(), block.begin(), block.end()); placed = true; }
        }
        if (!placed) return false;
    }
    return applyOrder(order, tr("Reorder"));
}

bool CanvasDocument::stepLayers(const QStringList& ids, int dir) {
    if (!db_.isOpen() || ids.isEmpty() || dir == 0) return false;
    const QSet<QString> moving(ids.begin(), ids.end());
    const int n = static_cast<int>(elements_.size());
    int lo = n, hi = -1;
    bool allInk = true;
    for (int i = 0; i < n; ++i) {
        const Document::Element& e = elements_[size_t(i)];
        if (!moving.contains(e.id)) continue;
        lo = std::min(lo, i); hi = std::max(hi, i);
        if (e.kind != QLatin1String("ink")) allInk = false;
    }
    if (hi < 0) return false;
    const auto isInk = [&](int i) { return elements_[size_t(i)].kind == QLatin1String("ink"); };
    if (dir > 0) {
        int i = hi + 1;
        while (i < n && moving.contains(elements_[size_t(i)].id)) ++i;
        if (i >= n) return false;   // already on top
        if (!allInk && isInk(i)) while (i + 1 < n && isInk(i + 1) && !moving.contains(elements_[size_t(i + 1)].id)) ++i;   // over the whole run
        return placeAbove(ids, elements_[size_t(i)].id);
    }
    int i = lo - 1;
    while (i >= 0 && moving.contains(elements_[size_t(i)].id)) --i;
    if (i < 0) return false;        // already at the bottom
    if (!allInk && isInk(i)) while (i - 1 >= 0 && isInk(i - 1) && !moving.contains(elements_[size_t(i - 1)].id)) --i;
    --i;                            // the anchor is what sits BELOW the neighbour (run)
    while (i >= 0 && moving.contains(elements_[size_t(i)].id)) --i;
    return placeAbove(ids, i >= 0 ? elements_[size_t(i)].id : QString());
}

QString CanvasDocument::duplicateElement(const QString& id) {
    const auto cur = element(id);
    if (!cur || cur->kind == QLatin1String("page")) return QString();
    if (cur->kind == QLatin1String("group")) {   // the frame and its children, as one row; the copies land above in the same order
        beginBatch(tr("Duplicate frame"));
        const QString nf = addElement(cur->kind, QRectF(cur->x + 24, cur->y + 24, cur->w, cur->h), cur->json, tr("Duplicate frame"));
        for (const QString& cid : childrenOf(id)) {
            const auto c = element(cid);
            if (!c || c->kind == QLatin1String("page")) continue;
            addElement(c->kind, QRectF(c->x + 24, c->y + 24, c->w, c->h), c->json, tr("Duplicate %1").arg(kindLabel(c->kind)), nf);
        }
        endBatch();
        return nf;
    }
    return addElement(cur->kind, QRectF(cur->x + 24, cur->y + 24, cur->w, cur->h), cur->json, tr("Duplicate %1").arg(kindLabel(cur->kind)));
}

bool CanvasDocument::removeElement(const QString& id) {
    const auto cur = element(id);
    if (!cur) return false;
    if (cur->kind == QLatin1String("page")) return removePage(id);
    if (cur->kind == QLatin1String("group")) {   // a frame takes its children (ungroup is the way to keep them)
        const QStringList kids = childrenOf(id);
        if (!kids.isEmpty()) {
            beginBatch(tr("Remove frame"));
            for (const QString& cid : kids) removeElement(cid);
            const bool ok = removeElement(id);   // (now childless)
            endBatch();
            return ok;
        }
    }
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

// ---- FRAMES (2026-10-22): groups and boards ----
static QJsonObject frameSpec(const Document::Element& e) { return QJsonDocument::fromJson(e.json.toUtf8()).object(); }
static QString frameJson(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

QString CanvasDocument::addFrame(const QRectF& rect, bool fixed) {
    QJsonObject o;
    o.insert(QStringLiteral("title"), tr("Frame"));
    o.insert(QStringLiteral("sizing"), fixed ? QStringLiteral("fixed") : QStringLiteral("elastic"));
    o.insert(QStringLiteral("layout"), QStringLiteral("free"));
    QRectF r = rect;
    r.setWidth(std::max(r.width(), kFrameMinW)); r.setHeight(std::max(r.height(), kFrameMinH));
    return addElement(QStringLiteral("group"), r, frameJson(o), fixed ? tr("Add board") : tr("Add frame"));
}

QStringList CanvasDocument::childrenOf(const QString& frameId) const {
    QStringList out;
    if (frameId.isEmpty()) return out;
    for (const Document::Element& e : elements_) if (e.parentId == frameId) out << e.id;
    return out;
}
QString CanvasDocument::parentOf(const QString& id) const { const auto e = element(id); return e ? e->parentId : QString(); }
QStringList CanvasDocument::withChildren(const QStringList& ids) const {
    const QSet<QString> set(ids.begin(), ids.end());
    QStringList out;
    for (const Document::Element& e : elements_)
        if (set.contains(e.id) || (!e.parentId.isEmpty() && set.contains(e.parentId))) out << e.id;
    return out;
}
bool CanvasDocument::frameElastic(const QString& id) const {
    const auto e = element(id);
    return e && e->kind == QLatin1String("group") && frameSpec(*e).value(QStringLiteral("sizing")).toString() != QLatin1String("fixed");
}
QString CanvasDocument::frameTitle(const QString& id) const {
    const auto e = element(id);
    return e && e->kind == QLatin1String("group") ? frameSpec(*e).value(QStringLiteral("title")).toString() : QString();
}
bool CanvasDocument::setFrameTitle(const QString& id, const QString& title) { return setFrameField(id, QStringLiteral("title"), title); }
bool CanvasDocument::setFrameField(const QString& id, const QString& key, const QVariant& value) {
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("group")) return false;
    QJsonObject o = frameSpec(*e);
    o.insert(key, QJsonValue::fromVariant(value));
    if (!setElementJson(id, frameJson(o), QString())) return false;
    if (key == QLatin1String("sizing")) hugFrame(id, QString());   // loosening to elastic snaps to the children at once
    return true;
}

bool CanvasDocument::setParent(const QString& id, const QString& parentId) {
    const auto cur = element(id);
    if (!cur || cur->parentId == parentId) return !!cur;
    UndoEntry u;
    u.id = id;
    u.before = snapshot(id, false);
    u.label = parentId.isEmpty() ? tr("Remove from frame") : tr("Add to frame");
    db_.begin(); db_.updateElementParent(id, parentId); db_.commit();
    reloadElements();
    u.after = snapshot(id, false);
    pushEntry(std::move(u), QString());
    noteChanged(cur->kind, true, false, false);
    return true;
}

bool CanvasDocument::hugFrame(const QString& frameId, const QString& coalesce) {
    const auto f = element(frameId);
    if (!f || f->kind != QLatin1String("group") || !frameElastic(frameId)) return false;
    QRectF u;
    for (const Document::Element& e : elements_) {
        if (e.parentId != frameId) continue;
        const QRectF r = cullRect(e);
        u = u.isNull() ? r : u.united(r);
    }
    if (u.isNull()) return true;   // an empty elastic frame keeps the rect it was given
    const QRectF want(u.x() - kFramePad, u.y() - kFramePad - kFrameTitleH, u.width() + 2 * kFramePad, u.height() + 2 * kFramePad + kFrameTitleH);
    if (QRectF(f->x, f->y, f->w, f->h) == want) return true;
    UndoEntry e;
    e.id = frameId;
    e.before = snapshot(frameId, false);
    e.label = tr("Move frame");
    db_.updateElementRect(frameId, want.x(), want.y(), want.width(), want.height());
    reloadElements();
    e.after = snapshot(frameId, false);
    pushEntry(std::move(e), coalesce);
    noteChanged(QStringLiteral("group"), false, true, false);
    return true;
}

bool CanvasDocument::packFrame(const QString& frameId) {
    const auto f = element(frameId);
    if (!f) return false;
    std::vector<QString> rest, kids;
    int at = -1;   // where the block goes in `rest`: the first position any member held
    for (const Document::Element& e : elements_) {
        if (e.id == frameId || e.parentId == frameId) {
            if (at < 0) at = static_cast<int>(rest.size());
            if (e.id != frameId) kids.push_back(e.id);
        } else rest.push_back(e.id);
    }
    if (at < 0) return false;
    std::vector<QString> order(rest.begin(), rest.begin() + at);
    order.push_back(frameId);
    order.insert(order.end(), kids.begin(), kids.end());
    order.insert(order.end(), rest.begin() + at, rest.end());
    return applyOrder(order, tr("Reorder"));
}

QString CanvasDocument::groupElements(const QStringList& ids) {
    QStringList members;
    QRectF u;
    for (const QString& id : ids) {
        const auto e = element(id);
        if (!e || e->kind == QLatin1String("group") || members.contains(id)) continue;   // no nesting; a frame among them is skipped
        members << id;
        const QRectF r = cullRect(*e);
        u = u.isNull() ? r : u.united(r);
    }
    if (members.isEmpty()) return QString();
    beginBatch(tr("Group"));
    QJsonObject o;
    o.insert(QStringLiteral("title"), tr("Group"));
    o.insert(QStringLiteral("sizing"), QStringLiteral("elastic"));
    o.insert(QStringLiteral("layout"), QStringLiteral("free"));
    const QRectF want(u.x() - kFramePad, u.y() - kFramePad - kFrameTitleH, u.width() + 2 * kFramePad, u.height() + 2 * kFramePad + kFrameTitleH);
    const QString fid = addElement(QStringLiteral("group"), want, frameJson(o), tr("Group"));
    for (const QString& id : members) setParent(id, fid);   // (a member leaves any frame it was in)
    packFrame(fid);
    endBatch();
    return fid;
}

bool CanvasDocument::ungroup(const QString& frameId) {
    const auto f = element(frameId);
    if (!f || f->kind != QLatin1String("group")) return false;
    beginBatch(tr("Ungroup"));
    for (const QString& cid : childrenOf(frameId)) setParent(cid, QString());
    const bool ok = removeElement(frameId);
    endBatch();
    return ok;
}

bool CanvasDocument::addToFrame(const QStringList& ids, const QString& frameId) {
    const auto f = element(frameId);
    if (!f || f->kind != QLatin1String("group")) return false;
    beginBatch(tr("Add to frame"));
    bool any = false;
    for (const QString& id : ids) {
        const auto e = element(id);
        if (!e || id == frameId || e->kind == QLatin1String("group")) continue;
        if (setParent(id, frameId)) any = true;
    }
    if (any) { packFrame(frameId); hugFrame(frameId, QString()); }
    endBatch();
    return any;
}

bool CanvasDocument::removeFromFrame(const QStringList& ids) {
    beginBatch(tr("Remove from frame"));
    QStringList frames;
    for (const QString& id : ids) {
        const QString p = parentOf(id);
        if (p.isEmpty()) continue;
        if (!frames.contains(p)) frames << p;
        setParent(id, QString());
    }
    for (const QString& fid : frames) hugFrame(fid, QString());
    endBatch();
    return !frames.isEmpty();
}

qreal CanvasDocument::inkWidth(const QString& id) const {
    const auto e = element(id);
    if (!e || e->kind != QLatin1String("ink")) return 0;
    qreal w = 0;
    for (const QJsonValue& v : QJsonDocument::fromJson(e->json.toUtf8()).object().value(QStringLiteral("shapes")).toArray())
        w = std::max(w, v.toObject().value(QStringLiteral("stroke_width")).toDouble());
    return w;
}

bool CanvasDocument::setInkWidth(const QString& id, qreal width, const QString& coalesce) {
    const auto cur = element(id);
    if (!cur || cur->kind != QLatin1String("ink")) return false;
    width = std::clamp(width, 1.0, 64.0);
    QJsonObject root = QJsonDocument::fromJson(cur->json.toUtf8()).object();
    QJsonArray shapes = root.value(QStringLiteral("shapes")).toArray();
    if (shapes.isEmpty()) return false;
    qreal oldW = 0;
    for (const QJsonValue& v : shapes) oldW = std::max(oldW, v.toObject().value(QStringLiteral("stroke_width")).toDouble());
    const qreal delta = (width - oldW) / 2.0;   // the rect is the stroke's bbox + half the width each side (CanvasInkCapture)
    if (std::abs(delta) < 0.001) return true;
    for (int i = 0; i < shapes.size(); ++i) {
        QJsonObject sh = shapes[i].toObject();
        sh.insert(QStringLiteral("stroke_width"), width);
        const bool oval = sh.value(QStringLiteral("type")).toString() == QLatin1String("oval");
        QJsonArray pts = sh.value(QStringLiteral("points")).toArray();
        for (int k = 0; k < pts.size(); ++k) {
            if (oval && k == 1) continue;              // the oval's radii are a size, not a position
            QJsonArray p = pts[k].toArray();
            if (p.size() < 2) continue;
            pts[k] = QJsonArray{ p[0].toDouble() + delta, p[1].toDouble() + delta };
        }
        sh.insert(QStringLiteral("points"), pts);
        shapes[i] = sh;
    }
    root.insert(QStringLiteral("shapes"), shapes);
    const QRectF r(cur->x - delta, cur->y - delta, cur->w + 2 * delta, cur->h + 2 * delta);
    beginBatch(tr("Pen width"));
    setElementJson(id, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)), coalesce.isEmpty() ? QString() : coalesce + QStringLiteral(":j"));
    setElementRect(id, r, coalesce.isEmpty() ? QString() : coalesce + QStringLiteral(":r"));
    endBatch();
    return true;
}

bool CanvasDocument::setInkColor(const QString& id, const QColor& color, const QString& coalesce) {
    const auto cur = element(id);
    if (!cur || cur->kind != QLatin1String("ink") || !color.isValid()) return false;
    QJsonObject root = QJsonDocument::fromJson(cur->json.toUtf8()).object();
    QJsonArray shapes = root.value(QStringLiteral("shapes")).toArray();
    if (shapes.isEmpty()) return false;
    const QJsonArray arr{ color.redF(), color.greenF(), color.blueF(), color.alphaF() };
    for (int i = 0; i < shapes.size(); ++i) { QJsonObject sh = shapes[i].toObject(); sh.insert(QStringLiteral("color"), arr); shapes[i] = sh; }
    root.insert(QStringLiteral("shapes"), shapes);
    return setElementJson(id, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)), coalesce);
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
    if (!livePage_.isEmpty() && !indexById_.contains(livePage_)) { livePage_.clear(); emit liveChanged(); }
    if (!focusPage_.isEmpty() && !indexById_.contains(focusPage_)) {
        focusPage_ = firstPageId();
        emit focusChanged();
    }
}

void CanvasDocument::beginBatch(const QString& label) {
    if (batchDepth_++ == 0) { batch_ = UndoEntry{}; batch_.label = label; }
}

void CanvasDocument::discardBatch() {
    if (batchDepth_ <= 0) return;
    if (--batchDepth_ > 0) return;
    batch_ = UndoEntry{};
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
