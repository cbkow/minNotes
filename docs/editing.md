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
| `1. ` (any number) | Numbered list item |
| ```` ``` ```` or ```` ```lang ```` then `Enter` | Code block (optionally for a language) |
| `--- ` (or `---` / `***` / `___` then `Enter`) | Divider |

List items nest with `Tab` / `Shift+Tab`.

## The page

The page is a steady reading measure — it never squeezes. A narrow window
scrolls sideways instead of rewrapping your text, and a wide table extends
past the page into the margin, scrolling the page horizontally when it
outgrows the window.

Each note has its **own page width**. The slim ruler above the page shows
the stops (760 up to 1600) — drag the handle or click a stop to change it;
the page reflows live and one Undo puts it back. Margin ink rides along:
annotations in the gutters keep their place beside the text as the page
widens. Pick a wide page for image boards, the classic measure for prose.

Down the right side runs the **block ruler**: every block's number, a shared
address you can reference anywhere ("see block 14" — the numbers also appear
in exports). **Drag a number to reorder its block**; an accent line shows
where it will land.

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
- **PDF** — paged inline with prev / next; open full-frame to read and
  **draw on the pages themselves** (see below). Page markup shows on the
  inline preview too; the PDF file is never modified.
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

## Comments

Select text in a single block and choose **Add comment** from the right-click
menu. A blue bubble appears in the margin beside the block — click it to open
the thread right there: read, reply, resolve. The Inspector's **Comments**
view lists every thread in the note.

## Drawing

There's no annotate mode to switch on — **the tool in your hand is the
mode**. The Tools grid at the top of the Inspector holds a Type tool
(regular editing — where you always start), Select/Move, and the drawing
tools: freehand, line, arrow, rectangle, oval, text box, eraser.

- **Pick any drawing tool** and draw anywhere — over text, in the margins,
  on media. Ink pins to the block it overlaps (ink on an image scales with
  the image), and it always renders exactly as drawn.
- **Press `Esc`** to drop back to typing. Closing the Inspector does the
  same.
- In a **PDF tab** the tools draw on the page under the cursor; hold
  **Space** to pan the pages with the hand. In a **sketch** or **video**
  tab they draw on that canvas.
- **Select/Move**: click an annotation to grab it, drag a **rubber band**
  over several (Shift-click adds or removes one), then drag the group or
  press `Delete` — each gesture is a single Undo step.
- The **eye** on the tab strip shows or hides the ink layer while you read.

## Importing

**File ▸ Import…** (or drop a file on the welcome screen) brings existing
notes in: Markdown, plain text, CSV/TSV, HTML, Word (`.docx`), RTF
(macOS), Evernote `.enex`, and Notion exports. A file becomes a note; an
archive of many becomes a folder of notes.

## Exporting

**File ▸ Export** writes your note as:

- **Markdown** — a portable `.md` plus a `.assets/` folder for images,
  sketches, and video-note thumbnails. Comments become footnotes.
- **HTML** — one self-contained file you can send anywhere. Colors,
  highlights, and tables survive exactly; annotations ride as layers behind
  an **Annotations** toggle; comment threads pop up on hover; every block
  keeps its ruler number; click any image — video-note frames included —
  to view it large in the built-in lightbox.
- **Word (.docx)** — your comments arrive as **native Word review comments**,
  anchored to the text, ready for replies.
- **Package (.mnpkg)** — a sealed snapshot with every referenced file
  inside: one file to archive or hand off. Packages open instantly, and
  video even plays straight out of the archive.

Videos, PDFs, and file attachments export as a poster image plus a reference
card (name, path, and metadata) — and a video's QCView notes can come along
underneath it, annotated frames included.

## Sketches

Insert a **sketch** block to draw inline with a pressure-smoothed pen. Sketches
edit full-frame and live in the note like any other block.

## Documents and tabs

Open several notes at once in tabs. Each tab is an independent note with its
own undo history. **New** (`⌘N`), **Open** (`⌘O`), **Save As** (`⌘⇧S`), and
**Close** (`⌘W`) are in the File menu.
