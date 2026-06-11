// annotation_io — VERBATIM port of QCView-Player's
// src/annotations/annotation_io.{h,cpp} (2026-06-11). The notes sidecar is
// a two-way interchange contract with QCView (see PLAN-video-annotations.md):
// folder layout, JSON shape and filenames must stay byte-compatible — fix
// QCView first, then re-port.
//
// Folder layout + JSON read/write for note metadata. Two layouts:
//
//   1. Per-media (single clip) — the one minNotes uses:
//        <media_dir>/.qcview/<media_filename>/notes.json
//        <media_dir>/.qcview/<media_filename>/images/note_<TC>.png
//
//   2. Per-project-timeline (QCView projects; unused here but kept so the
//      file stays diffable against QCView's copy):
//        <project_dir>/.qcview/<timeline_name>/notes.json
//        <project_dir>/.qcview/<timeline_name>/images/note_<TC>.png
//
// Read-side falls back to legacy `.ump` folder if `.qcview` doesn't
// exist (old "UMP" alpha used .ump). Write-side always creates
// `.qcview`. Writes are atomic (QSaveFile).

#pragma once

#include "annotation_note.h"

#include <QString>

#include <functional>
#include <vector>

namespace qcv::annotation_io {

// ---- Per-media path helpers ----
QString getQcViewPath(const QString &mediaPath);
QString getNotesJsonPath(const QString &mediaPath);
QString getImagesFolder(const QString &mediaPath);
QString sanitizeMediaName(const QString &filename);
QString generateImageFilename(const QString &timecode);

// ---- Per-project-timeline path helpers ----
// `projectPath` is the path to the project file (e.g. "foo.qcvproj");
// the project dir is its parent. timelineName is sanitized.
QString getProjectAnnotationPath(const QString &projectPath,
                                 const QString &timelineName);
QString getProjectImagesFolder(const QString &projectPath,
                               const QString &timelineName);
bool createProjectQcViewFolder(const QString &projectPath,
                               const QString &timelineName);

// ---- Folder management ----
bool createQcViewFolder(const QString &mediaPath);
bool ensureImagesFolderExists(const QString &mediaPath);

// ---- JSON I/O (sync) ----
bool saveNotes(const std::vector<AnnotationNote> &notes,
               const QString &mediaPath);
bool loadNotes(std::vector<AnnotationNote> &notes,
               const QString &mediaPath);

// ---- Async wrappers (fire-and-forget, off the calling thread) ----
using LoadCallback =
    std::function<void(bool success, const std::vector<AnnotationNote> &)>;
void loadNotesAsync(const QString &mediaPath, LoadCallback callback);
void saveNotesAsync(const std::vector<AnnotationNote> &notes,
                    const QString &mediaPath);

} // namespace qcv::annotation_io
