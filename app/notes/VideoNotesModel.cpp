#include "VideoNotesModel.h"

#include "annotation_io.h"
#include "../core/MediaStore.h"
#include "../player/timecode_formatter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <QThreadPool>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {

// QCView wipes both PNGs on a user-click delete (WindowManager::
// deleteAnnotationNote); the annotated sibling shares the clean stem.
QString annotatedSibling(const QString &cleanAbs)
{
    if (!cleanAbs.endsWith(QStringLiteral(".png"))) return {};
    QString s = cleanAbs;
    s.chop(4);
    return s + QStringLiteral("_annotated.png");
}

qint64 fileMtimeMs(const QString &path)
{
    const QFileInfo fi(path);
    return fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;
}

// Full-field equality — AnnotationNote::operator== is timecode-only (it's
// the sort key), which would make a reload that only changed text/strokes
// look like a no-op echo and get dropped.
bool sameNotes(const std::vector<qcv::AnnotationNote> &a,
               const std::vector<qcv::AnnotationNote> &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const qcv::AnnotationNote &x = a[i];
        const qcv::AnnotationNote &y = b[i];
        if (x.timecode != y.timecode || x.frame != y.frame
            || x.text != y.text || x.addressed != y.addressed
            || x.annotation_data != y.annotation_data
            || x.image_path != y.image_path) return false;
    }
    return true;
}

} // namespace

VideoNotesModel::VideoNotesModel(QObject *parent)
    : QObject(parent)
{
    reloadDebounce_.setSingleShot(true);
    reloadDebounce_.setInterval(200);
    connect(&reloadDebounce_, &QTimer::timeout, this, [this] {
        // Our own QSaveFile rename also fires the watcher — skip those by
        // mtime; anything else is QCView (or the user) editing the sidecar.
        const qint64 mt = fileMtimeMs(qcv::annotation_io::getNotesJsonPath(mediaPath_));
        if (selfWriteMtimes_.contains(mt)) return;
        reloadFromDisk();
    });
    auto poke = [this](const QString &) { rewatch(); reloadDebounce_.start(); };
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, poke);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, poke);
}

void VideoNotesModel::setMediaPath(const QString &path)
{
    if (path == mediaPath_) return;
    mediaPath_ = path;
    notes_.clear();
    tombstones_.clear();
    selfWriteMtimes_.clear();
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
    emit mediaPathChanged();
    bump();
    if (!mediaPath_.isEmpty()) {
        qInfo("VideoNotesModel: media -> %s", qPrintable(mediaPath_));
        rewatch();
        reloadFromDisk();
    }
}

void VideoNotesModel::setFps(double fps)
{
    if (fps == fps_) return;
    fps_ = fps;
    emit fpsChanged();
}

QVariantList VideoNotesModel::noteList() const
{
    const QString imagesDir = qcv::annotation_io::getImagesFolder(mediaPath_);
    QVariantList out;
    out.reserve(static_cast<qsizetype>(notes_.size()));
    for (const qcv::AnnotationNote &n : notes_) {
        const QString cleanAbs = imagesDir + QLatin1Char('/')
            + qcv::annotation_io::generateImageFilename(n.timecode);
        // QCView's filmstrip prefers the annotated composite when strokes
        // exist — match it, so their drawings show up in our cards.
        const QString annAbs = annotatedSibling(cleanAbs);
        const bool hasAnn = !n.annotation_data.isEmpty() && QFileInfo::exists(annAbs);
        QVariantMap m;
        m.insert(QStringLiteral("timecode"),   n.timecode);
        m.insert(QStringLiteral("frame"),      n.frame);
        m.insert(QStringLiteral("text"),       n.text);
        m.insert(QStringLiteral("addressed"),  n.addressed);
        m.insert(QStringLiteral("image"),      hasAnn ? annAbs : cleanAbs);
        m.insert(QStringLiteral("hasStrokes"), !n.annotation_data.isEmpty());
        out.append(m);
    }
    return out;
}

QString VideoNotesModel::mintTimecode(int frame) const
{
    // QCView's exact rule (WindowManager::addNoteAtCurrentFrame): a fresh
    // NON-drop formatter from the raw fps, frame-0 origin — the embedded
    // start timecode is deliberately ignored for note keys.
    const ufbplayer::TimecodeFormatter tc(
        static_cast<int>(std::lround(fps_ * 1000.0)), 1000, /*dropFrame=*/false);
    return tc.isValid() ? tc.format(frame) : QStringLiteral("00:00:00:00");
}

const qcv::AnnotationNote *VideoNotesModel::noteAt(const QString &timecode) const
{
    auto it = std::find_if(notes_.cbegin(), notes_.cend(),
        [&](const qcv::AnnotationNote &n) { return n.timecode == timecode; });
    return it == notes_.cend() ? nullptr : &(*it);
}

qcv::AnnotationNote *VideoNotesModel::noteAt(const QString &timecode)
{
    auto it = std::find_if(notes_.begin(), notes_.end(),
        [&](const qcv::AnnotationNote &n) { return n.timecode == timecode; });
    return it == notes_.end() ? nullptr : &(*it);
}

QString VideoNotesModel::addNoteAtFrame(int frame)
{
    if (mediaPath_.isEmpty()) return {};
    const QString timecode = mintTimecode(frame);
    if (!noteAt(timecode)) {
        const double ts = fps_ > 0.0 ? frame / fps_ : 0.0;
        const QString rel = QStringLiteral("images/")
            + qcv::annotation_io::generateImageFilename(timecode);
        notes_.emplace_back(timecode, ts, frame, rel, QString());
        std::sort(notes_.begin(), notes_.end());
        tombstones_.remove(timecode);   // re-adding resurrects a deleted key
        bump();
        scheduleSave();
    }
    captureCleanThumb(timecode, frame);   // idempotent either way
    return timecode;
}

void VideoNotesModel::setText(const QString &timecode, const QString &text)
{
    qcv::AnnotationNote *n = noteAt(timecode);
    if (!n || n->text == text) return;
    n->text = text;
    bump();
    scheduleSave();
}

void VideoNotesModel::setAddressed(const QString &timecode, bool addressed)
{
    qcv::AnnotationNote *n = noteAt(timecode);
    if (!n || n->addressed == addressed) return;
    n->addressed = addressed;
    bump();
    scheduleSave();
}

void VideoNotesModel::removeNote(const QString &timecode)
{
    auto it = std::find_if(notes_.begin(), notes_.end(),
        [&](const qcv::AnnotationNote &n) { return n.timecode == timecode; });
    if (it == notes_.end()) return;
    notes_.erase(it);
    tombstones_.insert(timecode);
    // Explicit user delete → wipe the PNGs too (QCView's user-click path).
    const QString cleanAbs = qcv::annotation_io::getImagesFolder(mediaPath_)
        + QLatin1Char('/') + qcv::annotation_io::generateImageFilename(timecode);
    QFile::remove(cleanAbs);
    const QString annAbs = annotatedSibling(cleanAbs);
    if (!annAbs.isEmpty()) QFile::remove(annAbs);
    bump();
    scheduleSave();
}

void VideoNotesModel::bump()
{
    ++revision_;
    emit revisionChanged();
}

void VideoNotesModel::reloadFromDisk()
{
    if (mediaPath_.isEmpty()) return;
    loading_ = true;
    emit loadingChanged();
    QPointer<VideoNotesModel> self(this);
    const QString path = mediaPath_;
    QThreadPool::globalInstance()->start([self, path] {
        std::vector<qcv::AnnotationNote> loaded;
        qcv::annotation_io::loadNotes(loaded, path);
        QMetaObject::invokeMethod(self, [self, path, loaded = std::move(loaded)]() mutable {
            if (!self || self->mediaPath_ != path) return;
            self->applyLoaded(std::move(loaded));
        }, Qt::QueuedConnection);
    });
}

void VideoNotesModel::applyLoaded(std::vector<qcv::AnnotationNote> loaded)
{
    loading_ = false;
    emit loadingChanged();
    // Disk is the merge baseline; locally deleted keys stay dead until the
    // delete has round-tripped through a save.
    loaded.erase(std::remove_if(loaded.begin(), loaded.end(),
        [this](const qcv::AnnotationNote &n) { return tombstones_.contains(n.timecode); }),
        loaded.end());
    if (sameNotes(loaded, notes_)) return;   // our own save echoed back — no churn
    qInfo("VideoNotesModel: loaded %d notes from %s",
          int(loaded.size()),
          qPrintable(qcv::annotation_io::getNotesJsonPath(mediaPath_)));
    notes_ = std::move(loaded);
    bump();
}

void VideoNotesModel::scheduleSave()
{
    if (mediaPath_.isEmpty()) return;
    QPointer<VideoNotesModel> self(this);
    const QString path = mediaPath_;
    const std::vector<qcv::AnnotationNote> snapshot = notes_;
    const QSet<QString> tombstones = tombstones_;
    QThreadPool::globalInstance()->start([self, path, snapshot, tombstones]() mutable {
        // Merge-on-save: a QCView save we haven't observed (watcher latency,
        // same-instant edits) must survive. Ours wins per timecode; theirs
        // survives for keys we don't hold and didn't delete.
        std::vector<qcv::AnnotationNote> disk;
        qcv::annotation_io::loadNotes(disk, path);
        std::vector<qcv::AnnotationNote> merged = snapshot;
        for (qcv::AnnotationNote &d : disk) {
            const bool ours = std::any_of(merged.cbegin(), merged.cend(),
                [&](const qcv::AnnotationNote &n) { return n.timecode == d.timecode; });
            if (!ours && !tombstones.contains(d.timecode))
                merged.push_back(std::move(d));
        }
        std::sort(merged.begin(), merged.end());
        const bool grew = merged.size() != snapshot.size();
        if (!qcv::annotation_io::saveNotes(merged, path))
            qWarning("VideoNotesModel: save FAILED for %s", qPrintable(path));
        const qint64 mt = fileMtimeMs(qcv::annotation_io::getNotesJsonPath(path));
        QMetaObject::invokeMethod(self, [self, path, mt, grew] {
            if (!self || self->mediaPath_ != path) return;
            self->selfWriteMtimes_.insert(mt);
            self->tombstones_.clear();  // round-tripped — deletes are durable now
            self->rewatch();            // QSaveFile rename drops the file watch
            // The merge pulled in notes we didn't have → bring them into memory.
            if (grew) self->reloadFromDisk();
        }, Qt::QueuedConnection);
    });
}

void VideoNotesModel::captureCleanThumb(QString timecode, int frame)
{
    if (mediaPath_.isEmpty()) return;
    QPointer<VideoNotesModel> self(this);
    const QString path = mediaPath_;
    QThreadPool::globalInstance()->start([self, path, timecode, frame] {
        if (!qcv::annotation_io::ensureImagesFolderExists(path)) return;
        const QString abs = qcv::annotation_io::getImagesFolder(path)
            + QLatin1Char('/') + qcv::annotation_io::generateImageFilename(timecode);
        // Idempotent: the clean PNG is the baseline QCView recomposites its
        // annotated overlay from — never re-capture over it.
        if (!QFileInfo::exists(abs)) {
            const QImage img = MediaStore::extractFrame(path, frame, 0);
            if (img.isNull() || !img.save(abs, "PNG")) {
                qWarning("VideoNotesModel: clean thumb failed for %s", qPrintable(abs));
                return;
            }
        }
        // Revision bump → the card's Image (revision-keyed source) retries.
        QMetaObject::invokeMethod(self, [self, path] {
            if (self && self->mediaPath_ == path) self->bump();
        }, Qt::QueuedConnection);
    });
}

void VideoNotesModel::rewatch()
{
    if (mediaPath_.isEmpty()) return;
    const QString json = qcv::annotation_io::getNotesJsonPath(mediaPath_);
    if (QFileInfo::exists(json)) {
        if (!watcher_.files().contains(json)) watcher_.addPath(json);
        return;
    }
    // notes.json doesn't exist yet — watch the nearest existing ancestor so
    // its creation (by QCView or our first save) flips us onto the file.
    QDir dir(QFileInfo(json).absolutePath());
    while (!dir.exists() && dir.cdUp()) {}
    const QString dirPath = dir.absolutePath();
    if (dir.exists() && !watcher_.directories().contains(dirPath))
        watcher_.addPath(dirPath);
}
