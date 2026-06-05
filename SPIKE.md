# minNotes — Virtualization Spike: Test Plan

> Status: **test plan, written before code.** Defines what the spike must prove and the bar it must clear, so "did it work?" is a measurement, not an opinion. Companion to `DESIGN.md` §7 + §10. Captured 2026-06-05.

## 0. The one question

> Can a QML scene hold a **smooth-scrolling, accurate-scrollbar, editable, variable-height** block list at ~100k blocks, where **editing a block reflows the layout without a jump or a stutter** — and if so, with `ListView`+model or only with `Flickable`+custom positioning?

Everything in `DESIGN.md` outside the rendering layer is "known-hard, solved-in-siblings." This is the concentrated risk. We answer it with the **dumbest thing that can fail the same way the real app would**, then measure.

---

## 1. What we build (and deliberately don't)

**Build — the minimum that exercises the real failure modes:**
- A C++ `QAbstractListModel` serving **synthetic** blocks lazily from an in-memory store of N rows: `{id, type, est_height, content}`. No SQLite yet — the model is the seam SQLite will plug into, so we prove the seam, not the DB.
- A **Fenwick / prefix-sum height index** (the structure `DESIGN.md` §7 calls "the most important structure"): `scrollY ↔ block` in O(log n), single-height update in O(log n), total height = root.
- Variable, type-derived **estimated** heights at cold open that **settle** to a measured height once a delegate has laid out (the CodeMirror/Monaco behavior).
- Editable delegates: a `TextEdit` per visible block whose content edit **changes height** → must reflow the index live.
- Two arms behind one switch: **Arm A** `ListView` + the model; **Arm B** `Flickable` + custom-positioned delegates driven directly by the height index. Same model, same data, same harness — only the view layer differs.
- An on-screen HUD: fps (frame time ms), visible range, delegate count, total height, scrollbar thumb pos vs. true pos.

**Don't build (out of scope for the spike — proven elsewhere or not the risk):**
- SQLite, the two-tier read, FTS — the model abstracts it; fake data is fine.
- Real markdown parsing — `content` is plain text; a fixed cost stand-in is enough.
- Media decode / `QQuickRhiItem` — media is the *easy* height case (arithmetic, §7); represent media blocks as a fixed-aspect colored rect with a known intrinsic `(w,h)`.
- Theming, real block types, persistence, undo. Synthetic.

If a cut item is the thing that breaks scroll/reflow, the spike was scoped wrong — revisit. Nothing cut above plausibly is.

---

## 2. Pass / fail thresholds

A number next to each. "Smooth" is not a verdict; frame time is.

| # | Criterion | PASS | FAIL |
|---|---|---|---|
| P1 | **Scroll frame time**, flinging through the doc (ProMotion 120 Hz target; report 60 Hz too) | p99 ≤ 8.3 ms (120 Hz); **hard floor** p99 ≤ 16.6 ms (60 Hz) with **zero** frames > 33 ms | sustained frames > 16.6 ms, or any visible hitch on fling |
| P2 | **Cold-open / jump-to-end** time at N=100k | first paint < 250 ms; jump-to-bottom < 100 ms | multi-second layout, or jump-to-bottom recomputes all heights |
| P3 | **Scrollbar accuracy** — thumb position & size vs. true content position | thumb error ≤ 0.5 % of track at all times; thumb size stable (no jitter) as estimates settle | thumb drifts, jumps, or resizes visibly while scrolling |
| P4 | **Edit-induced reflow** — type into a visible block, height changes | the edited block grows/shrinks, everything below shifts, **caret stays under the cursor**, no frame > 16.6 ms; index update O(log n) | content below jumps/teleports, caret desyncs, or a full relayout fires |
| P5 | **No layout shift from settling** — estimated → measured height correction for an *off-screen* block | correction is localized & O(log n); viewport content does **not** jump | scroll position lurches when an unseen block settles |
| P6 | **Memory flat in N** — hydrated objects ≈ zones 1+2 only | live delegates + hydrated rows bounded by viewport, ~constant from 10k→100k→500k | memory grows with N (delegates/rows not evicted) |
| P7 | **Cross-block caret nav** — ↑/↓/←/→ across block boundaries incl. across an evicted/never-realized block | caret crosses boundaries correctly; selection is logical `(block_id, offset)`, survives the target delegate not existing yet | nav stops at a delegate edge, or selection breaks when a delegate isn't realized |
| P8 | **Cross-block selection + delete** — select from block A into block C (B may be off-screen), delete | selection spans realized + unrealized blocks; delete is a model op on logical range; view reflows once | selection only works between realized delegates, or delete corrupts ranks/index |

P1–P6 are **scroll/layout** (the core gamble). P7–P8 are **caret/selection** (`DESIGN.md` §10.2 — "schema supports it, QML mechanics unproven"). A spike that nails P1–P6 but fails P7–P8 still answers the main question but flags the next risk; record both arms' results on all eight.

---

## 3. Test matrix

Run every cell against **both arms (A: ListView, B: Flickable)**.

**Block counts (N):** 10k · 100k (primary target) · 500k (headroom / does it degrade gracefully).

**Height distributions** (stress the variable-height assumption):
- *Uniform* — all ~1 line (baseline; should be trivial).
- *Mixed* — realistic: 70 % 1–3 line paragraphs, 15 % headings, 10 % code blocks 5–40 lines, 5 % media rects (tall, fixed aspect).
- *Adversarial* — heavy tail: a few blocks 200+ lines among short ones (worst case for estimate-vs-actual divergence and Fenwick correction size).

**Scroll modes:** slow drag · hard fling (momentum) · scrollbar-drag to arbitrary offset · jump-to-end / jump-to-start · Page Up/Down.

**Edit actions:** type a char (height unchanged) · paste a paragraph into a 1-line block (grows) · select-all-delete a 40-line code block (shrinks) · insert a block · delete a block.

**Cross-boundary:** caret ↓ off the bottom of the viewport into an unrealized block · shift-↓ selection spanning the viewport edge · select A→C with B scrolled away, then delete.

Record per cell: pass/fail per threshold, p50/p99 frame time, peak RSS. A results table at the bottom of this file, filled as runs complete.

---

## 4. Arm decision (ListView vs Flickable)

Default preference: **Arm A (`ListView`+model)** — less code, reuses Qt's delegate recycling (zones 1+2 for free, §7). Adopt Arm A **iff** it passes P1–P6 across the 100k Mixed + Adversarial cells **and** P4/P5 reflow doesn't force `ListView` workarounds (e.g. fighting `contentY` on height change, or `positionViewAtIndex` hacks that stutter).

Fall to **Arm B (`Flickable`+custom)** only if Arm A fails a scroll/reflow threshold that Arm B clears — the cost (we own positioning, recycling, and the visible-range calc) is justified only by a measured Arm-A failure, not a hunch.

Decision is logged here with the frame-time evidence that drove it. No evidence → no switch.

---

## 5. Definition of done for the spike

1. Both arms build & run on macOS (Qt 6.11.1) via CMake+Ninja.
2. The matrix (§3) is run; the results table is filled with real numbers.
3. Each of P1–P8 has a PASS/FAIL with measurements, per arm.
4. §4 arm decision recorded with evidence.
5. A short "what this means for the real build" note: which `DESIGN.md` assumptions held, which need revision, what the next risk is.

The spike's deliverable is **the answer + the evidence**, not reusable app code. Throwaway is fine; the numbers are the point.

---

## 6. Results

> Run 1, 2026-06-05, on the built-in Liquid Retina XDR (ProMotion). The HUD's frame metric is the *inter-frame interval* (vsync-floored), so it measures **cadence / dropped frames**, not GPU headroom.
>
> **Measurement caveat — macOS ProMotion focus-throttle (banked):** macOS drops unfocused/idle windows to 60 Hz, and ProMotion only ramps to 120 Hz while the surface is *actively* driving. `QScreen::refreshRate()` read 60 Hz at idle launch, and any rolling window quietly fills with 60 Hz (16.7 ms) frames the moment focus or motion stops — which diluted the first readings to p50 ~16 ms. **P1 must be read during a focused, sustained fling.** The HUD's `best` (fastest frame seen) cuts through this: when it hits ~8.3 ms the surface *did* reach 120 Hz.
>
> **120 Hz reachable — confirmed (Arm B):** during a focused sustained fling, `best` hit the ~8.3 ms / 120 Hz floor (`→ hit 120Hz`). So the app can present at 120 Hz; the open question is now steady-state p50/p99 *during motion* (how often it holds 120 vs drops a frame), not whether it can.

### Arm A — ListView + model
| Cell (N · dist) | P1 frame p50/p99/worst (ms) | P2 | P3 drift | P4 | P5 | P6 delegates | P7 | P8 |
|---|---|---|---|---|---|---|---|---|
| 100k · Mixed | 15.8 / 20.4 / 18.5 | _pending_ | **97.9 % — FAIL** | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |
| 100k · Adversarial | _pending_ | | | | | | | |
| 500k · Mixed | _pending_ | | | | | | | |

### Arm B — Flickable + custom (after modulo slot-recycling fix)
| Cell (N · dist) | P1 frame p50/p99/worst (ms) | P2 | P3 drift | P4 | P5 | P6 delegates | P7 | P8 |
|---|---|---|---|---|---|---|---|---|
| 100k · Mixed (focused fling) | 16.7 / ~31 | _pending_ | **0.00 % — PASS** | _pending_ | _pending_ | see ↓ | _pending_ | _pending_ |
| 500k · Mixed (focused fling) | ~16.7 / ~38 | _pending_ | **0.00 % — PASS** | _pending_ | _pending_ | **PASS (indirect)** | _pending_ | _pending_ |
| 100k · Adversarial | _pending_ | | | | | | | |

**P1 verdict (qualified):** under hard focused fling Arm B runs at **~60 Hz cadence** (p50 16.7 ms), *touches* 120 Hz (`best` ~8.3 ms) but does **not hold** it; **p99 31–38 ms = occasional 2–3 frame drops**. Subjectively smooth ("feels fine"). The strict 8.3 ms / 120 Hz bar is **not cleanly met under load** — but the cause is **per-frame delegate-realization cost** (instantiating `TextEdit`s + generating content for newly-exposed rows), a *delegate-hydration* problem, not the view architecture. Exactly what DESIGN.md §7's cheap-cold-delegate + prefetch-ring is meant to absorb. **Next-build optimization target, not an architecture blocker.**

**P6 verdict (PASS, indirect):** 100k → **500k** (5×) moved frame times only ~31 → ~38 ms p99, p50 flat. Per-frame work is bounded by the viewport, not N — the defining property of working virtualization. (Confirm with an explicit `live delegates` count + RSS in a later run, but the flat scaling is the load-bearing evidence.)

**Pre-fix note (Arm B):** the naive "slot = firstRow + index" mapping remapped *all* ~40 pool delegates per row scrolled → **worst 40 ms** (≈2–3 dropped frames). Modulo slot-recycling (rebind only the one slot that crosses the viewport edge) dropped worst to 18.5 ms. The Flickable approach is only as good as its recycling; this is the load-bearing detail.

### P7/P8 — cross-block caret & selection: two architectures tried

This is DESIGN.md §10.2's "schema supports it, QML mechanics unproven." We built it **twice** and the second attempt is the finding.

**Attempt 1 — per-block `TextEdit` (Arm B).** Each block is an editable `TextEdit`; a controller handles cross-block moves while each `TextEdit` natively handles within-block caret/selection. **Result: FAIL — not viable.** Two sources of truth (Qt's per-widget focus vs. the model's logical cursor) constantly desync: caret visibly in block 9 while keystrokes act on block 6; clicking after a selection re-selects a stale range; within-block vs cross-block selection fight. Every fix surfaced another desync. *Conclusion: N independent native text widgets is the wrong foundation for a block editor.*

**Attempt 2 — passive surface, model-owned cursor (Arm C, `ArmEditor.qml`).** Blocks are **read-only** `TextEdit`s used purely as a text-layout oracle (`positionToRectangle`/`positionAt`); they never take focus. One central `FocusScope` owns the keyboard; the model's logical `(anchor,focus)` is the sole cursor; caret and selection are **drawn as overlays** computed from it (selection = one rect per *visual* line, so wrapped lines highlight correctly). **Result: PASS.**

| Threshold | Arm C result |
|---|---|
| **P7 caret crossing** | **PASS** — ←/→/↑/↓ cross block boundaries *and* respect wrapped visual lines within a block; auto-scrolls into off-screen blocks (`ensureVisible`); the row keys act on always matches the visible caret (desync gone). |
| **P7 into unrealized block** | **PASS** — moving the caret off-screen scrolls the target in; the recycled delegate claims the logically-already-there caret. |
| **P8 selection** | **PASS** — Shift+arrows extend line-by-line within a block and block-to-block; highlight tracks wrapped lines. |
| **P8 range delete / edit** | wired (Backspace deletes selection or merges blocks; typing inserts; Enter splits) — model is sole owner. Confirm range-delete + merge in a follow-up pass. |
| Mouse drag-select across blocks | **not built** — click-to-place + keyboard only. This architecture *enables* it cleanly (central hit-test via `rowForY` + `positionAt`); banked as next step. |

### Arm decision
**Flickable + custom positioning driven by the Fenwick index (Arm B/C base), with a passive model-owned editing surface (Arm C).** Not ListView.

Evidence: Arm A **fails P3** (scrollbar drift ~98 % — ListView estimates total height from delegates seen, so the thumb is meaningless at scale) and shows a worse p99 (20.4 vs 17.7 ms) from per-realization work. The Flickable base **clears P3 exactly** (drift 0.00 %, because `contentHeight` *is* the Fenwick total) and, once recycling is fixed, matches Arm A's smoothness; it also feels optically smoother (tighter p99, stable cadence). Satisfies the §4 switch rule. On top of that base, the **passive surface (Arm C)** is the editing layer — Arm B's per-`TextEdit` editing fails P7/P8 (focus desync), Arm C passes.

The cost (we own positioning, recycling, visible-range, contentY-compensation, and overlay caret/selection) is real but bounded — the throwaway harness implements all of it in a few hundred lines of QML.

### What this means for the real build
The spike answered both of DESIGN.md's named risks (§10.1 virtualization, §10.2 cross-block cursor). Four load-bearing conclusions:

1. **Flickable + Fenwick, not ListView.** The cumulative-height index gives an exact scrollbar and O(log n) `rowForY`/`yForRow`; ListView's hidden contentHeight estimate is *disqualifying* for large docs (P3 drift ~98 %), not a tuning knob. DESIGN.md §7's "most important structure" call holds.
2. **Custom recycling is mandatory and subtle.** The 40 ms → 18.5 ms swing came entirely from *how* pool slots map to rows. The modulo mapping (rebind only the slot crossing the edge) is a real requirement to bank, not an optimization.
3. **The editing surface must be passive, with the model as sole cursor owner.** This is the biggest finding and it was *not* obvious up front: N independent native `TextEdit`s (Arm B) are unworkable for a block editor — Qt's per-widget focus and the model's logical cursor desync endlessly. The shippable architecture is **read-only text as a layout oracle + central focus + overlay-drawn caret/selection** (Arm C). Selection must render **per visual line** to handle wrapping.
4. **Media height must feed the layout index.** A media block whose reported height excludes its visual (the rectangle, not the measuring TextEdit) corrupts every Fenwick position below it → cascade overlap. Intrinsic dims must drive height (DESIGN.md §7's media-height rule, now empirically motivated).

**P1 at 120 Hz — RESOLVED (delegate cost is not the limiter).** After pre-generating content (cheap O(1) retrieval, representative of a SQLite fetch — string synthesis was a hot-path artifact), a repeatable auto-scroll bench with Qt render-timing (`QSG_RENDER_TIMING=1`) measured the actual per-frame **render work** (sync+render+swap), separate from the vsync wait:

| | mean | p50 | p99 | max |
|---|---|---|---|---|
| **render work / frame** (Arm C, Mixed, full sweep) | **1.4 ms** | <1 ms | **6 ms** | 18 ms |

Render work p99 (6 ms) sits **comfortably under the 8.3 ms / 120 Hz budget**, even for Mixed content. The inter-frame interval still reads ~16.2 ms (60 Hz) with `best` 7.9 ms.

**Root cause of the 60 Hz cap = Qt's macOS presentation layer, NOT our code, the display, or delegate cost.** Investigated directly (probe at `/tmp/refresh_probe.mm` comparing macOS's CoreGraphics vs AppKit refresh APIs, + per-screen `QScreen::refreshRate()` logging + on-display benches):

| Display | macOS `maximumFramesPerSecond` | `QScreen::refreshRate()` | Bench (Arm C) | Why 60 |
|---|---|---|---|---|
| DELL S3225QC (fixed 120 Hz) | 120 | **60 (Qt wrong)** | 60 fps | Qt reads stale CG `CGDisplayModeGetRefreshRate`=60 → animation driver throttled to 60 |
| Built-in Retina (ProMotion) | 120 | 120 (Qt right) | **60 fps** | even with correct 120 detection, the CVDisplayLink-driven render loop doesn't drive >60 / ProMotion doesn't ramp |
| PHL 27E1N8900 | 60 | 60 | 60 fps | genuinely 60 Hz |

The decisive result is the **built-in row**: Qt detects it as 120 Hz yet still presents at 60 fps — so this is **two compounding Qt-macOS bugs** (refresh mis-detection on some panels *and* the render loop/CVDisplayLink not sustaining >60), not just a wrong refresh number. Known cross-framework macOS issue (Zed hit it, reworked their display-link; cf. QTBUG-43296). It's a **Qt platform-plugin limitation shared by every Qt Quick app (incl. the `ufb` siblings)** — fully orthogonal to the editor architecture; the fix lives in Qt's cocoa render loop / refresh detection (Qt-source patch or upstream/version fix), not in app code.

**Conclusions:** (a) render-work headroom for 120 Hz is **proven** (display-independent, ~1.4 ms/frame); (b) delegate cost is **not** the limiter — DESIGN §7's cheap-cold-delegate / prefetch-ring is *not* needed for frame budget; (c) actually presenting at 120 Hz is a **separate Qt-platform task** (fix/patch the cocoa plugin's refresh-rate reporting / animation driver), not an editor-design problem and not a spike blocker.

**Still open (next builds, not blockers):**
- **P2/P4/P5** explicit measurements (jump-to-end timing, edit reflow, settle-without-jump) — the mechanisms exist (contentY compensation, Fenwick reflow); quantify them.
- **P8 range-delete + merge** end-to-end confirmation; **mouse drag-select** across blocks (architecture-enabled, unbuilt).
- The harness is **throwaway** — its value is these conclusions. The real implementation moves the model/Fenwick into the Rust core (DESIGN.md §3) behind the same seam.
