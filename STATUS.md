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
      accent blue. (White-on-selection was removed: binding selStart/selEnd to the
      selection re-highlighted the block mid-frame and corrupted
      positionToRectangle → broken selection rects. Markers stay accent.) Inline code sits in a
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
- [x] **Remove styling** — `Cmd+\` = **full reset to plain paragraph**: strips
      inline spans AND resets heading/quote/list → paragraph (`setHeading(0)` +
      `clearFormat`), acting on the caret's block (no selection needed); code
      blocks left as-is. `Cmd+B`/`Cmd+I` decide add-vs-remove **uniformly** across
      a multi-block selection, grouped as one undo step (`beginGroup`/`endGroup`,
      `hasFormat`/`setFormat`). A no-op group (e.g. clear on already-plain text)
      pushes no undo entry (`endTxn` before==after guard).

- [x] **Chassis + left rail** — adopted the family flat-button chassis from `ufb`:
      `FlatButton.qml` + `Icon.qml` (Phosphor icon font via `PhosphorIcons.js`),
      Theme `FontLoader`s + icon/color tokens. Bundled fonts (`MinNotes/App/fonts/`):
      **Inter** (text), **JetBrains Mono** (mono), **Phosphor** (4 weights). Editor
      text → Inter, mono → JetBrains (incl. inline-code spans via the highlighter's
      `codeFontFamily`). **`LeftRail.qml`** — vertical action rail (undo/redo +
      bold/italic/code/clear), wired to the editor's actions; the model-owned
      cursor means a rail click keeps the selection, focus returns to the editor.
      Buttons enable off `canUndo`/`canRedo`/`hasSelection`. Section-ready for
      collapsible groups later. `Main.qml` is now `Row { LeftRail; Editor }`.
- [x] **Family tooltip + headings** — `FlatToolTip.qml` (squared, opaque `#0e0e0e`,
      hairline border, Inter small) adopted from QCView/MinRender, baked into
      `FlatButton` and placed to the right (rail hugs the left edge). Rail now has
      an **H1–H5** section: `BlockModel.setHeading(row, level)` (0 = paragraph,
      undoable); editor `setHeading(level)` toggles the active level off and groups
      a multi-block selection; buttons show **checked** when the caret's block is
      that heading (`caretType`/`caretLevel`). No selection needed — acts on the
      caret block.
- [x] **Word-style active typing attributes** — Bold/Italic/Code with **no
      selection** become a **toggle** (arm the attribute); the next characters you
      type get the span, and it stays armed across the word until you toggle off
      or move the caret. The armed marks are applied *inside* `insertText`
      (`marks` bitfield) so a run of armed typing still coalesces into one undo
      step. Cleared on arrow/click; the rail B/I/code buttons light while armed.
- [x] **Fonts: full faces + serif quotes** — bundled **Inter Bold-Italic** so a
      bold+italic run renders a real face (Qt won't synthesise the combo when
      separate bold/italic faces exist). **Quote blocks** now use **Merriweather**
      (serif, 24 pt optical), upright by default (the serif + bar + muted colour
      mark them) with all four faces (regular/bold/italic/bold-italic) bundled so
      bold/italic spans inside a quote render real faces. `Theme.font.serif`.

- [x] **Block drag-reorder** — hover a block → a `dots-six-vertical` grip in the
      left gutter; press+drag it to reorder. `BlockModel.moveBlock(from, to)` is a
      pure **fractional-rank rewrite** (`rankBetween` + `Document.updateRank`), no
      renumbering, O(log n) + one DB write, undoable (region-snapshot txn;
      `applySnapshot` now restores rank too). Drag is owned by the **persistent
      content-level `mouse` MouseArea** (not a recycled cell), so it survives
      scroll/recycle and edge **auto-scroll** with no replica needed; a floating
      ghost + accent drop-line follow the cursor; on release → `moveBlock`. Verified
      headless: move/undo/redo correct.

- [x] **Formatting surface rounded out** — **strikethrough** + **underline** added
      as span kinds (`SpanStrike`/`SpanUnderline`, render via `setFontStrikeOut`/
      `setFontUnderline`, compose with the rest); rail buttons + `⌘⇧X`/`⌘U`; they
      inherit undo + armed-typing. Rail **block-type** buttons: **quote**/**bullet
      list** (toggle the caret block via `setBlockType`) and **divider**
      (`insertDivider`). Rail sections (**Headings**, **Blocks**) are now
      **collapsible** behind a chevron header — proves the rail-grows-down design.
- [x] **Bottom status rail** — `BottomRail.qml`: document **file name + path** on the
      left, **undo/redo** flush right (moved off the left rail to free room).
      `BlockModel.documentPath` exposes the open doc; `Main.qml` is now
      `Row { LeftRail; Column { Editor; BottomRail } }`. `FlatButton` gained a
      `tooltipSide` ("right"|"top") so the bottom buttons' tooltips point up.
- [x] **Code blocks + syntax highlighting** — a real `code` block type: create via
      a ` ```lang ` fence + Enter (`makeCodeBlockIfFence`) or the rail Code-block
      toggle (`makeCodeBlock`); Enter inserts a newline inside, an empty trailing
      line exits to a paragraph. Language in `attrs` (`lang`), round-trips, undoable.
      **Syntax colouring** via **KSyntaxHighlighting** (KDE) — a `QSyntaxHighlighter`
      (`CodeHighlighter`, same mechanism as the inline one), dark theme, the block
      fill matches the theme. A block's document attaches to ONE highlighter by type
      (inline-md for text, code for code). The library is **built from source** into
      `external/kf6` (gitignored) by `external/build-ksyntax.sh`; root CMake
      `find_package`s it. Code blocks get double vertical padding. Fence lines are
      kept literal (inline-md + `commitMarkdown` skip a leading ` ``` `) so typing a
      fence no longer flashes/clobbers into inline code. Language resolution is
      lenient (`js`/`bash`/`javascript`/`c++`… → the right definition).
- [x] **Block context menu + forward delete** — right-click any block →
      a themed `Popup` menu (Add block above/below, Duplicate, Make code block /
      Change language…, Delete; last block clears instead of vanishing). Backed by
      `setCodeLanguage`, `duplicateBlock`, and the existing insert/remove — all
      undoable. "Change language…" opens a language picker (typed `TextInput` +
      common quick-picks). `Del` now deletes **forward** (`forwardDelete`) instead of
      mirroring Backspace.
- [x] **Tables (P0–P7)** — a self-contained table tier docked as one block.
      Tables are one block whose 2D
      grid serializes to compact JSON in the block's `content` (so undo/redo +
      persistence reuse the existing chokepoint). **Data tier**: `core/TableGrid`
      — pure grid value-type (JSON round-trip, structure ops, TSV/CSV import +
      TSV/HTML export, the future paste seam). **Dock**: `BlockType::Table = 7`
      threaded through the type/height plumbing; `BlockModel` table seam
      (`insertTable` + cached `tableRows/Columns/Cell/ColWidth/…` queries +
      `tableSetCell/Insert…/Delete…/PasteTSV` mutators, per-cell typing coalesced).
      **Layout**: after exploring wider/centered/full-width options, the doc kept a
      single **760** reading measure for ALL blocks (tables included), left-aligned
      at a shared edge; a wide table **scrolls horizontally inside its block** with a
      **right-edge border** cueing the clip (hidden once scrolled to the end). The
      wide-table-ergonomics question is parked for a planned **bottom tab system**
      (Document + one full-frame tab per table). **Render (P2)**: `BlockTable.qml` —
      passive grid of read-only cells driven by the query seam, header-row styling,
      per-column alignment, horizontal scroll via a **root-overlay scrollbar** (an
      inner one sits under the document mouse layer). Row height is computed into a
      plain number per row (the `Row` positioner's implicitHeight would loop).
      **Keyboard (P3)**: a `tcur` table sub-cursor (active while the caret is on a
      table) — type/backspace/delete in a cell, arrows move within/across cells,
      Up/Down past the edge exits the table, Tab/Shift-Tab move cells (Tab past the
      last adds a row, lands select-all), Enter → cell below (Shift-Enter = newline
      in cell), Esc collapses/exits; in-cell caret + selection overlays.
      **Mouse (P4)**: routed through the document's central mouse layer (delegates
      can't own a MouseArea) — `BlockTable.cellAtPoint` hit-tests a click → click
      to place the cell caret, drag within a cell for text selection, drag across
      cells for a rectangular cell-range highlight (`tcur` range state).
      **Structure + entry points (P5)**: a LeftRail **Table** button + block-menu
      "Insert table below"; Notion-style **+row/+column** strips on the focused
      table (root overlays — a button inside the table can't receive clicks under
      the mouse layer); right-click a cell → insert/delete row & column, align
      left/center/right, toggle header row.
      **Column widths (P6)**: auto-size from content (live `TextMetrics`, capped
      ~360px so long columns wrap not scroll) — only MANUAL widths persist (stored
      >0 = pinned), so auto never pollutes undo; drag a column border to resize
      (live preview, one undo step), double-click a border resets to auto;
      single-hairline grid (cells draw right+bottom, frame draws top+left).
      **Clipboard (P7)**: a new `core/Clipboard` service (`QClipboard`/`QMimeData`,
      exposed like `blockModel`) — the app's first clipboard. Cmd-C/X/V in tables
      copy a cell range as TSV + HTML `<table>` (round-trips into Excel/Sheets/Docs)
      and paste a spreadsheet range from the TSV plain-text form, growing the grid;
      Cmd-C/V/X + a "Copy" menu item also work for normal text/blocks. Image read
      is shaped but unwired (future media paste).

- [x] **Table tabs** — a bottom tab strip (`TableTabs.qml`, above BottomRail, shown
      only when a table exists): **Document** + one tab per table in appearance
      order, keyed by block id (`tableBlockIds`/`rowForId`/`idForRow`) so the active
      tab follows its table across reorders while label/position derive live ("Table
      N"). A table tab shows it **full-frame** (full editor width + height, vertical
      Flickable + inner horizontal scroll) — reusing the SAME edit model: the cursor
      is pinned to that table and the press/drag/resize logic is shared via
      `beginTableInteraction`/`updateTableInteraction`/`endTableInteraction` (the doc
      central handler and the full-frame MouseArea both call them). Right-click a
      table → "Open in tab"; deleting the active table falls back to Document. The
      full-frame view has its own +row/+column buttons and a grabbable horizontal
      scrollbar (direct children — nothing stacks above it there), so a table tab is
      a complete editor at full width. The dataset+full-frame split is the
      foundation for a more spreadsheet-like table grid later.

- [x] **Smart paste IN (full pass)** — one `doPaste` router over the clipboard:
      plain/markdown text → multi-block (`pasteText`), TSV/CSV → a new table block,
      rich **HTML** (Word/Docs/Excel/web) walked via `QTextDocument`'s frame tree →
      headings/lists/paragraphs **with inline spans**, tables, and images (data:/
      local embeds imported to the `.minnotes` sidecar; remote `http(s)` downloaded
      async via `QNetworkAccessManager` → sidecar swap by block id). Copied files
      (Preview "Copy") routed through `Clipboard.readUrls` like a drop; unsupported
      files become **attachment chips** (Media `kind:"file"` — icon + name + path,
      "Open in ufb" / "Reveal in Finder"). **Links**: `SpanLink` (+payload `href`)
      with a hover pill + context-menu "Open URL" + a LeftRail link button / Cmd-K
      editor; payload spans (`link`/`fgcolor`/`highlight`) push whole, never merge.

- [x] **PDF (Qt PDF)** — `Qt6::Pdf` probes page count / size; inline shows ONE page
      with a nav strip (`image://pdfpage` cached provider survives delegate recycle),
      plus a full-scroll **tab** (`PdfMultiPageView`-style `ListView` of `PdfPageImage`).
      Qt PDF is an OPTIONAL Qt component (installed via Maintenance Tool).

- [x] **Media polish** — **image resizing**: bottom-right drag handle (proportional,
      ghost-preview during drag → commit on release so the doc doesn't reflow mid-drag)
      + top-right fit-to-760; per-block display-width override (`dw`). Video audio
      fixed (`AudioPlayer::initialize()` before `open()`). PDF/video toolbars hide when
      a table/PDF tab is active.

- [x] **Text colour + highlight (right rail)** — a new **`RightRail.qml`** inspector:
      an "A" text-colour tool + a highlighter tool (each APPLIES its current colour to
      the selection) + a palette toggle that expands an **HSV `ColorPickerInline`**
      (ported from QCView) with Text/Highlight target tabs, Hex + R/G/B mono fields,
      and a soft-grey "Revert to default". Spans `SpanFgColor=7` / `SpanHighlight=8`
      carry a hex payload; `InlineMarkdownHighlighter` renders per-char fg/bg.

- [x] **Tables — cell/row/column colours (TC-1)** — `TableGrid` `Cell` extended to
      `{text, bg, fg, spans, media}` (+ per-row/col colour vectors), JSON bumped once
      for the whole rich-content arc (cell = bare string when plain, else object;
      `rbg/rfg/cbg/cfg` arrays). `BlockModel` seam: `tableSetCellColor` (range),
      `tableSetRowColor`/`tableSetColColor`, and `tableCellBg/Fg` **cascade** readers
      (cell → row → column). When a table tab is active the right rail's colour tools
      route to the focused cell / selected cell-range instead of inline text;
      `BlockTable.qml` renders the cascade (selection/header still win). **Next:
      TC-2** rich text in cells (spans), then **TC-3** images in cells.

## Next (rough order)

> **Resuming (2026-06-07):** tables + code blocks shipped; **media is the next
> major arc** — see the approved plan `~/.claude/plans/virtual-tinkering-island.md`
> (M1 image foundation → M2 video → M3 annotation tier → M4 studio → M5 QCView
> interop; deep-dived ufb's image/video pipeline + QCView's annotation engine).
> **M1 DONE:** inline images (Qt-native formats) via `MediaBlock.qml` (async Image)
> + `core/MediaStore` (`.minnotes/<sha>` content-hashed storage for pasted bytes,
> `file://` refs for files — never bytes in the DB; intrinsic `{w,h}` in the media
> block's `content` JSON, tables-shaped → undo-free, jump-free layout). Acquisition:
> drag-drop (gap-snapped, animated insertion-line + ring indicator) + paste
> (`Clipboard.hasImage`). Hardened the caret on opaque media/divider blocks (typing/
> Enter/backspace/forward-delete no longer edit their JSON). **Next: M2** (port ufb's
> full video engine + FFmpeg). Before any new block type, re-read the reactivity rules.

- [ ] **Images / video — THE NEXT MAJOR ARC.** Block type `Media=3` already exists
      (placeholder rect) and is already treated as **opaque** (atomic for cross-block
      text ops). Adopt ufb's media pipeline: `QQuickRhiItem` video surface + FFmpeg,
      async image providers + thumb cache; QCView-Player's header-only FFmpeg probe
      for intrinsic dims; `file://` refs in `attrs` (never ingested), intrinsic
      `{w,h}` in `attrs` for jump-free layout (DESIGN §7). Media height must feed the
      Fenwick index (SPIKE conclusion 4). A media *gallery* could reuse the
      table tab/full-frame pattern. **This will hit the same virtualization
      reactivity rules — see below.**
- [ ] **Editor reactivity rules (MUST respect for new block types).** (1) Every
      per-row QML binding reading a `Q_INVOKABLE` (`typeForRow`/`contentForRow`/…)
      must carry the right revision dep: `(blockModel.layoutRevision,
      blockModel.contentRevision, …)`. (2) Any model mutation that changes the
      row→content MAPPING (`insertBlock`/`removeBlock`/`moveBlock`) must bump
      `contentRevision`, not just `layoutRevision` (else recycled delegates render
      stale content — the table-JSON-as-text bug). (3) Opaque blocks (table/media/
      divider) are atomic for cross-block text merges (see `cursor.opaque()` +
      `deleteSelection` snapping). (4) The central `mouse` MouseArea stacks above
      every delegate → interactive affordances must be root overlays or route via the
      central handler. (5) Tab/full-frame view = swap render+hit-test, pin the cursor.
- [ ] **Table follow-ups (small):** full-frame tab right-click menu; insertion-line
      affordance for table "insert" items (currently highlights the anchor row/col);
      BottomRail `R×C` + selection in a table tab.
- [ ] **Spans — finish past MVP**: a toolbar to drive `toggleFormat`; active-format-
      at-caret; source-mode toggle; markdown typing (`**x**`) → span + consume markers;
      tighten span bookkeeping on cross-block merge.
- [ ] **Inline markdown polish** — underscore emphasis (`_x_`, off to avoid
      `file_name_here`), `\*` escape, nesting (`***x***`), links.
- [ ] **Export to markdown** — the rule table *reverse* (copy-as-md / file export);
      the "one rule table drives autoformat + export" payoff.
- [ ] **Housekeeping:** ~24 commits on `main` unpushed (`git fetch` then push);
      lazy windowed content fetch; per-document open/new-file flow; FTS5 search.
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
