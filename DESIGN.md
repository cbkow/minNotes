# minNotes — Design Groundwork

> Status: **conversation-stage groundwork**, not a committed spec. Captured 2026-06-04 to start a real plan from. Forks marked _(recommended)_ are leanings, not ratified.

A block-based markdown notes editor in QML. Mostly WYSIWYG, with markdown preserved as an input method and escape hatch. Built to handle very large documents with lots of images and embedded video. macOS first, Windows parity later. A sibling to `MinRender` and `ufb`.

---

## 1. The family — what we reuse

The novel surface area of this app is **narrow**: the block editing + virtualization layer. Almost everything else already exists in the siblings, especially `ufb`:

- **Video playback (inline)** — `ufb`'s `VideoSurfaceItem` (`QQuickRhiItem`), frame decoder, vendored FFmpeg, audio routing, scrubable lightbox. Metal/D3D11 zero-copy.
- **Image handling** — `ufb`'s async `UfbImageProviders` + `ThumbCache` + per-format backends (PSD/EXR/PDFium).
- **Rust core + cxx-qt bridge** — `ufb`'s `core/` + `bindings/` pattern. Home for the markdown parser, document model, persistence.
- **Chassis** — Theme singleton, FlatButton/Icon components, platform title-bar/accent code, single-instance IPC, SQLite, Inno Setup + DMG packaging.
- **Media-item model + probe** — `QCView-Player` (`/Users/chris/Documents/GitHub/QCView-Player`, pure C++ Qt, *not* same family). Reference for: `MediaItem`/`VideoMetadata` struct (`src/project/media_item.h`), the **header-only FFmpeg probe** `FFmpegMetadataExtractor::extract()` (~10–50ms, async, no decode → returns `{w,h,frameRate,duration,sar}`), and the `${PROJECT_DIR}` relative-path token (== our "resolve relative to doc").

**Critical split — take rendering from ufb, NOT QCView.** QCView moved to a native `QWindow` + Metal/D3D11 overlay (right for a full-frame player, best presentation/HDR). That's **wrong for inline-in-a-scrollview media**: a native overlay doesn't live in the QML scene graph → won't clip at the viewport edge, scroll frame-perfectly, z-order between text, or multi-instance. ufb's `QQuickRhiItem` renders *into* the scene graph → scrolls/clips/layers/composes naturally. **Inline media = QQuickRhiItem (ufb). Data model + probe = QCView. Don't cross the streams.**

**Trim the struct.** QCView's `VideoMetadata` is a media-player beast (color primaries, HDR transfer, NCLC, broadcast audio, NLE links). Lift the *pattern*, not the struct — a notes media block needs ~`{w, h, sar, duration, posterPath}` in `attrs`. Out of scope (QCView has, we don't need): image sequences, OCIO/HDR color pipeline, the project-pool model.

**Implication:** media playback/decoding/probing is solved across the siblings; we adopt it and focus effort on the editor.

---

## 2. Principles / the key reframe

1. **Markdown is a serialization format, not a working format.** Great for portable interchange; bad at identity, lazy loading, incremental save, search. So it is **export/import only**, never the live edit buffer.
2. **Media is never ingested.** Images/videos stay on the user's filesystem and are referenced by `file://`-style links, loaded lazily. The user owns filesystem management; we don't get into that game. (Resolve links **relative to the document** as well as absolute — the only way a note + its asset folder travel together.)
3. **The scale problem is not a storage problem.** Once media is externalized and rendering is virtualized, a *text* document is never big in bytes (War & Peace ≈ 3 MB). The two real scale-killers — laying everything out at once, and decoding all media at once — both live in the **QML rendering layer**, not on disk.
4. **Three layers, three tools:**
   | Layer | Owned by |
   |---|---|
   | Block structure (order, depth, type, ids) | SQLite |
   | Inline formatting (`**bold**`, links) | markdown text, stored verbatim in `content` |
   | Media (images, video) | files on disk, referenced by link |

---

## 3. Storage — SQLite canonical

**Not a new file format** — SQLite is the universal, archival-grade substrate (the "effectively JSON" bar, cleared, plus indexing/ACID/FTS we need anyway). The document file **is** a SQLite database. Markdown is export-only.

Why not markdown-canonical-with-an-index: to make it work you'd need a block-boundary index + an open cache + a write-without-full-rewrite strategy = an index + a cache + a pager = you'd be hand-rebuilding SQLite beside a text file. The decisive requirement is **random _write_ + cheap incremental commit by position** — that is literally a database pager, which SQLite already is. Its B-tree pager is the disk side of our ring buffer, for free.

Two-tier read is the payoff for large docs:
- **Eager skinny scan** of layout columns `(rank, depth, type)` — index-only, a few hundred KB even at 200k blocks — seeds the height index and draws an accurate scrollbar.
- **Lazy fat fetch** of `content` as the viewport moves.

A markdown file can't do a skinny scan (you must parse everything to get boundaries/heights). That single property is most of why the database wins.

### Async markdown mirror
Export is a **pure function of the canonical tables** (`walk blocks ORDER BY rank → emit prefix(type, attrs) + content`). Therefore "copy as markdown," file export, the optional background `.md` write-behind mirror, and the per-selection "show source" escape hatch are **all the same function at different scopes**. The mirror is a v2 nicety; two-way sync (re-importing an externally edited `.md`) is a conflict swamp and deliberately out of v1.

---

## 4. Schema (flat model)

```sql
-- ========== CANONICAL (the portable document) ==========
CREATE TABLE blocks (
  id       TEXT PRIMARY KEY,            -- ULID: globally unique, time-sortable
  rank     TEXT NOT NULL,               -- lexicographic fractional key; one global sequence
  depth    INTEGER NOT NULL DEFAULT 0,  -- indentation; "subtree" = contiguous deeper run
  type     TEXT NOT NULL,               -- paragraph|heading|code|quote|list_item|image|video|divider…
  attrs    TEXT,                        -- JSON: {level:2}/{lang}/{checked}/{collapsed}/ media:{src,w,h[,start,end,scale]}
  content  TEXT NOT NULL DEFAULT '',    -- inline-markdown body, marker-free; media link/internal token
  created  INTEGER,
  modified INTEGER
);
CREATE INDEX blocks_order ON blocks(rank, depth, type);  -- covering skinny-scan; already in visible order

CREATE TABLE doc_meta (                 -- single row
  id INTEGER PRIMARY KEY CHECK (id = 1),
  title TEXT, schema_version INTEGER, app_version TEXT,
  created INTEGER, modified INTEGER,
  last_cursor TEXT                       -- {block_id, offset} → reopen exactly where left off
);

CREATE VIRTUAL TABLE blocks_fts USING fts5(content, content='blocks', content_rowid='rowid');
-- + AI/AU/AD triggers keep FTS in sync (external-content, no text duplication)

-- ========== EPHEMERAL (rebuildable, separate ATTACH'd file, never portable) ==========
CREATE TABLE layout_cache (
  block_id     TEXT NOT NULL,
  width_bucket INTEGER NOT NULL,         -- heights are width/font dependent → bucket them
  height       REAL NOT NULL,
  PRIMARY KEY (block_id, width_bucket)
);
```

Pragmas at open: `journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON`, `user_version` driving migrations from day one.

### Column rationale
- **`id` ULID** — permanent, sortable identity for backlinks, reopen-at-cursor, the mirror, any future sync. Integers renumber; unsafe.
- **`rank` lexicographic fractional** — insert/reorder = generate one key between neighbors, write one row, touch zero neighbors (O(1)). **Not** floats (precision dies after ~50 same-spot inserts).
- **`depth` (flat, not a tree)** — markdown is flat-with-indent, so this mirrors the on-disk format exactly. A "subtree" is a contiguous deeper run.
- **`type` authoritative, marker-free** — `content="Heading"`, `type=heading`, `attrs={level:2}`. The `##` never appears in WYSIWYG; reconstructed only on export.
- **`content` inline-markdown verbatim** — this is where "markdown preserved" literally lives, parsed per-block only for visible blocks.
- **`layout_cache` separate & ephemeral** — heights depend on width/font/zoom, so they are kept *out* of the canonical/portable file and rebuilt; width-bucketed.

---

## 5. Flat model — why it wins

A "subtree" is a contiguous run of deeper blocks, so the operations that seem to need a tree all become **contiguous-range ops**:
- **Fold/collapse** (toggle or heading section) = exclude the contiguous deeper run from the visible index. Folding lists and folding heading sections become the *same* operation.
- **Move a nested item with children** = move a contiguous slice, re-rank.
- **Insert/delete** = one `rank` op.

The real gift is to virtualization: **storage order = visible order = Fenwick index order.** One sequence, no tree-walk/flatten step ever. We'd only need a tree for full-Notion nesting (columns, arbitrary containers) — explicitly **out of scope** (the Notion complexity is a trap for a markdown-first editor).

---

## 6. Typing / WYSIWYG model

The "what happens when I type `## `" question and the schema's type-authority are **one decision**. Chosen pairing:

- **Transform-on-trigger _(recommended)_** — typing `## ` is an input shortcut that is *consumed*: markers vanish, `type=heading`, `content="Heading"`. Markers are never stored.
- **Escape hatches:** type-it-then-`Cmd-Z` reverts to literal text; `\## ` escapes; nothing transforms inside code blocks; Backspace at block start demotes heading → paragraph; a **source-mode** toggle (per-block or whole-doc) reveals raw markdown on demand. This is where "ability to use markdown is preserved" is honored without cluttering the default view.

**One rule table.** "`## ` at block start → heading 2" is a single `type ↔ prefix` mapping applied *incrementally* while typing (autoformat) and *in batch* on import — and reversed on export. Autoformat, import, export = one source of truth.

---

## 7. Virtualization — "ring buffer for document blocks"

Mental model the user landed on: a video player's frame ring buffer, but for blocks. Three concentric zones around the viewport (= playhead):
1. **Visible** — fully hydrated: inline markdown parsed, height measured, delegate live, media decoded.
2. **Prefetch ring** — hydrated, media as sized placeholders; biased toward scroll direction (like decode-ahead). Makes scrolling feel instant.
3. **Cold** — not in memory as objects; one cheap index row `{id, rank, depth, type, est_height}`. On disk.

You never hold more than zones 1+2 → flat memory regardless of document size. **Blocks are the fault-in/evict granularity** — what pages are to virtual memory. A monolithic `QTextDocument` can't do this (no addressable sub-units); blocks are the enabling structure.

Where the video analogy **breaks** (and the real work lives):
- **Variable, unknown heights.** Need a **cumulative-height index (Fenwick/prefix-sum tree)**: `scrollY ↔ block` in O(log n); updating one measured height is O(log n); total height (scrollbar) is the root. Most important structure in the design. Cold-open estimates from `type` and **settles** as blocks are measured (CodeMirror/Monaco behavior).
- **Blocks are editable** → it's really a **read-write page cache / DB buffer pool**, not a pure ring buffer: edits change height (reflow the index), dirty blocks must **flush** to SQLite before eviction, and the **cursor/selection can reference evicted blocks** → selection lives in the *model* as logical `(block_id, offset)` ranges, never in delegates. Decide this on day one.
- The media-decode layer *is* a pure ring/LRU (drop freely, memory-budgeted) — this is exactly where ufb's image/video pipeline plugs in.

**QML mapping:** `ListView` is already a ring buffer *for delegates* (free zones 1+2). The open question is whether `ListView` + a C++ `QAbstractListModel` (serving blocks lazily, backed by SQLite + the Fenwick index) holds up at scale with edit-induced reflow, or whether we drop to a `Flickable` + custom-positioned delegates driven directly by the height index. **This is the make-or-break spike.**

### Media height — kill layout shift at the source
Media's height being *unknown until decoded* is the classic scroll/cursor nightmare (item pops to real size on load → everything below jumps). But displayed height is deterministic:

```
displayed_height = clamp( content_width × (intrinsic_h / intrinsic_w),  max_height_policy )
```

The only unknown is the aspect ratio — a file-header property readable **without a full decode** (ufb's pipeline already reads it). So:

- **Probe intrinsic `(w, h)` at drag/import and cache it in `attrs`** (`{src, w, h}` for images; `+{start,end,duration}` for video). Viewer-*independent*, so it's canonical (unlike rendered height, which is width/font-dependent and lives in `layout_cache`).
- A media block's height is then computable in the skinny scan **before any pixel decodes** → Fenwick index correct on first paint, exact box reserved, no jump, stable caret math. Media becomes *more* deterministic than text (text height depends on shaping/wrapping; media is arithmetic).
- **Block setup fixes the _mechanism_** (a late correction is O(log n), localized); **intrinsic dims fix the _cause_** (unknown height). Need both.
- Stored dims are a cache of the asset at import — if the file is swapped, reconcile on decode and reflow that one block (rare).
- Edge cases: missing/un-probed media → reserve a default box (e.g. 16:9 at column width), never `0 → pop`; user-resized → store chosen width/scale, height still from aspect; pathologically tall images → max-height policy, still deterministic.
- **Use *display* aspect, not raw `w/h`:** `display_aspect = (w × sar) / h`. Square-pixel images have `sar=1`, but anamorphic video doesn't — the probe returns SAR for free (QCView tracks `sarNum/sarDen`), so account for it.
- **Poster-by-default, decode-on-play** (the "lots of videos" scale rule): inline video blocks render a **poster frame** (an image, through the normal image ring buffer); a decoder spins up only for the *one* video actually being played (inline `VideoSurfaceItem` or lightbox). Many video blocks cost posters, not decoders. The poster's dims double as the layout-dimension source.
- **Probe split:** images need only a header read for dims (trivial, cheap — can live in the Rust core); video needs the FFmpeg header probe (QCView's `extract()`, shared with ufb's FFmpeg). Both async on import, cached into `attrs`.

---

## 8. Deliberate scope cuts (v1)

- No filesystem/asset management — `file://` links, user-owned.
- No Notion-style arbitrary nesting (columns, infinite containers).
- No two-way markdown mirror sync (export only; optional write-behind mirror later).
- Shallow nesting only (lists + toggles).
- macOS first; Windows parity staged later (ufb's playbook).

---

## 9. Annotations on media (deferred feature — design banked)

A markup layer (freehand strokes / shapes with color + width) drawn over an image or video — like QCView's annotations. Not v1, but the design is fully compatible with the core, and **verified to be a clean port** of QCView's annotation model (`src/annotations/{active_stroke.h, annotation_serializer.cpp, stroke_tessellator.cpp}`).

- **Port directly:** `ActiveStroke` (tool enum: freehand/rect/oval/arrow/line; **normalized `[0..1]` points**; rgba-0..1 color; `strokeWidth` in logical px; `filled`) + the `AnnotationSerializer` JSON shape (`{version, coordinate_system:"normalized", shapes:[…]}`). That JSON *is* our `mn-annot` fence payload. Optional polish: their ink-stroke-modeler smooths freehand (Google lib, 0..1000 model space) — easy to add later.
- **Coordinate space (verified identical to our plan):** strokes stored **normalized `[0..1]`**; render = `media_box.origin + pt × media_box.size` (QCView's `StrokeTessellator::normalizedToScreen` is the same formula). Resolution-independent (survives a higher-res file swap) and portable (just numbers, round-trips through markdown).
  - **Delta vs QCView:** they normalize to the *visible viewport sub-rect* (they pan/zoom media); we always display the **whole** frame, so we anchor to the **full media frame** — no pan/zoom ambiguity, *simpler* than QCView, and robust even if zoom-into-media is added later.
- **Storage vs serialization:** storage = `attrs` JSON on the media block (canonical). Markdown = an adjacent fenced block the exporter emits after the media, e.g. ` ```mn-annot src=…\n{"coordinate_system":"normalized","shapes":[…]}``` `. The importer **folds** a `mn-annot` fence following a media reference back into that block's `attrs` → markdown has two adjacent constructs, the model has one media block (a special case in the `type ↔ markdown` rule table, same fold/unfold machinery as headings). Token is internal; nobody types it.
  - **Delta vs QCView:** they persist to a sidecar `.qcview/<media>/notes.json`; we relocate the same payload into block `attrs`/fence. **Trim** their review-workflow fields — `image_path` (rasterized PNG of the frame), `text`, `addressed` — unless we later want comment-style annotations.
- **Video time anchor — per-frame (the clean port), not a range.** QCView pins all strokes at a frame to one `{timecode, frame, timestamp_seconds}` moment. Adopt per-frame for v1 (pin markup to a moment, jump-to-it like a marker). `t:[start,end]` ranges are a maybe-later, not a v1 commitment. Scale path if a video accrues many: a separate `annotations(block_id, frame, data)` table indexed by `(block_id, frame)`.
- **Orthogonal to virtualization (why it's safe):** the overlay lives *within* the media's box → does **not** change block height → Fenwick index / scroll model untouched, zero new layout complexity. It's a child scene-graph item (`Canvas`/`Shape`) on the media's rendered rect → scrolls/clips with the media for free (same scene-graph win as ufb's `QQuickRhiItem`; another reason QCView's native overlay would be wrong here).
- **Stroke width:** logical px / DPR-aware (constant pen feel), matching QCView — confirmed, not normalized.

## 10. Open questions / next session

1. **The QML virtualization spike — the one genuine unknown.** Build the dumbest possible virtualized list of editable, variable-height blocks; try to break: (a) smooth scroll + accurate scrollbar at ~100k blocks with edit-induced reflow, (b) cross-block selection / arrow-nav across boundaries, (c) `ListView`+model vs `Flickable`+custom-positioning. Define pass/fail before building anything real. Everything else is "known-hard, solved-in-siblings"; this is the project's concentrated risk.
2. **Cross-block selection & cursor mechanics in QML** — schema supports it (logical selection in the model); QML mechanics unproven. Tied to the spike.
3. **The `type ↔ markdown prefix` rule table** — the finite, testable mapping that makes round-trip provably lossless (powers autoformat + import + export).
4. **Edge cases to stress-test the flat + contiguous-range model:** code block containing literal `## ` lines (suppress transform inside fences); folding a heading section containing shallower list items; select-across-collapsed-range then delete.

### Resolved
- **Video syntax** is a non-decision: media is added by drag/import (a button), so it's a backend concern. The on-disk markdown token is just an internal serialization label nobody types. Slots into `type=video` + `attrs={src,start,end}`.
