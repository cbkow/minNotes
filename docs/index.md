---
title: Home
permalink: /
nav_order: 1
---

# minNotes

A fast, block-based notes editor for macOS and Windows. Notes are plain
SQLite files (`.mndb`) you can keep anywhere — local disk, a network share,
Dropbox, LucidLink — and open on either platform.

## What it does

- **Block editor** — paragraphs, headings, quotes, code blocks with syntax
  highlighting, bullet / numbered / task / choice lists, dividers. A steady
  reading measure with a block-number ruler down the right side — drag a
  number to reorder its block. Each note picks its own page width, from a
  prose measure up to a wide board for image-heavy work.
- **Tables + kanban** — rich cells, choice/check columns, sort & fill, and a
  one-click board view grouped by a column.
- **Media inline** — drop or paste images, video (with a scrub + annotate
  studio), PDFs (page-by-page), and file attachments.
- **Ink & comments** — pick a drawing tool and mark up anything: the page
  and its margins, sketches, video frames (interchangeable with QCView),
  even PDF pages directly. Comments anchor to text — a blue bubble in the
  margin opens the thread right there.
- **Import** — Markdown, plain text, CSV/TSV, HTML, Word, RTF (macOS),
  Evernote `.enex`, and Notion exports; a multi-note import becomes a
  folder of notes.
- **Export** — Markdown (with an assets folder), a self-contained HTML
  review page (one file: annotations as toggleable layers, comment threads
  on hover, a click-to-zoom lightbox, block numbers in the margin), a Word
  document where your comments arrive as native review comments, or a
  sealed **package** (`.mnpkg`) with every referenced file inside.
- **Markdown-style input** — type `## `, `- `, `> `, ```` ``` ```` and the
  block converts as you go.
- **Multi-document tabs** and **cross-OS path mapping** so referenced media
  resolves on every machine.
- **Auto-update** built in (Sparkle on macOS, WinSparkle on Windows).

## Get going

1. **Download** the latest `.dmg` (macOS) or `.exe` (Windows) from the
   [releases page](https://github.com/cbkow/minNotes/releases/latest).
2. Open or create a note, then read the [Editing](editing.md) primer and the
   [Keyboard shortcuts](shortcuts.md).

## Saving

minNotes edits a fast local working copy of your note and writes back to the
original file when you **Save** (`⌘S` / `Ctrl+S`). A dot on the tab and an
"Edited" hint in the title show unsaved changes; closing a tab or quitting
with unsaved work prompts you first. This keeps notes safe even on network
shares that don't play well with live databases.
