#pragma once
// File-system link helpers shared by the page model, the canvas and the exporters (2.0 phase A1).
// ONE recipe for the sibling ufb browser's deep link — ufb:///{os}/{percent-encoded path, slashes
// literal} (ufb's build_path_uri; `union:///` is an alias on ufb's side) — and the platform "reveal"
// / "open" actions that BlockModel::revealMedia / openMediaInUfb and Exporter::ufbLinkFor each
// carried their own copy of. Nothing here touches a document: callers hand in a resolved local path.
#include <QString>

namespace mn {

// "" for an empty path. Backslashes normalise to '/' so a Windows path reads the same from any OS.
QString ufbLinkFor(const QString& path);

// Select the file (or open the directory) in Finder / Explorer / the desktop's file manager. No-op
// when the path does not exist.
void revealInFileManager(const QString& path);
// Open a directory in the file manager (the folder itself, not its parent).
void openFolder(const QString& dir);
// Hand the path to the sibling ufb browser (no-op when no handler is registered for the scheme).
void openInUfb(const QString& path);
// The platform's default application for the file (a folder opens in the file manager).
void openWithDefaultApp(const QString& path);

} // namespace mn
