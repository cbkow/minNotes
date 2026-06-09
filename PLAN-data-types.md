# Plan: Bespoke data types — a minNotes markdown variant

> Status: **exploratory / next-session starting point.** Captured 2026-06-08 from a
> design chat. The conceptual model is settled; milestones below are coarse, not yet
> fully specced. The umbrella idea: minNotes grows its own set of **bespoke inline data
> types with simple inline logic + markdown-style shortcuts** — effectively a
> minNotes-flavored markdown extension. SQLite stays canonical (keeps the rich types);
> markdown export flattens them.

## The core primitive: a "choice value"

The key unification from the chat: **checkbox and dropdown are the same thing.** Both
are a **self-contained multiple-choice value** that carries its own options inline:

```
{ options: [ "Todo", "Doing", "Done" ], selected: 1 }
```

- A **checkbox is the degenerate 2-option case**: `[x]` == `{options:[off, on], selected:1}`.
- They differ only by **arity + chrome**:
  - 2 options, rendered as a box → **checkbox**
  - N options, rendered inline → **radio / segmented**
  - N options, rendered as a popup → **dropdown**
- Because it carries its own options, a choice value needs **no external "field"** — it
  is fully self-contained and can live anywhere.

## Three homes (one primitive, same data shape)

- **Inline** — a **payload span.** Links/colours/highlights are already spans carrying a
  payload (`href`, hex); a choice is just another payload-span kind whose payload is
  `{options, selected}`. Click the chip → popup → update `selected`. **Reuses existing
  span machinery** — not new infrastructure ([[minnotes-inline-formatting]]).
- **Block / list item** — e.g. a task list (below).
- **Table cell** — self-contained per cell.

## Column types = the OPTIONAL optimization (not a requirement)

A table **column type** is purely the "share one option set down a column" version:
define `{Todo, Doing, Done}` once, every row's cell draws from it (DRY for repetition,
rename-an-option-everywhere, consistent per-option colour). It does **not** make choices
possible — inline self-contained options already do that. It's the optimization for
*tabular repetition* only. Lives alongside `colWidths_/colAligns_` in `TableGrid`.

## Block-level task lists (`- [ ]` / `- [x]`) — the markdown-native gap

Flagged because it's **missing from the design doc** and is the most contained, highest-
value piece:

- A **`ListItem` (type 5) variant + a `checked` attr**; a `- [ ] `→task **autoformat
  trigger** mirroring the existing `- `→list rule; a checkbox glyph + click-to-toggle;
  **round-trips cleanly to GFM `- [ ]`**.
- It rides `ListItem` precisely because **done-ness is a property of the *line*, not of a
  *value*** — that's what makes the checkbox a prose construct, while general choices are
  value constructs. (This is the boundary the chat found.)

## Related (structural, not a data type): row/column reorder handles

- Drag handles on row-left / column-top to move rows/columns. **Data op is trivial**
  (`moveRow/moveCol` = vector splices, via `mutateTable` → undoable). Interaction is
  mostly **reuse**: the existing block-drag (ghost + drop-gap), column-resize live
  preview, context-menu `hiScope` row/column highlight, and root-overlay handle
  patterns. Near-term-easy "when you want it" feature; independent of the data-type work.

## Markdown round-trip caveat (the "our own variant" reality)

Choice values / typed cells have **no standard markdown.** On export they **flatten to
their selected value** (checkbox → `[x]`/`[ ]`, dropdown → its text). The **DB keeps the
type; markdown is the lossy export.** This is the conscious trade of inventing a variant
— canonical richness in SQLite, graceful degradation to portable markdown.

## Coarse next-session milestones

- **DT-1 — Block task lists (`- [ ]`).** Smallest, markdown-native, high-value. `ListItem`
  + `checked` attr + autoformat + glyph/toggle + GFM round-trip.
- **DT-2 — Inline choice-value primitive** as a **payload span** (one data shape; render
  as checkbox / radio / dropdown by arity + a chrome hint). Click → popup → set `selected`.
- **DT-3 — Table choice columns** (the shared-options optimization over DT-2).
- **DT-4 — Row/column reorder handles** (structural; can land anytime).

## Reuse / seams
- **Inline choices:** the payload-span system (`SpanLink`-style) — [[minnotes-inline-formatting]].
- **Task lists:** `ListItem` + block `attrs` + the markdown-autoformat rule table.
- **Table choices:** `TableGrid` + `mutateTable` + the per-column-metadata pattern + the
  branchy cell delegate (already branches text vs image).
- **Reorder:** block-drag + column-resize-preview + `hiScope` + root-overlay handles.
- Respect the editor reactivity rules for any inline-widget rendering inside a text run
  (selection/caret interplay) — [[minnotes-editor-reactivity]].

## Open questions
- Shared-vs-inline option storage (and how a table column "promotes" inline cells to a
  shared set).
- Per-option colours / chips.
- Whether to grow beyond *choices* to other typed values (number / date), and whether
  those want a **properties block** (a Notion-page-properties / YAML-frontmatter style
  Nx2 construct — a degenerate table) rather than living inline.
- Inline-widget caret/selection behaviour (treat a choice chip as an atomic inline
  object, like the opaque-block treatment for media).
