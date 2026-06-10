# Plan: Document management — menu bar, save/save-as/open, settings

> Status: **ratified direction, sequenced.** Captured 2026-06-10 after inspecting
> the sister apps (ufb / MinRender / QCView-Player) for their menu + settings
> patterns. End goal (later milestone): multiple open documents in tabs — which
> REQUIRES this arc (a real file model + menu chassis) first.

## What the sister apps settled

- **Menu bar = Qt Quick Controls 2 `MenuBar`** (MinRender + QCView both): one
  in-window menu strip on every platform — consistent with the family's custom
  flat chrome (no native macOS global-menu detour via Qt.labs.platform).
  QCView's **`ThemedMenu`** wrapper is the reference: palette-recolored to the
  theme + `popupType: Popup.Window` (Windows child-HWND z-order fix). Port it,
  squared per [[minnotes-style-rules]].
- **Dialogs = `QtQuick.Dialogs` FileDialog** (native on all platforms), wired
  `onAccepted → C++ invokable`, with `nameFilters`.
- **Shortcuts on the menu `Action`s** via `StandardKey.Open/Save/SaveAs` —
  platform-aware (Cmd on macOS, Ctrl elsewhere). The shortcut system intercepts
  BEFORE item Keys handlers, so the editor's key routing is untouched; ⌘N/⌘O/⌘S
  are currently unbound in the editor. QCView also proves the "Recent" submenu:
  a `Repeater` over a C++ string-list property, QSettings-backed, cap 10.
- **Settings = QtCore `Settings` (QSettings)**; org/app name already set in
  main.cpp. minNotes already uses this for swatches + board prefs. UFB's window
  geometry persistence is the most battle-tested: debounce 500ms, capture only
  windowed-mode geometry, screen-overlap validation before restoring position,
  explicit maximized flag. **(Window geometry: DONE — Main.qml, the first
  setting.)**

## The file model: write-through changes what "Save" means

The document file IS a SQLite DB with per-edit write-through — **there is no
dirty state and never will be**. Don't fight that with a scratch-copy/dirty-flag
model; embrace it (Bear/Notion semantics) while keeping the expected menu verbs:

- **Save (⌘S)** = `wal_checkpoint(TRUNCATE)` — flushes the WAL into the main DB
  file so the on-disk single file is complete/portable at that instant. Cheap,
  honest, satisfies muscle memory. (Optionally flash "Saved" in the BottomRail.)
- **Save As… (⌘⇧S)** = copy the CURRENT doc to a new path and switch to it:
  checkpoint → `VACUUM INTO 'new.min'` (atomic, compacting) → **copy the media
  sidecar** → reopen at the new path. ⚠️ The `.minnotes/` sidecar (content-hashed
  media bytes, keyed off docPath in MediaStore) MUST travel with the copy or
  images break. Also offer **Save a Copy…** later (same op, don't switch).
- **Open… (⌘O)** = close current (checkpoint), open another DB at path.
- **New (⌘N)** = prompt for a location up front (a DB needs a file), create
  empty doc there, open it. (No "untitled buffer" — write-through needs a file.
  v1 keeps it simple; an untitled-temp-promoted-on-save flow can come later.)
- **Open Recent** = QSettings list, cap 10, QCView's Repeater pattern.

**Extension: `.min`** (user's pick; current internal scratch stays
`scratch.mndb` until first Save As). FileDialog filter:
`"minNotes document (*.min *.mndb)"`. The doc is self-contained EXCEPT sidecar
media + in-place `file://` refs — Save As copies the sidecar; in-place refs stay
refs (documented behaviour).

## C++ work (BlockModel/Document)

All document state lives in BlockModel members (rows_/content_/ids_/ranks_/
fenwick_/undo_/caches) + Document + MediaStore. Needed invokables:

- `bool openDocument(QString path)` — teardown (commit txn, clear undo stack,
  reset caches/revisions, beginResetModel) → `doc_.open(path)` → rebuild
  MediaStore on the new path → `loadFromStore()` → reset cursor to (0,0) QML-side.
  The constructor's scratch-open becomes `openDocument(defaultPath)`.
- `bool newDocument(QString path)` — create + open (skip the synthetic seed for
  user-created docs: seed ONE empty paragraph instead).
- `bool saveCheckpoint()` — WAL checkpoint.
- `bool saveDocumentAs(QString path)` — checkpoint → VACUUM INTO → sidecar copy
  → openDocument(path).
- `documentPath` already exposed (BottomRail). Add a `documentName` for the
  window title: `title: blockModel.documentName + " — minNotes"`.
- Recents: tiny QSettings helpers in C++ (QCView's window_manager.cpp:6776
  pattern) or pure-QML JSON like boardPrefs — either fine; C++ matches family.

QML-side resets on document switch: tcur/cursor, activeTableId/PdfId ("" →
Document tab), boardMode, undo buttons, video/pdf row recomputes (these hang off
contentChangedSpike/model reset — verify after openDocument's beginResetModel).

## Menu structure (v1)

- **File**: New… ⌘N · Open… ⌘O · Open Recent ▸ (+ Clear) · ── · Save ⌘S ·
  Save As… ⌘⇧S · ── · Reveal in Finder/Show in Explorer · ── · Quit (Windows; macOS
  gets Quit in the app menu automatically… note: with an in-window QQC2 MenuBar
  there IS no native app menu — keep Quit visible everywhere).
- **Edit**: Undo/Redo (route to blockModel), Cut/Copy/Paste (route to editor) —
  optional v1; the shortcuts already work, menu items are discoverability.
- Place the MenuBar via `ApplicationWindow.menuBar` in Main.qml; ThemedMenu port
  styled flat/squared; height folds into the existing column layout.

## Milestones

- **DM-1 — Window geometry persistence.** DONE (this commit).
- **DM-2 — Menu chassis**: ThemedMenu port + MenuBar with File menu wired to
  stub invokables; window title shows documentName. Smallest visible step.
- **DM-3 — openDocument/newDocument/saveCheckpoint/saveDocumentAs** in C++ +
  FileDialogs + recents. The real meat; test the QML reset list hard
  (tabs/board/undo/cursor after switch).
- **DM-4 — Polish**: Save feedback flash, Reveal item, drag-a-.min-onto-window
  to open, file association/registration (later, packaging time).
- **(Later, separate arc) Multi-document tabs**: requires per-document
  BlockModel instances — today `blockModel` is ONE context property assumed
  globally by every QML file. That refactor (injected model property or one
  window per doc) is the known cost; do NOT start it inside this arc.

## Open questions

- Default startup doc once real files exist: reopen last document (recents[0])
  vs always scratch? (Lean: reopen last, fall back to scratch.)
- `.min` association/icon — packaging-time work, not this arc.
- Editor key handler vs menu shortcuts: verify ⌘S/⌘O/⌘N reach Actions while the
  editor FocusScope has focus (shortcut system should win; test on both OSes).
