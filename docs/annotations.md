---
title: Drawing & Ink
permalink: /drawing/
nav_order: 8
---

# Drawing & Ink

The Tools grid at the top of the Inspector holds the Type tool
(regular editing, where you always start), Select/Move, and the drawing
tools: freehand, line, arrow, rectangle, oval, text box, eraser.

![The Tools grid](images/mn017.png)

Pick a tool and draw anywhere — over text, in the margins, on media. Press
`Esc` to drop back to typing.

## On the Page

Ink pins to the block it overlaps, so it stays with its content as the
note changes. Ink in the margins is anchored to the page edges — it keeps
its place beside the text even when you change the page width. Ink on an
image scales with the image.

![Margin ink beside text](images/mn018.png)

The **eye** on the tab strip shows or hides the whole ink layer while you
read. Ink exports as toggleable layers in HTML and bakes into images for
Word and PDF exports.

## Select & Move

With **Select/Move**: click an annotation to grab it, drag a rubber band
over several (`Shift+Click` adds or removes one), then drag the group or
press `Delete`. Each gesture is a single undo step.

## Media Block Tabs

Below each note lives a tabbed interface for each media block in your document. **Clicking on a tab** opens the media block in a studio setup where you can annotate and edit special blocks like PDFs, videos, sketches, and tables.

![Drawing on a PDF page](images/mn019b.png)

## PDF Pages

Open a PDF full-frame and the drawing tools mark up **the page under the
cursor** — each page keeps its own ink, and the text tool drops text chips
onto pages. Hold **Space** to pan with the hand while a tool is armed.
Page markup shows on the inline preview, travels with the note, and
exports as annotated page images. The PDF file itself is never touched.

![Drawing on a PDF page](images/mn019.png)

## Video

Open a video full-frame for the review studio: scrub with skim audio,
step frames, and draw on any frame. Annotations save to a `.qcview`
sidecar next to the media — **fully interchangeable with QCView**: notes
made in either app open in the other.

![The video studio](images/mn020.png)

| Action | Shortcut |
|---|---|
| Rewind / fast-forward (hold) | `A` / `D` (or `J` / `L`) |
| Cycle review speed (0.5×–2×) | `R` |
| Reset speed | `⇧R` |

## Streams

Notes QCView takes on a live feed (a Premiere Pro or After Effects
sequence over QCBridge, or an SRT stream) have no clip behind them: they
live in the QCView project, in `.qcview/streams/<stream>/`, each with the
frame it captured and the time it was taken. QCView's **Open in minNotes**
hands that folder here; so does opening the folder (or its `notes.json`)
with minNotes.

The first open creates a document beside the notes with every note
already in it — the captured frame as an image block carrying QCView's
strokes as ink, then a line with the capture time, the host's timecode
when the feed carries one, and the note's text. The document is yours
from there: edit, reorder, export. The bottom rail keeps the folder in
view; as QCView adds notes to the stream, **Insert notes** brings the
folder's notes in again as blocks at the end of the document.

## Sketches

![A sketch block](images/mn021b.png)

Insert a **sketch** block for a blank canvas with a pressure-smoothed pen.
Sketches edit full-frame with pan and zoom, the canvas is resizable in
every direction, and ink is allowed to overflow the frame — pull the
frame out later to reveal it.

![A sketch block](images/mn021.png)
