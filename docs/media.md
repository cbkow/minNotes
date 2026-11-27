---
title: Media & Drawing
permalink: /media/
nav_order: 8
---

# Media & Drawing

A note can hold pictures, PDFs, videos and any other file. Each one is a
block of the note, like a paragraph or a table. You look at media, play it,
draw on it and write review notes on it **in the block itself**. When a PDF
or a picture needs more room, **Open PDF** and **Open Image** show the same
block across the whole window; see [The Large View](#the-large-view).

![Media blocks in a note](images/mn3-media-blocks.png)

## Adding Media

- **Drop** files from the Finder or Explorer onto the note. A line shows
  where they will land, between two blocks. Several files dropped together
  are one undo step.
- Use the toolbar's **Picture** or **File** button and choose files in the
  system's dialog. They go in after the block the caret is in.
- **Paste** with `Cmd+V` / `Ctrl+V`: files copied in the Finder or Explorer,
  or a picture copied in another application (a screenshot, **Copy Image**
  in a browser).

What a file becomes depends on its kind: a picture, a PDF and a video each
get a block of their own; anything else, a folder included, becomes a file
chip.

Pictures inside text you paste from a web page, a spreadsheet or Markdown
come along with it. Each is fetched once from its address and then kept
with the collection.

**Note:** a file dropped on the note is never imported as text. To turn a
document into a note, use **File > Import…**.

## Linked, Not Copied

minNotes points at media where it lies. Videos, PDFs and file chips are
always references: nothing is duplicated, and the original is never
changed.

Pictures are the exception, so that a screenshot does not vanish with its
temporary file:

| What you add | What minNotes keeps |
|---|---|
| A picture on a local disk | A copy, in `minNotes Media/` beside the collection |
| A picture on a network share | A reference to the file where it is |
| A pasted picture, or one fetched from the web | A copy, in `minNotes Media/` |
| A Photoshop, SVG, Illustrator, HEIC or RAW file | A reference to the original, plus a PNG preview in `minNotes Media/` |
| A video, a PDF, any other file, a folder | A reference to the file where it is |

`minNotes Media/` is an ordinary, visible folder next to the `.mnd` file.
A new copy waits on your machine and is written there by the next **Save**
(see [Saving & Shared Storage](saving.md)).
If you move the collection by hand, move the folder with it.

To bring every referenced file into that folder, so the collection can
travel, use **File > Collect Media…**. See [Packages](packages.md).

## Pictures

minNotes reads PNG, JPEG, TIFF, TGA, BMP, EXR and DPX directly, and the
first frame of a GIF or WebP. A JPEG stored on its side is shown upright,
as its EXIF orientation says.

Some files are shown through a **preview** rendered from them, with a small
label in the corner naming the format:

- **PSD / PSB**: the flattened picture. The file must have been saved with
  "Maximize Compatibility".
- **SVG**: drawn at the size it is shown.
- **AI**: the first artboard, flat.
- **HEIC** and camera **RAW** (DNG, CR2, CR3, NEF, ARW, RAF, ORF, RW2, PEF,
  SRW): macOS only.

When the original changes, the preview is rendered again by itself.

A picture comes in at its own size, never wider than the page. To resize
it, in editing, click it: the current picture gets an outline and a handle at each
corner. Drag a corner. `Esc` during the drag gives it up. To go back,
right-click the picture and choose **Original Size**.

When a picture cannot be shown, its box says why:

- **Not found**, with the file's name and where it was looked for. This is
  also what you see when the file is on a volume this machine has no path
  for.
- **Cannot be shown**, with the reason, when the file is there but could
  not be read.

## PDFs

A PDF block shows one page at a time. The strip under the page has the
page number among the pages, with an arrow on each side: click the left
half of the strip for the previous page, the right half for the next. The
page a block shows is kept with the note, except in reading, where turning
pages changes nothing in the note.

Drag across the page to select text, then `Cmd+C` / `Ctrl+C` to copy it.
With the PDF block current, `Cmd+A` / `Ctrl+A` selects the whole page's
text. `Esc` lets the selection go.

To read a PDF through, with every page in one scrolling column, choose
**Open PDF** from its menu or press the page twice in quick succession; see
[The Large View](#the-large-view).

## Video

A video block shows a still of the clip with its length in the corner.
Under it is a strip: rewind, play and fast-forward buttons, the timecode of
the frame, and a line to scrub on.

- Click the picture, or press `Space` with the block current, to play and
  pause.
- Drag on the line to scrub.
- Hold rewind or fast-forward to run through the clip, as `J` and `L` do.
- One video plays at a time. A playing video that scrolls out of view
  pauses.

The block remembers the frame it stands on (in reading, the frame is not
written into the note, so playing a video does not mark the note as
changed). Loop, speed, volume and mute
are remembered between launches; when one of them is not as usual (a speed
other than 1, muted, not looping) the strip says so at its right end.

The timecode reads `HH:MM:SS:FF`, counted from the clip's first frame.

While a video block is current, the keyboard is the transport:

| Action | Key |
|---|---|
| Play / pause | `Space`, `K`, `W` or `S` |
| Step a frame back / forward | `Left` / `Right`, or `Q` / `E` |
| Rewind / fast forward (hold) | `J` / `L`, or `A` / `D` |
| Go to the start / the end | `Home` / `End` |
| Loop on and off | `V` |
| Cycle the review speed | `R` |
| Reset the speed | `Shift+R` |
| Mute | `M` |

`Up` and `Down` leave the block, as they do for any block.

minNotes plays the formats QCView plays, among them MOV, MP4, M4V, MXF,
MKV, AVI and WebM. The playing video is shown through the same colour
pipeline as in QCView, HDR included. A video at rest shows an SDR still. A clip stored
on its side, as phones store them, is shown upright: at rest, while it
plays and in a web folder.

## Other Files

Any other file is a **chip**: an icon for its kind, the name, and the size
and extension. Audio files, spreadsheets, archives and folders are all
chips. A chip always points at the file where it is.

Click a chip to select it; click it again to open the file in its own
application.

## The Media Menu

Right-click a media block for its commands, above the usual block
commands:

| Command | On | What it does |
|---|---|---|
| **Open PDF** | PDFs | Shows the PDF across the window; see [The Large View](#the-large-view) |
| **Open Image** | Pictures | Shows the picture across the window |
| **Reveal in Finder** / **Show in Explorer** | Pictures, PDFs, videos, chips | Shows the file in its folder |
| **Copy Path** | Pictures, PDFs, videos, chips | Copies the file's path |
| **Open** | Chips | Opens the file in its own application |
| **Locate File…** | Chips; a picture, a video or a PDF that is not found | Points the block at the file again |
| **Original Size** | A resized picture | Removes the width you gave it |

A picture pasted from the web that has not been fetched has no file, so it
has no such rows.

![The menu of a file chip](images/mn3-media-menu.png)

## Missing Files

A file that has moved, or is on a volume that is not connected, shows as
**Not found** (a chip says **Missing**).

Right-click the block and choose **Locate File…**, then pick the file.

- A **video** or a **PDF** is pointed at the file where it is now and keeps
  everything else: it is the same file, moved.
- When you relink one **picture**, the note's other missing pictures that
  have a file of their name in the same folder are linked with it, in the
  same undo step.
- A **chip** has **Locate File…** in its menu at all times.

**File > Collect Media…** lists every file the collection refers to and
shows the missing ones, each with a way to locate it. See
[Packages](packages.md).

When files are missing because the same volume has a different path on
this machine (a share mounted as `/Volumes/projects` on macOS and `P:` on
Windows), set the volume up under **File > Path Mappings…** on each
machine, under the same name. Files on that volume are then recorded by
the volume's name and their place on it, and found on every machine that
has a volume of that name. See [Packages](packages.md).

## Drawing on Pictures and PDF Pages

Drawing needs the window to be in editing; in reading the tools are put
away. See [Reading and Editing](collections.md#reading-and-editing).

Click a picture or a PDF block. While it is the current block, the toolbar
shows the drawing tools in place of the text tools.

![The drawing tools over a picture](images/mn3-drawing-toolbar.png)

| Tool | Key | What it does |
|---|---|---|
| Pick | `V` | Picks a stroke on its line: drag to move it, `Delete` to remove it. Drag a box over empty ground, or `Shift`-click, to pick several: they move, go and take a colour or a width together |
| Pen | `P` | Draws freehand. A tablet's pressure varies the line |
| Pen path | | A path point by point: click for a corner, drag for a curve; click the first point to close it, `Enter`, `Esc` or a right click ends it |
| Line | `L` | A straight line |
| Arrow | `A` | An arrow |
| Rectangle | `R` | A rectangle |
| Oval | `O` | An oval |
| Eraser | `E` | Rubs out the part of a pen line it passes over; a shape or a pen path goes whole |

While the pen path is armed, or a path is picked, four more tools light up
after the drawing tools:

| Tool | What it does |
|---|---|
| Select points | Click a point to pick it (`Shift`-click for more), or drag a box over empty ground to pick several. Drag a point or one of its handles; `Delete` removes the picked points |
| Add a point | Click on the path. The curve keeps its shape |
| Remove a point | Click a point |
| Corner or curve | Click a point to switch it between a corner and a curve; drag on it to pull out new handles |

They work on the picked path. Hold `Alt` while dragging a handle to move it
alone.

After the tools come the **pen colour** and the **Thinner line** /
**Thicker line** buttons. With a stroke picked, they change that stroke.
The colour and width are remembered.

Press a tool's key or button again to put it down; `Esc` does the same.
Each stroke is one undo step of the note.

A drawing belongs to the picture, or to the PDF page it was drawn on. It
scales with the block, turns with the page, and goes out with the note's
exports. The picture or PDF file itself is never changed.

## The Large View

A PDF or a picture can be shown across the whole window, over the page and
the sidebar. It is the same block, only larger: the same file and the same
drawings, and nothing new is stored.

![A PDF in the large view, with the strip of small pages](images/mn3-large-view-pdf.png)

**To open it**

- Right-click a PDF block and choose **Open PDF**, or a picture block and
  choose **Open Image**.
- Or press the PDF's page, or the picture, twice in quick succession, with
  no drawing tool in hand.
- On a canvas, select the card and choose **Open PDF** or **Open Image**
  from the canvas's menu; see [The Canvas](canvas.md).

**To leave it**, press `Esc` or the **Back** button at the left of the
view's head. The note is where you left it.

### Pages

A PDF shows every page in one column that scrolls. The head names the file.

- Scroll with the wheel or the trackpad, or with `Up` / `Down`,
  `Page Up` / `Page Down`, `Space` / `Shift+Space` and `Home` / `End`.
- A PDF of more than one page has a **strip of small pages** at the left,
  each with its number. The page in view is marked and kept in sight. Click
  a small page to go to it. The wheel over the strip moves the strip alone.
- A picture is one page, shown whole at first.

Leaving the view does not change the page the block shows in the note.

### Zoom

The head has, at its right, **Zoom out**, the percentage, **Zoom in** and
**Fit**. At 100 % a PDF is at its paper size and a picture is pixel for
pixel. The zoom goes from 5 % to 1600 %.

| Action | macOS | Windows |
|---|---|---|
| A PDF: the view's width. A picture: the whole picture | `Cmd+0` | `Ctrl+0` |
| The whole page in the view | `Cmd+9` | `Ctrl+9` |
| Zoom in / out | `Cmd+=` / `Cmd+-` | `Ctrl+=` / `Ctrl+-` |
| Zoom about the pointer | `Cmd` + wheel, or pinch | `Ctrl` + wheel, or pinch |
| Scroll sideways, when the page is wider than the view | `Shift` + wheel, or a sideways swipe | `Shift` + wheel, or a sideways swipe |

For a PDF, **Fit** switches between the view's width and the whole page.

### Drawing

In editing, the toolbar shows the drawing tools while the large view is
open, with the same keys as in the note. Take a tool and draw: the stroke
goes on **the page under the pointer**. Each stroke is one undo step.

A drawing made here is the block's own. It is on the page in the note
afterwards, and in the note's exports.

`Esc` steps back one thing at a time: the stroke being drawn, a picked
stroke, the tool in hand, and then the view itself.

In reading the tools are away, as they are in the note.

### Selecting and Finding Text

With no tool in hand, and always in reading, a PDF's text can be selected.

| Action | macOS | Windows |
|---|---|---|
| Select text | Drag across it, within one page | Drag across it, within one page |
| Select the text of the page in view | `Cmd+A` | `Ctrl+A` |
| Copy the selection | `Cmd+C` | `Ctrl+C` |
| Find through every page | `Cmd+F` | `Ctrl+F` |
| The next place found | `Enter` or `Cmd+G` | `Enter` or `Ctrl+G` |
| The place before | `Shift+Enter` or `Shift+Cmd+G` | `Shift+Enter` or `Ctrl+Shift+G` |
| Let the selection go; close the find field | `Esc` | `Esc` |

`Cmd+F` / `Ctrl+F` opens a field in the head, "Find in this PDF…". The
words are looked for through every page, without regard to capitals or to
spaces. The places found are washed on their pages, the pages scroll to the
one you are on, and the head counts them: "3 of 12", or "not found". In a
long PDF it says "reading page 40 of 400" until the first place is found.

Find in the large view is for PDFs; a picture has no text to find.

While the large view is open, the keys are its own: nothing you type
reaches the note under it. Save and the reading switch still work.

## Video Review Notes

A review note belongs to **one frame** of a clip. It can hold a drawing,
words, or both.

Writing and drawing review notes need the window to be in editing
(`Cmd+R` / `Ctrl+R`). In reading you can play and scrub the video and see
its notes, not add or change them. See
[Reading and Editing](collections.md#reading-and-editing).

Click a video block. The toolbar now shows, in order: select, pen, pen
path, line, arrow, rectangle, oval, eraser and **Write a note on this
frame**; then the four path tools (see
[Drawing on Pictures and PDF Pages](#drawing-on-pictures-and-pdf-pages));
then the pen colour, thinner and thicker; and at the right end **Mark the note
addressed** and **Delete the note**.

![A video with its review notes](images/mn3-video-notes.png)

### Writing a Note

1. Go to the frame: scrub, or step with `Left` / `Right`.
2. Click **Write a note on this frame**. A field opens in the bar under
   the video's controls.
3. Type, then press `Return` / `Enter` to keep the words. `Esc` gives them
   up.

To change a note's words later, go to its frame and click them in the bar
under the controls, or click the toolbar button again.

The bar shows a note's words on two lines. When there are more, point at
the bar to read all of them. The picture itself stays clear.

### Drawing on a Frame

Click a drawing tool, then draw on the picture. The video pauses for it.
Drawing on a frame that has no note makes one.

With a tool armed, a click on the picture draws instead of playing. Click
the tool again to put it down.

The select tool picks a drawing: drag to move it, drag its corners to
scale it, `Delete` to remove it. Drag a box over empty picture, or
`Shift`-click, to pick several; they move and go together. `Cmd+Z` / `Ctrl+Z` and `Shift+Cmd+Z` /
`Ctrl+Shift+Z` step through the clip's drawings. That history is the
clip's own, apart from the note's.

### Finding Notes Again

- Every note is a **mark on the scrub line**. Click a mark to go to its
  frame.
- A clip with notes has a **row of small pictures** under the strip, one
  per note, each with its timecode. Click one to go to its frame; click it
  again to edit its words. The one the video stands on is outlined.
- On a note's frame, its drawing shows over the picture and its timecode
  and words along the lower edge.

### Addressed, Hidden, Deleted

- **Mark the note addressed** ticks the note the video stands on, and
  again unticks it. An addressed note carries a tick and its mark on the
  line changes colour.
- The **eye** at the right of the scrub line hides the notes' drawings and
  captions on the pictures, for every video, and shows them again. The
  marks and the row stay.
- **Delete the note** removes the note of the current frame, with its
  drawing.

### Shared With QCView

Review notes are not stored in the collection. They are kept beside the
clip, in a `.qcview` folder, in QCView's own format. Open the same clip in
[QCView](https://qcview.com) and you see the same notes and drawings;
notes written there show up in minNotes. Anyone who has the clip and that
folder has the notes.

QCView 3.0.3 and later can start this from its side: **Export to
minNotes…** in its Notes panel asks for a place and minNotes makes a note
there with the clip in it, and **Open in minNotes** on a live stream makes
a note of the stream's captured notes. See
[Installation](installation.md#opening-a-collection-from-outside-minnotes).

To leave a clip's notes out of an export, untick **Export > Include Video
Notes**. See [Export](export.md). In a web folder a video plays in the
browser with its notes; see [Web Folder](web-folder.md).
