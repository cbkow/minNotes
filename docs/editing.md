---
title: Editing
permalink: /editing/
nav_order: 4
---

# Editing

A note is a list of **blocks**. Press `Enter` to start a new block,
`Backspace` at the start of an empty block to merge it up. Grab the number to the far right of a block and drag to rearrange blocks.

![Blocks and the block ruler](images/mn007.png)

## Markdown-Style Triggers

Type these at the **start** of a block and it converts as you go:

| Type this | Becomes |
|---|---|
| `# ` … `###### ` | Heading 1–6 |
| `> ` | Quote |
| `- `, `* `, `+ ` | Bullet list item |
| `- [ ] ` / `- [/] ` / `- [x] ` | Task (to-do / doing / done) |
| `1. ` (any number) | Numbered list item |
| ```` ``` ```` or ```` ```lang ```` then `Enter` | Code block (optionally for a language) |
| `---` / `***` / `___` then `Enter` | Divider |

List items nest with `Tab` / `Shift+Tab`.

Every code block carries a small **language chip** in its top-right corner —
click it to pick the syntax highlighting from the full language list (type to
filter). The ```` ```lang ```` fence tag still works if you prefer typing it.

## The Page

The page is a steady reading measure — it never squeezes. A narrow window
scrolls sideways instead of rewrapping your text; a wide table extends past
the page into the margin.

Each note has its **own page width**. The slim ruler above the page shows
the stops (560 up to 1600; 760 is the default) — drag the handle or click a stop; the page
reflows live and one Undo puts it back. Margin ink rides along as the page
widens. Pick a wide page for image boards, the classic measure for prose.
In a window wider than the page, the page sits centred with its margins;
a table wider than the page centres under it.

**Zoom** the view with `⌘+` / `⌘−`, `⌘0` for 100%, `⌘1` to fit the page and
its margins to the window, `⌘`-wheel or a trackpad pinch to zoom about the
pointer. The zoom group in the bottom strip, beside Undo and Redo, is the
readout: − and + step, the slider sits at 100% in the middle, and the
percent opens Fit width / 100%. Zoom is a view setting for the tab: it
changes nothing in the note, is not saved with it, and a freshly opened
tab starts at 100%. Every tab keeps its own zoom — each note in the tab bar,
and each table or PDF tab along the bottom. The same keys and the same group zoom a PDF or a
sketch opened full-frame. To **pan** a zoomed page, hold Space in
annotation mode (or in a PDF or sketch tab) and drag with the hand, or
drag with the middle mouse button at any time.

![The page-width ruler](images/mn008.png)

Down the right side runs the **block rail**: a dot for every block, the
focused block's a little brighter. **Drag a dot to reorder its block**
(a table row's dot carries the row; the header row's carries the table);
an accent line shows where it will land.

## Side by Side

A **split row** sets blocks side by side in **lanes**. Each lane is a short
stack of ordinary blocks — paragraphs, lists, images, sketches — and the
row is as tall as its tallest lane.

- **Make one:** right-click a block and choose **Split into columns**, or
  `⌘`-drag from the left or right edge of a block (a plain drag at a
  table cell's edge resizes the column). Dropping a dragged block or a
  file on a block's side edge puts it in a new lane beside that block.
- **Resize:** drag the gap between two lanes. It snaps at ¼, ⅓, ½, ⅔ and
  ¾; dividers that line up with the rows above and below move together —
  hold `⌥` to move just this row's.
- **Move around:** `←` / `→` walk the lanes in reading order, `↑` / `↓`
  stay in a lane until its top or bottom; `Tab` / `⇧Tab` jump to the next
  or previous lane.
- **Tidy up:** the block menu offers **Align lanes**, **Merge with the row
  below**, and **Delete lane**. Empty a lane and its space goes back to
  its neighbour; a row left with one lane becomes plain blocks again.

Tables always span the page, so a table never lands inside a lane — paste
one (or a whole split row) with the caret in a lane and it goes in just
below the row.

## Inline Formatting

Select text and use a shortcut or the Inspector — **bold**, *italic*,
`code`, underline, strikethrough, links (`⌘K`). Clear formatting on a
selection with `⌘\`.

### Colors

The Inspector's palette has three tabs: **Draw** (annotations) **Text** (text color) and **Back** (background). Pick a swatch with text selected to color it; with nothing
selected the color arms as a pen for what you type next. The same palette
colors table selections — whole rows and columns included (see
[Tables](tables.md)). **Revert** clears both back to default.

![The color palette](images/mn009.png)

### Choice Chips

`⌥⌘C` inserts an inline **choice chip** — To do / Doing / Done — anywhere
in text, including inside table cells. Click the chip to change its state;
the chip travels with the text through exports.

![The color palette](images/mn009b.png)

## Undo & History

Every gesture is one undo step — a bulk table edit, a whole tab merge, a
run of image-resize nudges all take a single `⌘Z`. The Inspector's
**History** view shows the note's timeline: click any entry to time-travel
the document to that state (and forward again).

![The History panel](images/mn010.png)
