---
title: Privacy
permalink: /privacy/
nav_order: 15
---

# Privacy

minNotes works on your machine and on the storage you choose. It has no
accounts, and it sends no usage data, statistics or crash reports.

## What minNotes sends

minNotes uses the network for two things.

**The update check (macOS).** Once a day, and when you choose
**minNotes > Check for Updates…**, minNotes reads the update feed at
`https://minnotes.app/appcast.xml`. It sends no profile of your system with
the request. When you accept an update, it is downloaded from the address the
feed names. On Windows the Microsoft Store installs and updates minNotes, and
minNotes makes no update requests of its own.

**Pictures named by a web address.** When you paste or import content that
names a picture on the web (a web page, formatted text from a browser, a
Markdown file with a picture's address), minNotes fetches that picture when
the block is first shown and keeps a copy with the collection. After that the
note uses the copy. The request goes to the address in your content and
identifies the application only as "minNotes".

Nothing else is fetched. A link in a note is a link: it is opened by your
browser when you follow it, and minNotes does not load the page behind it for
a title or a preview.

## Your text and the system's writing tools

minNotes has no spelling or grammar checker of its own. Instead, on macOS, it
shows the text of the open note to the system through the accessibility
interface, as one text field. This is what lets a writing tool you have
installed, such as Grammarly, and assistive software such as a screen reader,
read the note and place corrections in it.

minNotes does not send your text anywhere. A tool that reads it this way may:
what such a tool does with the text is set by that tool and its maker.

## What is kept on the machine

Besides the collections you save where you choose, minNotes keeps these on
the machine you work on:

| What | macOS | Windows |
|---|---|---|
| Your work since the last **Save**, for each open collection, and work left by a crash | `~/Library/Application Support/minNotes/sessions` | `sessions` in the `minNotes` folder of the application's local data (`%LOCALAPPDATA%`) |
| Settings: the list of recent collections, the player's loop, speed and volume, the pen's colour and width, the two export options, your [path mappings](packages.md) | `~/Library/Application Support/minNotes` | The same `minNotes` folder |
| Previews of pictures that are costly to read (Photoshop, SVG, HEIC and camera RAW files), up to 5 GB; the ones used longest ago go first | `~/Library/Caches/minNotes/previews` | `previews` in the same `minNotes` folder |

The list of recent collections and the path mappings hold file paths. None of
these files leave the machine.

## What is in a collection

A collection is one `.mnd` file, with a `minNotes Media` folder beside it for
the files kept with it. Videos, PDFs and other files you add are referenced
where they are, by their path, unless you collect them. See
[Media & Drawing](media.md) and [Collect Media & Packages](packages.md).

Anyone you give a collection to can read those paths, and the names of your
drives and folders with them.

## What is in an export

Exports are written where you choose and shared only by you.

- **File paths.** With **Export > Include File Paths** ticked (it is ticked
  to begin with), an export states the full path of each referenced video,
  PDF and file. Untick it before exporting for someone who should not see
  them.
- **Video.** The videos of a [web folder](web-folder.md) are made on your
  machine by the FFmpeg that comes with minNotes. Nothing is uploaded.
- **A web folder's password.** A password seals the pages, the list of notes
  and the search text; the media files are not sealed, only given names that
  cannot be guessed, so anyone who has a media file's address can open it
  without the password. There is no way to recover a forgotten password. See
  [Web Folder](web-folder.md).

## Checking this yourself

minNotes is free software. The source of minNotes 3 is in the
[minplatform repository](https://github.com/cbkow/minplatform) (`apps/mn`),
under the GNU General Public License, so everything on this page can be
checked. Questions: [open an issue](https://github.com/cbkow/minNotes/issues).
