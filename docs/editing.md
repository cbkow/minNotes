---
title: Editing
nav_order: 2
---

# Editing

A note is a list of **blocks**. Press `Enter` to start a new block, `Backspace`
at the start of an empty block to merge it up. Type to edit; everything is
saved to your note when you **Save** (`⌘S` / `Ctrl+S`).

## Markdown-style triggers

Type these at the **start** of a block and it converts as you go:

| Type this | Becomes |
|---|---|
| `# ` … `###### ` | Heading 1–6 |
| `> ` | Quote |
| `- `, `* `, `+ ` | Bullet list item |
| `- [ ] ` / `- [/] ` / `- [x] ` | Task (to-do / doing / done) |
| ```` ``` ```` or ```` ```lang ```` then `Enter` | Code block (optionally for a language) |
| `--- ` (or `---` / `***` / `___` then `Enter`) | Divider |

## Inline formatting

Select text and use the toolbar or a shortcut — **bold**, *italic*,
`code`, underline, strikethrough, a link, a text colour, or a highlight.
See [Keyboard shortcuts](shortcuts.md). Clear all formatting on a selection
with `⌘\` / `Ctrl+\`.

## Media

Drag a file onto the note, or paste from the clipboard:

- **Images** — shown inline; resize by dragging the handle.
- **Video** — an inline player with a transport bar; open it full-frame to
  scrub and draw annotations (interchangeable with QCView).
- **PDF** — paged inline with prev / next; open full-frame to read.
- **Other files** — added as an attachment chip you can reveal in Finder /
  Explorer.

Referenced media stays where it is on disk; minNotes stores an OS-neutral
reference so the note resolves on macOS and Windows. Set up share roots under
**File ▸ Path Mappings…**.

## Tables and kanban

Insert a table from the toolbar (or paste tab-separated text). Cells take rich
text and images; columns can be plain text, a **choice** (single-select
options), or a **check** column. Sort by a column, fill a selection down/right,
reorder rows and columns, and switch a choice/check column to a **board**
(kanban) view.

## Sketches

Insert a **sketch** block to draw inline with a pressure-smoothed pen. Sketches
edit full-frame and live in the note like any other block.

## Documents and tabs

Open several notes at once in tabs. Each tab is an independent note with its
own undo history. **New** (`⌘N`), **Open** (`⌘O`), **Save As** (`⌘⇧S`), and
**Close** (`⌘W`) are in the File menu.
