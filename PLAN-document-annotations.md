# Plan: Full-document annotations (free ink + living comments)

> Status: **TIER 1 (Sketch blocks) SHIPPED 2026-06-11.** The shared stroke
> engine arrived a tier early — it was ported for the VIDEO annotation arc
> (PLAN-video-annotations.md, also shipped 2026-06-11) and sketch reuses it
> wholesale. Implementation rulings that landed: sketch is a **Media KIND**
> (`kind:"sketch"` — the media machinery did the heavy lifting), transparent
> square 480×480 canvas displayed page-width inline (vector upscale), one
> document-undo step per stroke (no coalescing), full-frame-tab editing with
> the Inspector's Draw target, insert via LeftRail + context menu (opens the
> tab immediately). REMAINING: tier 2 (block-pinned margin ink) + tier 3
> (comments), below.
>
> Earlier: design-ratified, REVISED 2026-06-10 — see "Revision: block-pinned ink +
> sketch blocks" below, which SUPERSEDES the page-space canvas (Layer 2) with a
> block-anchored model and adds a Sketch block type as the new v1. Original
> design chat 2026-06-08. The QCView annotation port (shelved media plan M3
> stroke engine + M5 import) is NOT a separate system: it is the shared stroke
> engine underneath every tier here (now living in `app/notes/`).

## Revision 2026-06-10: block-pinned ink + sketch blocks (supersedes Layer 2)

The page-space canvas's accepted weakness was DRIFT (ink pinned to absolute
content-Y slides off its target when text above is edited). Pinning ink to a
**block** kills that by construction, and a new **Sketch block** covers
deliberate drawing with maximal reuse. Three tiers, one stroke engine:

1. **Sketch blocks — the new v1.** A `Sketch` block type: an opaque block whose
   `content` is stroke JSON — the TABLE playbook end-to-end. Undo/persistence/
   copy ride the existing chokepoints (content JSON via a `mutateSketch`-style
   seam); known-geometry height feeds the Fenwick like media; **inline render is
   passive** (strokes painted by a QQuickPaintedItem in the delegate); **editing
   happens in a full-frame tab** ("Open in tab", direct mouse input — no central-
   mouse-layer gymnastics). Markdown export: rasterize to the sidecar → image
   ref. Sketch blocks are media-shaped (intrinsic canvas size, `dw` display-width
   override) so they inherit image-style resize handles. Needs NONE of the old
   plan's mode machinery (760 assertion / floating inspector / fade pills).
2. **Block-pinned margin ink** (replaces the page-space canvas). A stroke stores
   `(anchor block id, points in the anchor's LOCAL space)`. **Anchor-space rule:**
   - *text blocks*: pixel space — Δx from page center, Δy from block top (text
     doesn't scale; position-stable, rides reorder/edits-above).
   - *media blocks*: **frame-NORMALIZED space** (fractions of the media frame;
     values outside [0,1] = margin overshoot). Resizing the image scales the
     ink with it — an arrow from the margin to an image feature keeps its tip
     ON the feature. This makes "annotate over an image" (shelved M3) a special
     case of this tier, not its own system.
   - **Accepted wrinkle:** ink anchored to media scales WITH the media (a margin
     note beside an image shrinks when the image does — the annotation cluster
     hugs its target, GoodNotes-style). No two-regime strokes; complexity hell.
   - Multi-block strokes anchor to the topmost overlapped block; deleting a
     block deletes its ink (defined semantics page-space never had). Within-
     block re-wrap drift accepted (much smaller than the old whole-doc drift).
   - **Storage open question:** NOT block `attrs` (skinny-scan loads attrs
     eagerly; heavy ink would slow open) — lazy content-side storage or a
     dedicated ink table keyed by block id.
   - The old plan's frame/mode machinery (760 assertion, pan, floating
     inspector, fade + "N hidden" pill) applies ONLY to this tier, when it ships.
3. **Comments** — unchanged from the original Layer 1 (a `SpanComment` span
   kind; nearly free off the span system; Inspector hosts threads).

**Sequencing: Sketch blocks → block-pinned ink → comments** (or comments
anytime — they're independent and cheap). The QCView stroke-engine port
(ActiveStroke, serializer, ink-stroke-modeler) lands with tier 1 and is reused
by tier 2 and the video-frame annotation work (M3–M5) alike. Design the stroke
JSON format ONCE for all consumers (polyline + width/colour/pressure; eraser
semantics TBD).

---

*(Original 2026-06-08 design below — Layer 1 stands; Layer 2's page-space
coordinate model and window machinery are kept for reference and for tier 2's
mode work, but the canvas-decoupled-from-blocks premise is superseded.)*

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
