# minNotes — Build Status

> Living tracker of what's built vs planned. Design rationale lives in
> `DESIGN.md`; the virtualization/architecture validation in `SPIKE.md`.
> Last updated: 2026-06-06.

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
- [x] **Inline markdown (styling)** — `**bold**`, `*italic*`, `` `code` `` styled in
      place via `InlineMarkdownHighlighter` (a `QSyntaxHighlighter` exposed as a
      QML element, attached per pooled `TextEdit`). The `TextEdit` stays
      `PlainText` raw source → **identity caret positions**, so caret / selection
      / hit-testing / editing need *no* column mapping and no reveal-on-focus.
      Markers are **dimmed in place** (not hidden); enabled for paragraph/quote/
      list, off for code (markdown is literal in a fence). Colours themed from
      `Theme`. No HTML — an earlier HTML/`RichText` attempt was scrapped (Qt
      leaked its `QTextDocument.toHtml()` serialization when the format switched;
      per-block HTML was the wrong altitude).

      **Endgame (not yet built):** formatting becomes a SEMANTIC fact — a *span*
      stored in `attrs` (offsets + kind), **not** `**` in the text — mirroring how
      block headings already consume their marker. Menu **and** markdown both just
      trigger a span; raw markdown becomes a power-user *source mode* + the export
      path. The same highlighter then renders from spans (markers gone entirely
      for regular users). See [Inline formatting direction] note. Dimmed-markers
      is the power-user interim; the highlighter + PlainText/identity layer carry
      forward unchanged.
- [x] **Sticky goal-x vertical nav** — up/down keep the x the caret aimed for
      across a run (te-local, so consistent across blocks), instead of drifting
      toward shorter lines; reset on any horizontal move / edit / click. Crossing
      a block boundary lands at goal-x on the adjacent line, not column 0.
- [x] **Marker restyle + inline-code chip** — markdown markers render in the
      accent blue, flipping to white when inside the selection (`selStart`/
      `selEnd` → `selectedMarkerColor` on the highlighter). Inline code sits in a
      lighter `inlineCodeBg` chip so it stands out from prose. The chip is an
      **overlay rect** (`codeRangesForRow` → per-visual-line rects, drawn below
      the selection and glyphs), NOT a char-format background — a char background
      paints inside the TextEdit *above* the selection overlay and would occlude
      the highlight. Overlay layering = selecting code highlights it correctly.
- [x] **Semantic format spans (MVP — the endgame, first slice)** — bold/italic/
      code stored as `attrs.spans` `[{s,e,k}]`, **not** `**` in the text, so they
      render **clean with no markers**. `BlockModel.toggleFormat(row,start,end,
      kind)` (interval add/remove/merge) + `spansForRow`; **Cmd+B / Cmd+I** apply
      over the selection (per row for multi-block). Offsets tracked across
      `insertText`/`splitBlock`/`deleteRange`. The highlighter renders spans by
      accumulating per-char flags (bold+italic overlap combines). Verified:
      seeded spans show clean styled text, no faded marks.

- [x] **Markdown-as-input (commit on leave/load)** — markdown is now an *input
      method*, not storage. Type `**b**`/`*i*`/`` `c` `` in the focused block (blue
      markers visible); the moment the caret **leaves** the block (arrow across a
      boundary or click into another), `commitMarkdown` consumes the markers into
      spans and strips them → the block renders **clean** ("turns into a normal
      block"). `convertMarkdown` also runs **on load**, so blocks from disk are
      clean and markers only ever appear while you're actively typing them.
      One-way (re-entry stays clean; raw-markdown editing returns later as *source
      mode*). In-memory on load; the DB migrates lazily as blocks are edited.

- [x] **Undo / redo (region-snapshot transactions)** — one chokepoint for all
      mutations before formats multiply. Each undoable step snapshots the touched
      row-region `{id,rank,type,level,content,spans}` **before/after**; undo/redo
      just swap the region (generic — no per-op inverses; new formats inherit
      undo free). `beginTxn/endTxn` (depth-counted so multi-block ops group into
      one step) wrap every mutation; **typing and single-char deletes coalesce**;
      auto-transforms (`commitMarkdown`, `## `→heading) are discrete steps (Cmd-Z
      reverts to the literal markdown, per DESIGN §6). Caret is mirrored from QML
      via `noteCaret` and restored on undo/redo (`caretRestoreRequested`). Linear
      but **tree-ready**: each `UndoEntry` stores its `parent`; redo = newest
      child (branching UI later). `Cmd+Z` / `Cmd+Shift+Z` / `Cmd+Y`. Verified
      headless: coalesced typing, split/merge structural, and format all undo/redo
      correctly with right caret + counts.
- [x] **Remove styling** — `Cmd+\` clears ALL formatting over the selection
      (`clearFormat`); `Cmd+B`/`Cmd+I` now decide add-vs-remove **uniformly**
      across a multi-block selection (all-covered → remove, else add), grouped as
      one undo step (`beginGroup`/`endGroup`, `hasFormat`/`setFormat`).

## Next (rough order)
- [ ] **Spans — finish past MVP**: a menubar/toolbar to drive `toggleFormat`
      (today only Cmd+B/I); active-format-at-caret (type after toggling with no
      selection); source-mode toggle (reveal raw markdown); markdown typing
      (`**x**`) converts to a span + consumes markers; export reconstructs `**`.
      Tighten span bookkeeping on cross-block merge (adjacent same-kind not yet
      coalesced) and code spans (no `enabled`-gate interplay with code blocks).
- [ ] **Code blocks** (` ``` ` fence) — multi-line fenced block type (no inline
      formatting inside; markdown literal).
- [ ] **Inline markdown polish** — underscore emphasis (`_x_`, deliberately off to
      avoid `file_name_here`), `\*` escape, nesting (`***x***`), links.
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

## Banked ideas — sibling block editor (rubymamistvalove.com/block-editor)
A near-identical Qt6/QML/SQLite/Markdown block editor (its model is also a C++
`BlockModel`). Cross-validates our bets and offers a few things to steal:
- **Validates the spans endgame.** It keeps an editable RichText surface with
  Markdown underneath and reveals raw markdown when the cursor enters a span —
  which forced HTML↔Markdown conversion + longest-common-prefix cursor mapping +
  regex, and exposed a Qt `toMarkdown()` bug (markers not closed). That's the
  exact mapping hell our highlighter + (future) spans approach avoids. Don't go
  back to editable HTML / per-delegate `TextArea` (their multi-delegate selection
  path is the Arm-B desync we rejected).
- **Undo/redo (when we build it):** store old+new block text per op; **coalesce**
  single-char inserts into one CompoundAction (undo removes the whole typed run);
  merge multi-block indents into one step. Gotcha for future rich blocks
  (media/Kanban-like): keep their undo stacks IN the model, not in the block
  object, or deleting the block drops its history.
- **Replica pattern for drag-reorder:** virtualization destroys the dragged
  delegate when it scrolls off-screen → clone at drag-start, hide the original,
  drag the clone. Applies directly to our ring buffer when we add reordering.
- **`{{blockType "k":v}} … {{/blockType}}` encapsulation** for markdown export /
  clipboard of non-standard blocks (media/embeds) — keeps them round-trippable in
  plain text. For the export-to-markdown task; storage stays SQLite type+attrs.
- They confirm our other bets: per-block write-through (not whole-doc save) is the
  right call; ListView drag/pooling friction is why we chose Flickable+Fenwick;
  macOS scroll smoothness has a Qt ceiling (already banked above).
