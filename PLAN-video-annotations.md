# Plan: Video annotations — QCView-interop studio

> Status: **ratified direction, sequenced.** Captured 2026-06-11 after a deep
> re-read of QCView-Player's annotation subsystem (~3.8k LoC,
> `QCView-Player/src/annotations/`) and minNotes' video/media seams. This arc
> runs BEFORE document management (DM-2+) and before the in-doc annotation
> tiers (PLAN-document-annotations.md), which are a separate phase.

## The decision that shapes everything: full QCView interchange

Video notes do **NOT** live in the minNotes document. They live in QCView's
per-media sidecar — `.qcview/<media_filename>/` next to the original video —
in QCView's exact format, so QCView and minNotes read and write **each
other's** notes. Video annotation is a bespoke aspect of minNotes (review
tooling that travels with the *video*), distinct from the in-document
annotation tiers (sketch blocks / block-pinned ink / comments), which save
into the document and come later.

Consequences:

- **The sidecar layer is a contract, not a design space.** We port QCView's
  `annotation_note`, `annotation_serializer`, `annotation_io`,
  `annotation_manager` essentially verbatim (plain C++/Qt, no QCView
  entanglements). Schema: `notes.json` (`media_file`, `media_type`,
  `created`, notes array) + stroke JSON v2.0 (`coordinate_system:
  "normalized"`, `is_modeled`, shapes: freehand/rect/oval/arrow/line).
- **Timecode is the primary key** — every QCView mutation is keyed by the
  `"HH:MM:SS:FF"` string (incl. drop-frame semicolon convention). Our
  frame→timecode derivation must be copied from QCView **exactly** (not
  re-derived), or the two apps mint duplicate notes for the same frame.
- **We must write the thumbnails too**: `images/note_<TC>.png` (clean frame,
  storage resolution) + `note_<TC>_annotated.png` (QPainter composite).
  QCView's filmstrip expects them; JSON-only notes would render blank there.
  `MediaStore::extractFrame()` (the poster mechanism) is the frame grab;
  the composite is a small port of `annotation_thumbnail`.
- **ink-stroke-modeler comes along** (Google lib; FetchContent in QCView).
  Modeled points are baked into the JSON, so smoothing happens at capture
  time and strokes look identical in both apps. Vendor it the
  [[minnotes-ksyntax-dependency]] way (external/ + build script) or
  FetchContent — decide at port time.
- **Writes are atomic + merge-aware**: QSaveFile (as QCView does), and on
  save we **read-merge-write keyed by timecode** instead of blind overwrite,
  so a QCView save we haven't observed is never clobbered. A
  QFileSystemWatcher on `notes.json` reloads on external change → we see
  QCView's edits live; QCView sees ours on media (re)open (it has no watcher
  — adding one THERE is a separate, optional, later task).
- **Undo stays OUT of the document.** Video notes never touch
  beginTxn/endTxn — they get QCView's in-memory per-stroke annotation undo
  stack. Two undo worlds: ⌘Z routes to the annotation stack while a drawing
  tool is armed / studio focus is on annotation, to the doc stack otherwise.
  Save As / checkpoint logic never thinks about video notes at all.

## What we do NOT port

- **Metal/D3D11 annotation renderers + stroke_tessellator.** Our video is a
  scene-graph item (`VideoSurfaceItem`, QQuickRhiItem), so strokes render in
  a sibling **QQuickPaintedItem** overlay — QPainter polylines with round
  caps/joins are plenty for review strokes. Port only the arrowhead geometry
  math and the eraser hit-test from the tessellator/annotator.
- **ViewportAnnotator's render-thread mutex machinery.** Our overlay paints
  on the GUI thread; the port is a slimmed capture class (push-based
  `onPointerEvent(phase, pos, timestamp)`, screen ↔ frame-normalized [0..1]
  mapping incl. aspect-fit letterboxing, tool dispatch, modeler on
  freehand).
- **QCView's exporters** (MD/HTML/PDF/DOCX, ~1k LoC) — deferred; QCView can
  export the shared sidecar meanwhile. Markdown export may ride the doc's
  P6 export arc later.

## The studio interface

Videos get the full-frame **tab** treatment (the table/PDF playbook —
direct mouse, no central-layer routing):

- **Tabs:** the bottom tab strip (TableTabs.qml) gains video tabs — keyed by
  block id, labeled by filename (the PDF pattern). `activeVideoId` joins
  `activeTableId`/`activePdfId` in the mutually-exclusive set behind
  `activeFrameId`.
- **Studio layout (active video tab):** large video surface (the shared
  `VideoDecoder` + `VideoSurfaceItem`; activating the tab activates that
  row's playback) + transport bar (reuse the existing frame-accurate
  transport) + **notes panel at the bottom** — QCView's filmstrip,
  re-skinned per [[minnotes-style-rules]] (squared, divider-grey selection,
  body 14 / chrome 13):
  - Add-note tile → grab clean frame at current playhead, mint note at
    current timecode.
  - Horizontal card scroll: thumbnail, timecode (mono) + frame number,
    multi-line text (auto-save on unfocus), addressed checkbox, delete.
  - Card click → frame-accurate seek to the note's frame (we have
    `stepVideoFrames`/scrub already).
- **Tools live in the Inspector:** a third target — **Text | Highlight |
  Drawing** — reusing `ColorPickerInline` as the stroke-color editor, plus a
  FlatButton tool grid (freehand / rect / oval / arrow / line / eraser) and
  a width slider (1..24). Color + width persist via QSettings (QCView uses
  `annotation/colorHex`). Arming a tool enters annotation mode in the studio
  (pause playback on stroke start); deselect / Esc exits.
- **Stroke overlay:** QQuickPaintedItem over the studio surface renders the
  current frame's strokes + the live in-progress stroke. Optional later
  nicety: render current-frame strokes over the *inline* video too (a
  "this frame has notes" signal outside the studio).

## Milestones (engine-free first; each live-verifiable)

- **VA-1 — Video tabs + studio chassis.** Video tabs in the strip; studio
  layout (surface + transport + empty notes panel scaffold). No annotation
  code yet. Verify: open video in tab, play/scrub full-frame, doc tab
  round-trip, QML reset on tab switch.
- **VA-2 — Sidecar port + text notes (interop-testable WITHOUT strokes).**
  Port note/serializer/io/manager + exact timecode derivation; filmstrip
  cards live (add note at frame → clean thumbnail PNG + notes.json; edit
  text; addressed; delete; card click seeks). QFileSystemWatcher +
  read-merge-write. **Verify against QCView itself:** notes created in one
  app appear correctly in the other, no duplicate timecode keys.
- **VA-3 — Stroke engine + drawing.** Vendor ink-stroke-modeler; port
  ActiveStroke + slim capture class + serializer's stroke half; Inspector
  Drawing target (tools/color/width); QPainter overlay (live + committed
  strokes); eraser hit-test; annotated-thumbnail composite (debounced);
  in-memory annotation undo with ⌘Z routing. Verify: strokes drawn here
  render identically in QCView and vice versa (`is_modeled` honoured).
- **VA-4 — Polish.** Inline current-frame stroke indicator/overlay; studio
  edge cases (resize, multi-video docs, missing/moved media → sidecar
  missing); only then consider QCView-side watcher + exports.

## Open questions (decide at the milestone, not now)

- Vendor ink-stroke-modeler via external/ build script vs FetchContent.
- Whether the studio transport is the existing per-row toolbar reparented or
  a studio-specific instance bound to the same decoder state.
- `media_type` "audio"/"image" notes: read them fine (contract), but UI is
  video-only for now.
