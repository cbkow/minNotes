---
title: Coming from minNotes 2
permalink: /from-2/
nav_order: 3
---

# Coming from minNotes 2

minNotes 3 is a new application under the same name. It was written again
from the start, and it is organised differently: a document is no longer one
canvas with pages on it, but a **collection** of notes, folders and canvases
in a sidebar.

It installs over minNotes 2 (see [Installation & Updating](installation.md)).
Read this page before you let it.

## Your minNotes 2 documents

**minNotes 3 does not open documents made with minNotes 2, and it does not
import them.** This covers their pages and their canvases, and documents
from minNotes 1 as well. A package made by minNotes 2 holds such a document,
so it is of no use to minNotes 3 either.

When you try to open one, minNotes 3 says "This document was made with an
earlier minNotes and can't be opened." and leaves the file as it is. Nothing
is written into it.

Both generations use the extension `.mnd`, so the name of a file does not
tell you which one made it.

## What to do with old work

- **Keep the files.** They stay as they are, and minNotes 2 goes on reading
  them. If you still need to work in them, keep a copy of minNotes 2 and do
  not replace it yet.
- **Bring the text across by export.** In minNotes 2, export a page as
  Markdown, HTML or Word. In minNotes 3, open a collection and choose
  **File > Import…**: each file becomes a new note. Check the note against
  the original; an import carries text, formatting, tables and pictures, not
  everything minNotes 2 could show. See [Import](import.md).
- **Canvases do not come across.** A minNotes 2 canvas has no export that
  minNotes 3 reads back as a canvas. Lay it out again on a
  [canvas](canvas.md) in the new collection.
- **Video notes are safe.** The notes and drawings on a video are kept in a
  folder beside the video, in the format QCView uses, not in the minNotes
  document. Add the same video to a note in minNotes 3 and they are there.

The source code and the manual of minNotes 2 stay on the
[`2.x` branch](https://github.com/cbkow/minNotes/tree/2.x) of the minNotes
repository.

## Where things went

| In minNotes 2 | In minNotes 3 |
|---|---|
| A file is a canvas with pages on it | A file is a **collection**: notes, folders and canvases in a tree. A page is now a **note**. See [Collections, Notes & Folders](collections.md) |
| The path bar across the top | The sidebar's tree. Click a note or a canvas to open it |
| Several files open, switched from the file name | One collection in the window |
| The canvas is the front door | A canvas is one kind of entry in the tree, made when you want one. Notes are dragged onto it from the sidebar. See [The Canvas](canvas.md) |
| Save overwrites, with a warning when the file changed | Your work is kept on the machine at once; **Save** publishes it to the document and merges what someone else saved meanwhile. See [Saving & Shared Storage](saving.md) |
| The Inspector, the History timeline | Gone. Undo and redo remain, one history for each note and each canvas |
| The block rail down the right side | One handle in the left margin, beside the block the caret is in. See [Editing](editing.md) |
| Blocks side by side in lanes | A **Columns** block |
| The built-in spelling and grammar checker | Gone. The note's text is shown to the system instead, so a writing tool you have installed, such as Grammarly, can check it. See [Privacy](privacy.md) |
| The board (kanban) view of a table, sorting, the row filter | Gone. See [Tables](tables.md) |
| The video studio, the full-window PDF and table views, the lightbox | Gone. A video is played, noted and drawn on in its block; a PDF is paged and drawn on in its block. See [Media & Drawing](media.md) |
| Streams | Gone |
| Evernote and Notion import, **Merge File Into This Page…** | Gone. See [Import](import.md) for what is read |
| A file dropped on a page is imported | A dropped file is a picture or a file chip. Only **File > Import…**, or a drop on the sidebar, imports |
| The hidden `.minnotes` folder for kept media | A visible folder, `minNotes Media`, beside the document |
| A package opens read-only, straight from the archive | A package is unpacked to a folder when it is opened, and is then an ordinary collection. See [Collect Media & Packages](packages.md) |
| The **Document** and **Help** menus | **File**, **Edit**, **Format**, **Export** and **View**. **Collect Media…** and **Path Mappings…** are in **File** |
| The export options dialog | Two ticked items in the **Export** menu: **Include File Paths** and **Include Video Notes** |

## What is new

- Folders in the sidebar's tree.
- A save that merges, for collections on shared storage.
- Video notes and drawings made in the note, without leaving it.
- [Web Folder](web-folder.md) export: a note, some notes or the whole
  collection as pages for a browser, with videos that play and an optional
  password.
