#include "VideoNotesModel.h"

#include "annotation_io.h"
#include "annotation_serializer.h"
#include "annotation_thumbnail.h"
#include "../core/BlockModel.h"
#include "../core/MediaStore.h"
#include "../player/timecode_formatter.h"

#include <QDateTime>
#include <QUrl>
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
        const qint64 mt = fileMtimeMs(notesJsonPath());
        if (selfWriteMtimes_.contains(mt)) return;
        reloadFromDisk();
    });
    auto poke = [this](const QString &) { rewatch(); reloadDebounce_.start(); };
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, poke);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, poke);
}

QString VideoNotesModel::notesJsonPath() const
{
    if (!sidecarDir_.isEmpty()) return qcv::annotation_io::notesJsonPathIn(sidecarDir_);
    return mediaPath_.isEmpty() ? QString() : qcv::annotation_io::getNotesJsonPath(mediaPath_);
}

QString VideoNotesModel::imagesFolder() const
{
    if (!sidecarDir_.isEmpty()) return qcv::annotation_io::imagesFolderIn(sidecarDir_);
    return mediaPath_.isEmpty() ? QString() : qcv::annotation_io::getImagesFolder(mediaPath_);
}

QString VideoNotesModel::streamName() const
{
    return header_.value(QStringLiteral("stream")).toString();
}

void VideoNotesModel::setSidecarDir(const QString &dir)
{
    if (dir == sidecarDir_) return;
    sidecarDir_ = dir;
    mediaPath_.clear();
    header_ = QJsonObject();
    notes_.clear();
    tombstones_.clear();
    selfWriteMtimes_.clear();
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
    emit sidecarDirChanged();
    emit mediaPathChanged();
    bump();
    if (!sidecarDir_.isEmpty()) {
        qInfo("VideoNotesModel: stream folder -> %s", qPrintable(sidecarDir_));
        rewatch();
        reloadFromDisk();
    }
}

void VideoNotesModel::setMediaPath(const QString &path)
{
    if (path == mediaPath_ && sidecarDir_.isEmpty()) return;
    mediaPath_ = path;
    if (!sidecarDir_.isEmpty()) { sidecarDir_.clear(); header_ = QJsonObject(); emit sidecarDirChanged(); }
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
    const QString imagesDir = imagesFolder();
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
        // Stream notes: when it was captured and the host's timecode.
        const bool stream = n.isStreamNote();
        m.insert(QStringLiteral("isStream"),     stream);
        m.insert(QStringLiteral("capturedAt"),   n.captured_at);
        m.insert(QStringLiteral("hostTimecode"), n.host_timecode);
        m.insert(QStringLiteral("stream"),       n.stream);
        const QDateTime at = QDateTime::fromString(n.captured_at, Qt::ISODateWithMs);
        m.insert(QStringLiteral("captureLabel"), at.isValid() ? at.toString(QStringLiteral("HH:mm:ss.zzz")) : QString());
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
    if (mediaPath_.isEmpty()) return {};   // a stream's notes are minted by QCView, from the feed
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
    const QString cleanAbs = imagesFolder()
        + QLatin1Char('/') + qcv::annotation_io::generateImageFilename(timecode);
    QFile::remove(cleanAbs);
    const QString annAbs = annotatedSibling(cleanAbs);
    if (!annAbs.isEmpty()) QFile::remove(annAbs);
    bump();
    scheduleSave();
}

QString VideoNotesModel::annotationDataFor(const QString &timecode) const
{
    const qcv::AnnotationNote *n = noteAt(timecode);
    return n ? n->annotation_data : QString();
}

void VideoNotesModel::setAnnotationData(const QString &timecode, int frame,
                                        const QString &json)
{
    qcv::AnnotationNote *n = noteAt(timecode);
    if (!n) {
        if (json.isEmpty()) return;          // nothing to restore onto nothing
        addNoteAtFrame(frame);               // recreate (redo path); idempotent thumb
        n = noteAt(timecode);
        if (!n) return;
    }
    if (n->annotation_data == json) return;
    n->annotation_data = json;
    bump();
    scheduleSave();
}

void VideoNotesModel::removeNoteKeepFiles(const QString &timecode)
{
    auto it = std::find_if(notes_.begin(), notes_.end(),
        [&](const qcv::AnnotationNote &n) { return n.timecode == timecode; });
    if (it == notes_.end()) return;
    notes_.erase(it);
    tombstones_.insert(timecode);
    bump();
    scheduleSave();
}

void VideoNotesModel::writeAnnotatedThumb(const QString &timecode)
{
    const qcv::AnnotationNote *n = noteAt(timecode);
    if (!n || source().isEmpty()) return;
    // Snapshot on the GUI thread; the worker only touches files.
    const QString cleanAbs = imagesFolder()
        + QLatin1Char('/') + qcv::annotation_io::generateImageFilename(timecode);
    const QString annAbs = annotatedSibling(cleanAbs);
    const std::vector<qcv::ActiveStroke> strokes =
        qcv::AnnotationSerializer::jsonStringToStrokes(n->annotation_data);
    QPointer<VideoNotesModel> self(this);
    const QString path = source();
    QThreadPool::globalInstance()->start([self, path, cleanAbs, annAbs, strokes] {
        if (strokes.empty()) {
            QFile::remove(annAbs);
        } else {
            const QImage clean(cleanAbs);
            // PAR 1/1: minNotes doesn't track pixel aspect (storage dims only).
            const QImage out = qcv::renderNoteThumbnail(clean, strokes, 1, 1);
            if (out.isNull() || !out.save(annAbs, "PNG")) {
                qWarning("VideoNotesModel: annotated thumb failed for %s",
                         qPrintable(annAbs));
                return;
            }
        }
        QMetaObject::invokeMethod(self, [self, path] {
            if (self && self->source() == path) self->bump();
        }, Qt::QueuedConnection);
    });
}

void VideoNotesModel::bump()
{
    ++revision_;
    emit revisionChanged();
}

void VideoNotesModel::reloadFromDisk()
{
    if (source().isEmpty()) return;
    loading_ = true;
    emit loadingChanged();
    QPointer<VideoNotesModel> self(this);
    const QString path = source();
    const bool stream = isStream();
    QThreadPool::globalInstance()->start([self, path, stream] {
        std::vector<qcv::AnnotationNote> loaded;
        QJsonObject header;
        if (stream) qcv::annotation_io::loadNotesAt(path, loaded, &header);
        else        qcv::annotation_io::loadNotes(loaded, path);
        QMetaObject::invokeMethod(self, [self, path, header, loaded = std::move(loaded)]() mutable {
            if (!self || self->source() != path) return;
            if (self->isStream()) self->header_ = header;
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
          qPrintable(notesJsonPath()));
    notes_ = std::move(loaded);
    bump();
}

void VideoNotesModel::scheduleSave()
{
    if (source().isEmpty()) return;
    QPointer<VideoNotesModel> self(this);
    const QString path = source();
    const bool stream = isStream();
    const QJsonObject header = header_;
    const QString jsonPath = notesJsonPath();
    const std::vector<qcv::AnnotationNote> snapshot = notes_;
    const QSet<QString> tombstones = tombstones_;
    QThreadPool::globalInstance()->start([self, path, stream, header, jsonPath, snapshot, tombstones]() mutable {
        // Merge-on-save: a QCView save we haven't observed (watcher latency,
        // same-instant edits) must survive. Ours wins per timecode; theirs
        // survives for keys we don't hold and didn't delete.
        std::vector<qcv::AnnotationNote> disk;
        QJsonObject diskHeader;
        if (stream) qcv::annotation_io::loadNotesAt(path, disk, &diskHeader);
        else        qcv::annotation_io::loadNotes(disk, path);
        std::vector<qcv::AnnotationNote> merged = snapshot;
        for (qcv::AnnotationNote &d : disk) {
            const bool ours = std::any_of(merged.cbegin(), merged.cend(),
                [&](const qcv::AnnotationNote &n) { return n.timecode == d.timecode; });
            if (!ours && !tombstones.contains(d.timecode))
                merged.push_back(std::move(d));
        }
        std::sort(merged.begin(), merged.end());
        const bool grew = merged.size() != snapshot.size();
        bool saved;
        if (stream) {
            // The header on disk is the baseline (QCView refreshes the name);
            // ours only fills what it lacks.
            QJsonObject h = diskHeader;
            for (auto it = header.begin(); it != header.end(); ++it) if (!h.contains(it.key())) h.insert(it.key(), it.value());
            saved = qcv::annotation_io::saveNotesAt(path, h, merged);
        } else {
            saved = qcv::annotation_io::saveNotes(merged, path);
        }
        if (!saved) qWarning("VideoNotesModel: save FAILED for %s", qPrintable(path));
        const qint64 mt = fileMtimeMs(jsonPath);
        QMetaObject::invokeMethod(self, [self, path, mt, grew] {
            if (!self || self->source() != path) return;
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
    if (mediaPath_.isEmpty()) return;   // stream notes bring their own capture
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

// ---- notes → blocks (plan C) --------------------------------------------------------

namespace {
int insertNotes(BlockModel *bm, const std::vector<qcv::AnnotationNote> &notes,
                const QString &imagesDir, int afterRow)
{
    if (!bm || notes.empty()) return 0;
    int row = afterRow;
    int inserted = 0;
    bm->beginGroup(std::max(0, afterRow), std::max(0, afterRow));
    for (const qcv::AnnotationNote &n : notes) {
        const QString png = imagesDir + QLatin1Char('/')
            + qcv::annotation_io::generateImageFilename(n.timecode);
        int r = -1;
        if (QFileInfo::exists(png)) {
            r = bm->insertImageFromUrl(row, QUrl::fromLocalFile(png).toString(), /*forceCopy=*/true);
            if (r >= 0) {
                row = r;
                const std::vector<qcv::ActiveStroke> strokes =
                    qcv::AnnotationSerializer::jsonStringToStrokes(n.annotation_data);
                if (!strokes.empty())   // the note's strokes stay VECTOR, as the image's page-0 media ink:
                    bm->setMediaPageInk(row, 0, qcv::AnnotationSerializer::strokesToJsonString(strokes));   // QCView's normalized space, 1:1 with the sidecar
            }
        }
        // The caption: when (a stream) or where (a clip), then the text.
        QString head;
        if (n.isStreamNote()) {
            const QDateTime at = QDateTime::fromString(n.captured_at, Qt::ISODateWithMs);
            head = at.isValid() ? at.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) : n.timecode;
            if (!n.host_timecode.isEmpty()) head += QStringLiteral("  ·  TC ") + n.host_timecode;
        } else {
            head = n.timecode + QStringLiteral("  ·  frame ") + QString::number(n.frame);
        }
        QString caption = (n.addressed ? QStringLiteral("✅ ") : QString()) + head;
        if (!n.text.trimmed().isEmpty()) caption += QStringLiteral(" — ") + n.text.trimmed();
        const int p = bm->insertParagraphBelow(std::max(0, row));
        if (p >= 0) { bm->setContent(p, caption); row = p; }
        ++inserted;
    }
    bm->endGroup();
    return inserted;
}
} // namespace

int VideoNotesModel::insertNotesAsBlocks(QObject *blockModel, int afterRow) const
{
    auto *bm = qobject_cast<BlockModel *>(blockModel);
    if (!bm) return 0;
    return insertNotes(bm, notes_, imagesFolder(), afterRow);
}

int VideoNotesModel::insertNotesFromDir(QObject *blockModel, const QString &dir, int afterRow)
{
    auto *bm = qobject_cast<BlockModel *>(blockModel);
    if (!bm || dir.isEmpty()) return 0;
    std::vector<qcv::AnnotationNote> notes;
    QJsonObject header;
    qcv::annotation_io::loadNotesAt(dir, notes, &header);
    std::sort(notes.begin(), notes.end());
    return insertNotes(bm, notes, qcv::annotation_io::imagesFolderIn(dir), afterRow);
}

void VideoNotesModel::rewatch()
{
    if (source().isEmpty()) return;
    const QString json = notesJsonPath();
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
