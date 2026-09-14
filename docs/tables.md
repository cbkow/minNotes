---
title: Tables & Kanban
permalink: /tables/
nav_order: 6
---

# Tables & Kanban

A table is rows of cells, and every cell holds blocks — text with
formatting, images, video, lists. Insert one from the block menu
(**Insert table below**) or the left rail, paste tab-separated text (a
spreadsheet selection pastes straight in), or import a CSV, spreadsheet,
or HTML table. Wide tables extend past the page rather than squeezing.

![A table with the grip handles showing](images/mn011.png)

## The header row

The header row is the table's spine: column widths, alignment, and
types live there, and the rows beneath it are the body until the next
non-table block. Right-click a header cell for the **Table** menu —
**Add a header row**, **Remove a header row**, **Unassign header** (the
rows become plain side-by-side blocks), **Delete table**. Any split row
can become a table the other way round: **Assign as header**.

## Columns

Right-click any cell for the **Column** menu:

| Column type | What it does |
|---|---|
| Text | The default — any blocks |
| Choice | Single-select options with colors (edit the set via **Edit options…**) |
| Checkmark | Tri-state task per cell: to-do / doing / done (`Space` cycles it) |
| Timecode | Frame counts and timecodes snap to the column's frame rate when you leave the cell; sorts by frames |

The same menu inserts, moves, duplicates, aligns, and deletes columns,
and sorts the body by a column. Drag a column border to set its width;
the strip past the last column adds one to every row. Header cells stay
text whatever the column's type.

## Rows

Right-click for the **Row** menu — insert above or below, move,
duplicate, delete. `Enter` moves down the column; on the last row it
adds one, and `Enter` in an empty last row steps out of the table.
`⌘Enter` inserts a row below the caret's. `Tab` moves to the next cell
and adds a row past the last one.

## Selection

- **Click** a cell to edit it; **drag** across cells for a rectangle;
  `Shift+Click` extends it.
- **Grip handles** appear when you hover just above a column or left of
  a row — click to select the whole column or row, `Shift+Click` for a
  span, `⌘Click` to add or remove one. Drag a column grip to reorder
  columns; rows reorder by their rail number like any block.
- `⌘A` climbs: the block, its cell, the table, the document.
- `Esc` steps back down: a selected row becomes the whole table, then
  the caret lands below it.

## Bulk Operations

Right-click inside a grip selection for a focused menu:

![The bulk selection menu](images/mn012.png)

- **Clear contents** / **Delete** for the selected rows or columns (a
  selection that includes the header deletes the table)
- **Align** and **column type** changes across every selected column
- Palette colors apply to the whole selection
- `Backspace` clears a grip selection's contents; `⌘C` copies it as
  cells, tab-separated text, and an HTML table

Every bulk operation is a **single undo**. Deleting rows or columns is
menu-only, on purpose.

## Pasting

Tab-separated text pasted outside a table becomes a new one. Inside a
table it fills cells from the caret, growing the table as needed. Cells
copied with their header row paste by matching column names; **Paste
Special ▸ Paste into cells** fills by position instead.

## Table Tabs

Every table gets a tab in the bottom strip. It opens the table on its
own, with a **Table / Board** toggle and a **Filter rows…** field:
rows with no cell matching the text fold away, and the count shows
what's left. The filter is a view — copy, export, and undo see every
row — and clears when you leave the tab.

## Board View

Any choice or checkmark column can become a **kanban board**: right-click
the column → **View as board**, or the toggle on the table's tab. Cards
are the table's rows; drag a card between lanes to change its value (and
reorder). Double-click a card to jump back to its cell.

![Board view](images/mn013.png)
