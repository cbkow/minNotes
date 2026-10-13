// VideoNotesModel — the QML adapter over the QCView-interop notes sidecar
// (.qcview/<media>/notes.json; see PLAN-video-annotations.md). The on-disk
// layer is the verbatim QCView port (annotation_note / annotation_io); this
// object owns the GUI-thread note list and adds the minNotes-side glue:
//
//   - QCView's EXACT timecode minting (non-drop, fps*1000/1000, frame-0
//     origin — copied from WindowManager::addNoteAtCurrentFrame) so both
//     apps key the same frame to the same note instead of minting twins.
//   - Clean-frame thumbnail capture via MediaStore::extractFrame, written
//     as images/note_<TC>.png. IDEMPOTENT: an existing clean PNG is never
//     overwritten (it's the baseline QCView recomposites strokes onto).
//   - A QFileSystemWatcher reload so QCView's edits appear live here.
//   - Merge-on-save keyed by timecode + local delete tombstones, so a
//     QCView save we haven't observed yet is never clobbered by ours.
//     Policy: for a timecode both sides hold, OUR copy wins (the watcher
//     keeps us fresh, so a conflict means a same-instant edit); disk-only
//     timecodes survive unless we deleted them locally (tombstone).
//
// Notes live OUTSIDE the document DB on purpose (they travel with the
// video, shared with QCView) — nothing here may touch beginTxn/endTxn.

#pragma once

#include "annotation_note.h"

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration>

#include <vector>

class VideoNotesModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mediaPath READ mediaPath WRITE setMediaPath NOTIFY mediaPathChanged FINAL)
    Q_PROPERTY(double fps READ fps WRITE setFps NOTIFY fpsChanged FINAL)
    // Stream mode (QCView plan C, 2026-10-13): the notes of a live feed live
    // in a project sidecar folder — <project>/.qcview/streams/<name>__<id8>/ —
    // with no media file behind them. Point here instead of mediaPath; the
    // keys are capture stamps (sorted chronologically), no frame is minted,
    // no thumbnail extracted; the header (stream, stream_id, stream_url,
    // created) round-trips untouched.
    Q_PROPERTY(QString sidecarDir READ sidecarDir WRITE setSidecarDir NOTIFY sidecarDirChanged FINAL)
    Q_PROPERTY(bool isStream READ isStream NOTIFY sidecarDirChanged FINAL)
    Q_PROPERTY(QString streamName READ streamName NOTIFY revisionChanged FINAL)
    // Bumped on ANY note change (CRUD, external reload, thumbnail written) —
    // QML bindings tuple through it, the BlockModel revision pattern.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY revisionChanged FINAL)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged FINAL)
    QML_ELEMENT

public:
    explicit VideoNotesModel(QObject *parent = nullptr);

    QString mediaPath() const { return mediaPath_; }
    void setMediaPath(const QString &path);
    QString sidecarDir() const { return sidecarDir_; }
    void setSidecarDir(const QString &dir);
    bool isStream() const { return !sidecarDir_.isEmpty(); }
    QString streamName() const;
    // Insert the notes as blocks into `blockModel` (a BlockModel) after
    // `afterRow`: per note an image block of the clean PNG with the strokes
    // as frame-space ink, then a paragraph with the capture (or timecode)
    // line and the text. One undo group. Returns the notes inserted.
    Q_INVOKABLE int insertNotesAsBlocks(QObject *blockModel, int afterRow) const;
    static int insertNotesFromDir(QObject *blockModel, const QString &dir, int afterRow);
    double fps() const { return fps_; }
    void setFps(double fps);
    int revision() const { return revision_; }
    int count() const { return static_cast<int>(notes_.size()); }
    bool loading() const { return loading_; }

    // Card list for the filmstrip Repeater: one QVariantMap per note —
    // { timecode, frame, text, addressed, image (abs path; prefers the
    //   _annotated sibling when QCView drew strokes), hasStrokes }.
    Q_INVOKABLE QVariantList noteList() const;

    // Mint (or refresh) the note at `frame` using QCView's timecode rules.
    // Existing timecode → no duplicate; the clean thumbnail is (re)ensured
    // either way. Returns the timecode.
    Q_INVOKABLE QString addNoteAtFrame(int frame);
    Q_INVOKABLE void setText(const QString &timecode, const QString &text);
    Q_INVOKABLE void setAddressed(const QString &timecode, bool addressed);
    // Explicit user delete — the one path that also wipes the note's PNGs
    // (QCView preserves them on its undo path; we mirror the user-click one).
    Q_INVOKABLE void removeNote(const QString &timecode);

    // ---- Stroke seam (VideoAnnotator) ----
    Q_INVOKABLE QString timecodeForFrame(int frame) const { return mintTimecode(frame); }
    Q_INVOKABLE bool hasNote(const QString &timecode) const { return noteAt(timecode) != nullptr; }
    Q_INVOKABLE QString annotationDataFor(const QString &timecode) const;
    // Replace a note's stroke JSON (empty = no strokes; serializes as null,
    // QCView's exact erased-to-zero shape). Recreates a missing note at
    // `frame` when json is non-empty — that's redo after undoing the stroke
    // that created the note.
    void setAnnotationData(const QString &timecode, int frame, const QString &json);
    // Annotation-undo delete: the note goes, its PNGs stay (a redo must be
    // able to restore them) — QCView's deleteNote semantics.
    void removeNoteKeepFiles(const QString &timecode);
    // Recomposite images/note_<TC>_annotated.png from the clean PNG + the
    // note's current strokes (worker thread; removes the file when no
    // strokes remain). Callers debounce — one write per drawing burst.
    void writeAnnotatedThumb(const QString &timecode);

signals:
    void mediaPathChanged();
    void sidecarDirChanged();
    void fpsChanged();
    void revisionChanged();
    void loadingChanged();

private:
    QString mintTimecode(int frame) const;
    // The source behind the notes: the media path, or the stream folder.
    QString source() const { return sidecarDir_.isEmpty() ? mediaPath_ : sidecarDir_; }
    QString notesJsonPath() const;
    QString imagesFolder() const;
    const qcv::AnnotationNote *noteAt(const QString &timecode) const;
    qcv::AnnotationNote *noteAt(const QString &timecode);
    void bump();
    void reloadFromDisk();              // async read → marshalled apply
    void applyLoaded(std::vector<qcv::AnnotationNote> loaded);
    void scheduleSave();                // snapshot → worker merge-save
    void captureCleanThumb(QString timecode, int frame);
    void rewatch();                     // watch notes.json, or nearest dir until it exists

    QString mediaPath_;
    QString sidecarDir_;
    QJsonObject header_;                // stream mode: the notes.json header, round-tripped
    double fps_ = 0.0;
    int revision_ = 0;
    bool loading_ = false;

    std::vector<qcv::AnnotationNote> notes_;
    QSet<QString> tombstones_;          // timecodes deleted locally since last sync

    QFileSystemWatcher watcher_;
    QTimer reloadDebounce_;
    // mtimes (ms since epoch) of our own writes — used to skip the watcher
    // reload our own QSaveFile rename triggers.
    QSet<qint64> selfWriteMtimes_;
};
