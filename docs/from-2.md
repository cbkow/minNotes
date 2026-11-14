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
Your old documents can be imported; read below what an import brings.

## Your minNotes 2 documents

minNotes 3 does not open a minNotes 2 document as it is, but it **imports
its pages**. The old file is never changed, so you can import it again or
keep using it in minNotes 2.

Open the old document the way you open any collection: double-click it,
choose **File > Open…**, or drop it on the sidebar. minNotes 3 says the
file was made with minNotes 2 and offers **Import**. A package made by
minNotes 2 (`.mnpkg`) is offered the same way.

- A new, untitled collection opens with a folder named after the old file.
  Each page is a note in it, in the old order, named as minNotes 2 named
  the page: its first heading, or else its first line.
- The first **Save** asks where the new collection goes. Give it a new
  name, or save it in another folder: both generations use the extension
  `.mnd`.
- To add an old document's pages to a collection you already have open,
  choose **File > Import…** and pick the old file.

When the import is done a message says what came and what did not.

### What comes across

- Text with its formatting, headings, lists, tasks, quotes, code, dividers,
  links, colours and choice chips.
- Columns, and tables with their typed columns, widths and colours.
- Pictures, videos, PDFs and file chips. A file minNotes 2 kept with the
  document (in its hidden `.minnotes` folder, or inside a package) is
  copied into the new collection's `minNotes Media` folder when you save.
  A file referred to where it lies is referred to in the same place.
- Drawings on a PDF. Video notes and drawings are not in the document at
  all: they are in the folder beside the video and are there as before.

### What does not

- **Anything on the old canvas outside a page**: pictures, files, text
  notes, drawings, frames and connectors placed directly on the canvas.
  The message counts them. The canvas's layout is not imported either;
  lay it out again on a [canvas](canvas.md) and drag the notes onto it.
- Comments.
- A file on a shared volume that minNotes 2 found through its own path
  mappings shows as missing until you tell minNotes 3 where that volume is
  on this machine: **File > Path Mappings…**. See
  [Saving & Shared Storage](saving.md).
- Documents from minNotes 1. Open them in minNotes 2 and save them there
  first.

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
| The board (kanban) view of a table | A table with a choice or a check column can be shown as a board in the note. See [Tables](tables.md) |
| Sorting and the row filter of a table | Gone |
| The video studio, the full-window table view | Gone. A video is played, noted and drawn on in its block |
| The full-window PDF view, the lightbox | **Open PDF** and **Open Image** in the block's menu show a PDF or a picture large, over the window. See [Media & Drawing](media.md) |
| Streams | Gone |
| Evernote and Notion import, **Merge File Into This Page…** | Gone. See [Import](import.md) for what is read |
| A file dropped on a page is imported | A dropped file is a picture or a file chip. Only **File > Import…**, or a drop on the sidebar, imports |
| The hidden `.minnotes` folder for kept media | A visible folder, `minNotes Media`, beside the document |
| A package opens read-only, straight from the archive | A package is unpacked to a folder when it is opened, and is then an ordinary collection. See [Collect Media & Packages](packages.md) |
| The **Document** and **Help** menus | **File**, **Edit**, **Format**, **Export**, **View** and **Help**. **Collect Media…** and **Path Mappings…** are in **File** |
| The User Guide, built as a canvas | A folder of pages that can only be read, at the foot of the sidebar, beside your collection and never part of it. It is put away until you choose **Help > User Guide**. See [Collections, Notes & Folders](collections.md#the-user-guide) |
| A document opens ready to edit | A collection that exists opens in reading; `Cmd+R` / `Ctrl+R` switches to editing. See [Reading and Editing](collections.md#reading-and-editing) |
| The export options dialog | Two ticked items in the **Export** menu: **Include File Paths** and **Include Video Notes** |

## What is new

- Folders in the sidebar's tree.
- A save that merges, for collections on shared storage.
- Video notes and drawings made in the note, without leaving it.
- [Web Folder](web-folder.md) export: a note, some notes or the whole
  collection as pages for a browser, with videos that play and an optional
  password.
