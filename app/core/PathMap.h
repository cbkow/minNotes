#pragma once
#include <QString>
#include <QVector>
#include <QJsonValue>

// Cross-OS path mapping for referenced media/attachments. A "volume" pairs a
// stable id with the share's root on each OS; a referenced file is stored in the
// descriptor as an OS-neutral structured ref { "vol": <id>, "rel": [seg,…] } —
// `rel` is the path BELOW the volume root as separator-free segments, so no OS
// owns the canonical form and a literal backslash in a POSIX filename can't be
// mangled. Resolution = thisOS root + native-separator join of `rel`.
//
// Sibling apps UFB (core/src/utils.rs translate_path_to) and MinRender
// (src/core/path_mapping.h) do the same with canonical-Windows STRINGS; minNotes
// uses the segment form instead (decided in design chat). Mappings are app-wide
// (not per-document); PathMapController owns the list and feeds it here.
namespace mn {

struct PathMapping {
    QString id;       // stable, generated (ULID) — docs reference volumes by this
    QString label;    // human label for the dialog
    QString win;      // root on Windows, e.g. "Z:\\union-jobs"
    QString mac;      // root on macOS,   e.g. "/Volumes/union-jobs"
    QString lin;      // root on Linux,   e.g. "/mnt/union-jobs"
    bool enabled = true;
};

// "win" | "mac" | "lin" for the current build.
QString currentOsTag();

// The active app-wide mappings. Set once at startup + on every edit by
// PathMapController; read by MediaStore on the per-document resolve path.
const QVector<PathMapping>& activeMappings();
void setActiveMappings(QVector<PathMapping> mappings);

// Absolute current-OS path → portable ref. Returns a JSON object
// {vol,rel:[…]} when the path falls under an enabled mapping's current-OS root,
// else the absolute path as a JSON string (unmapped — stays local).
QJsonValue toRef(const QString& absPath);

// Stored descriptor `src` → absolute current-OS path.
//   - object {vol,rel} → mapping root[currentOS] + native-join(rel); "" if the
//     volume is unknown/disabled (caller renders a broken-media state).
//   - string → best-effort: if it matches some enabled mapping's any-OS root,
//     rebuild for the current OS (so legacy absolute-path docs gain portability);
//     otherwise returned unchanged. Relative ".minnotes/…" srcs are NOT handled
//     here (MediaStore resolves those against the document dir).
QString resolveRef(const QJsonValue& src);

} // namespace mn
