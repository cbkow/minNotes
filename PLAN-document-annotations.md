# Plan: Full-document annotations (free ink + living comments)

> Status: **design-ratified, not started.** Worked out in a design chat 2026-06-08.
> Sequencing: this is the **natural next step after the media-annotation arc lands**
> — specifically after QCView annotations are ported into the video player (the
> shelved media plan M3 "annotation data tier / stroke engine" + M5 "QCView import").
> That arc puts the QCView stroke engine (`ActiveStroke`, serializer, ink-stroke-
> modeler) into the codebase; Layer 2 here reuses it, retargeted from "over an image"
> to "over the whole page." Media-annotation plan: `~/.claude/plans/virtual-tinkering-island.md`.

## The core idea

Two layers, two coordinate models — **not a compromise, the natural seam.** Free ink
and review-comments are different beasts and want different anchoring:

- **Layer 1 — living-doc structured annotations (comments / highlights).** Intent is
  explicit *by construction* (you selected a range / clicked an anchor), so they
  anchor cleanly and **reflow for free** off the existing span machinery.
- **Layer 2 — free ink / markup canvas.** Free ink carries **no parseable intent** —
  the system can't "suss out" what a margin scribble or a multi-block stroke *means*,
  so it must NOT try to content-anchor it. It lives on a positional canvas, decoupled
  from blocks, and drift-on-edit is handled by manual move in annotation mode.

Industry sanity-check: Docs/Notion/Word do structured comments on living text and
deliberately *not* free ink over reflowing text; GoodNotes/PDF do free ink on frozen
pages. This split mirrors that.

## Coordinate model (the crux)

**Key realization:** the document only reflows on **one axis**. The page is a fixed
760-wide column, centered. It never reflows horizontally, so **X is stable**; only Y
flows. That collapses "2D ink over a virtualized reflowing doc" into "1D Y-handling,"
which the Fenwick index already answers.

- **Free ink point = `(Δx from PAGE-center, contentY from doc top)`.** Decoupled from
  blocks: margins fall out for free (Δx past ±380), multi-block strokes fall out for
  free (a polyline owns nothing). Render = one `QQuickPaintedItem` over the content,
  `x = pageCenter + Δx`, `y = contentY − flick.contentY` — same overlay pattern as the
  caret layer, independent of recycled delegates (virtualization-safe).
- Store Δx relative to the **page center** (not window center) so annotations ride the
  centered 760 page when the inspector opens / the window resizes.
- **Positional, not content-anchored** → ink drifts when you edit text above it. That's
  accepted; the escape hatch is **manual move in annotation mode** (legit because
  annotation is a deliberate mode, not babysat while writing).
- Optional later dial: anchor only the *Y* to the nearest row for rigid vertical follow
  (X stays center-relative) — same data model, one extra field.

**Load-bearing invariant:** the page stays **760, never narrower**, so the horizontal
frame is stable.

## Window behavior (lock-vs-fade, resolved)

Be **flexible by default, rigid only while drawing.**

- **Viewing / writing:** fully responsive. If the 760 frame doesn't fit the *column*
  (note: rails + inspector affect this, not raw window width), annotations **fade out**
  (data preserved) with a small **"N hidden — widen to show"** pill so they're never
  silently forgotten.
- **Annotation mode:** ASSERT the frame, but **don't seize the OS window**. Mechanism =
  lock page to 760 + **pan horizontally** to reach margins (optional *soft* min-width to
  prevent squishing mid-draw; avoid force-resizing — it fights tiling / split-screen).
- In annotation mode the **Inspector FLOATS** (overlay) instead of pushing the column,
  so reaching for the color picker can't push the column under 760 and fade the layer
  you're drawing on. (This is the inspector float-vs-push fork, decided in favour of
  float *only in annotation mode*.)

## Milestones (each ends buildable/committable)

### DA-1 — Free-ink stroke model + render (self-contained core)
- Data tier: stroke = tool (freehand/rect/oval/arrow/line) + rgba + width + points in
  `(Δx-from-page-center, contentY)`. Persist in SQLite (a `doc_annotations` table, or a
  doc-level blob), edited through the **`beginTxn/endTxn` chokepoint** so undo/redo are
  free. Reuse QCView `ActiveStroke` + `AnnotationSerializer` JSON shape (ported in the
  media arc).
- Render: `app/qml/.../InkOverlay.qml` (`QQuickPaintedItem`/QPainter) over the document
  content, mapping content→viewport; draws over text + media + margins; bumps with
  `contentRevision`/`layoutRevision`. No per-delegate coupling.

### DA-2 — Annotation mode + drawing interaction
- A mode toggle (left rail / inspector). Enter → inspector floats, page locks to 760,
  horizontal pan enabled, soft min-width.
- Central mouse layer flips to drawing: pointer → screen→content coords → ink-stroke-
  modeler smoothing → `addStroke` (txn). Eraser = AABB hit-test → `eraseStrokeAt`.
  Tool / color / width live in the floating inspector (an annotation toolbar — the
  inspector's second "full interface" use, after colours).
- **Manual move:** select/lasso a stroke, drag to reposition (rewrites its anchor delta).

### DA-3 — Fade / frame-assertion polish
- Flexible + fade-when-frame-doesn't-fit (the "N hidden" pill) for display.
- Lock-760 + pan + soft-min assertion scoped to annotation mode only. No permanent
  min-width tax on the app.

### DA-4 — Living-doc comments (fast-follow; nearly free off spans)
- New span kind `SpanComment` (payload = thread id) → rides `shiftSpansInsert/Delete`,
  reflows for free. A `comments` table holds thread bodies.
- Render: range highlight + margin pin (Y from Fenwick `yForRow`).
- Thread UI in the inspector (comments panel): add / reply / resolve / delete; click a
  pin → open its thread.

### DA-5 — Persistence + export/interop (last)
- Round-trip both layers through the SQLite doc.
- Export the ink layer; optional **"freeze to pages and mark up"** variant reusing the
  **Qt PDF page-render stack** (paginate → ink in pure page space, zero anchoring) as a
  parallel "review a version" mode.
- Comments export (markdown footnotes / sidecar).

## Reuse / seams
- **From the media arc:** QCView stroke engine (`ActiveStroke`, `AnnotationSerializer`),
  ink-stroke-modeler, the stroke render formula.
- **minNotes:** the txn/undo chokepoint, the span system (comments), the Fenwick height
  path (`yForRow`), the central-overlay rendering pattern (respect the editor
  reactivity rules), the sliding Inspector (toolbar + comment threads), the Qt PDF
  page-render stack (for the optional freeze-to-pages export).

## Scope guardrails
- **In:** the milestones above. **Deferred:** Y-stickiness dial (DA-1 stays pure
  positional), the true "freeze to pages" markup variant (DA-5 optional), collaborative
  / multi-user comments, per-stroke pressure/tilt. Keep annotation a deliberate **mode**,
  not an always-on layer that fights writing.

## v1 suggestion
Ship **DA-1 → DA-2 → DA-3** first (the free-ink canvas — the self-contained, exciting
part), then **DA-4 comments** as the fast-follow. DA-5 last.
