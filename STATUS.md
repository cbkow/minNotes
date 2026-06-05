# minNotes — Build Status

> Living tracker of what's built vs planned. Design rationale lives in
> `DESIGN.md`; the virtualization/architecture validation in `SPIKE.md`.
> Last updated: 2026-06-05.

## Stack (decided)
- **Pure C++/Qt6** (Qt 6.11) + QML. **No Rust / cxx-qt** — the spike proved the
  whole stack in C++, the central object is a `QAbstractListModel` (cxx-qt's
  weak spot), and the ufb reuse we want (media/chassis) is C++ anyway. Rust may
  return later only as an isolated module behind plain `cxx` if it earns it.
- Build: CMake + Ninja (`just build` / `just run`), presets `debug`/`release`.
- Storage: SQLite-canonical document (`Qt6::Sql`), the doc file *is* the DB.

## Layout
```
app/
  main.cpp                       sets the blockModel context property
  core/
    FenwickTree.h                cumulative-height index (scrollY↔block, O(log n))
    BlockModel.{h,cpp}           QAbstractListModel over the store + Fenwick;
                                 edit ops, markdown rule table, fractional ranks
    Document.{h,cpp}             SQLite store: blocks table, ULID ids, WAL,
                                 skinny-scan read + write-through edits
  qml/MinNotes/App/
    Main.qml                     window
    Editor.qml                   passive-surface editor (model owns the cursor;
                                 overlay caret/selection; mouse + keyboard)
    Theme.qml                    dark design-token singleton (ufb family palette)
spike/                           throwaway harness that validated the architecture
DESIGN.md  SPIKE.md  STATUS.md
```

## Done
- [x] **Virtualization spike** — Flickable + Fenwick (exact scrollbar) beats
      ListView; passive-surface editor (model-owned cursor) beats per-TextEdit;
      render headroom for 120 Hz (the cap is a Qt-macOS platform issue, banked).
      See `SPIKE.md`.
- [x] **Pure-C++ app skeleton** — builds & runs the editor.
- [x] **SQLite store (Phase 1a)** — blocks table, ULID ids, WAL, skinny-scan
      load; document persists across restarts.
- [x] **Persisted edits (Phase 1b)** — write-through UPDATE/INSERT/DELETE with
      lexicographic **fractional ranks** (`rankBetween`, unit-tested).
- [x] **Mouse** — click, cross-block drag-select, shift-click, double-click word,
      edge auto-scroll.
- [x] **Markdown autoformat** — `#`..`######`→heading, `>`→quote, `-`/`*`/`+`→
      list, `---`→divider; marker-free, persisted, styled, reload-correct.
- [x] **Dark theme** — `Theme.qml` token singleton wired through the editor.

## Next (rough order)
- [ ] **Code blocks** (` ``` ` fence) + **inline markdown** (`**bold**`, `*ital*`,
      `` `code` `` rendered within block content — currently shown raw).
- [ ] **Images / video** — adopt ufb's media pipeline (`QQuickRhiItem`, async
      providers, FFmpeg probe); intrinsic-dims-in-`attrs` for jump-free layout
      (DESIGN §7). Deferred per discussion until after code/inline.
- [ ] **Export to markdown** — the rule table *reverse* (copy-as-md / file
      export); the "one rule table drives autoformat + export" payoff.
- [ ] **Lazy windowed content fetch** — true two-tier read (loads all content
      eagerly today).
- [ ] **Document management** — new / open file (one fixed `scratch.mndb` now).
- [ ] **FTS5 search**; real window chrome (title bar / menus); macOS packaging.

## Known issues / banked
- **120 Hz on macOS**: render work is ~1.4 ms/frame (ample headroom) but Qt
  presents at 60 Hz — a Qt-cocoa platform limitation (refresh detection +
  CVDisplayLink), not our code. Affects all Qt apps. See `SPIKE.md` §6.
