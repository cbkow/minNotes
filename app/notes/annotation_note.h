// AnnotationNote — VERBATIM port of QCView-Player's
// src/annotations/annotation_note.h (2026-06-11). The notes sidecar is a
// two-way interchange contract with QCView (see PLAN-video-annotations.md):
// do NOT change field names, types, or on-disk shape here — fix QCView
// first, then re-port.
//
// Per-note metadata. One AnnotationNote per timecode in a piece of
// media. Notes are always kept sorted by timecode (string compare).
//
// `annotation_data` is the schema's quirk: in memory it's a JSON
// string (the stroke list), on disk it's parsed into a nested object
// so the file is hand-readable. Round-trip preserves textual
// equivalence; whitespace differences are not significant.

#pragma once

#include <QString>

#include <utility>

class QJsonObject;

namespace qcv {

struct AnnotationNote {
    QString timecode;          // HH:MM:SS:FF — primary sort key
    double  timestamp_seconds = 0.0;
    int     frame             = 0;
    QString image_path;        // "images/note_HH_MM_SS_FF.png" (relative)
    QString annotation_data;   // JSON string (stroke list); empty if none
    QString text;              // user note (multi-line OK)
    bool    addressed         = false;

    AnnotationNote() = default;
    AnnotationNote(QString tc, double ts, int f, QString img, QString txt)
        : timecode(std::move(tc))
        , timestamp_seconds(ts)
        , frame(f)
        , image_path(std::move(img))
        , text(std::move(txt))
    {}

    bool operator<(const AnnotationNote &o) const  { return timecode < o.timecode; }
    bool operator==(const AnnotationNote &o) const { return timecode == o.timecode; }
};

// Serialize a single note. The annotation_data field is parsed back to
// a JSON object before embedding (preserves the on-disk shape: nested
// object, not stringified).
QJsonObject noteToJson(const AnnotationNote &note);

// Deserialize a single note from a JSON object. Returns false if
// required fields are missing.
bool noteFromJson(const QJsonObject &obj, AnnotationNote &out);

} // namespace qcv
