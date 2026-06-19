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
  highlighting, bullet / task / choice lists, dividers.
- **Tables + kanban** — rich cells, choice/check columns, sort & fill, and a
  one-click board view grouped by a column.
- **Media inline** — drop or paste images, video (with a scrub + annotate
  studio), PDFs (page-by-page), and file attachments.
- **Ink** — sketch blocks and on-video annotations, interchangeable with
  QCView.
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
