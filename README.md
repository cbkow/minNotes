# minNotes

A fast notes editor for macOS and Windows built around a canvas: a file is an
infinite canvas whose elements are pages (block editors), media, ink and text.
Files are SQLite databases (`.mnd`); a page is a flat list of blocks; the
editor is a virtualized Qt Quick surface that owns the page model directly.

## What's in the box

| Surface | Stack |
|---|---|
| App shell, editor, dialogs, menus | Qt 6.11 / QML / Qt Quick Controls 2 |
| Document store (`.mnd`), blocks, undo | SQLite (C++ `app/core/`) |
| Block types | paragraphs, headings, code (syntax-highlighted), quotes, bullet/numbered/task/choice lists, dividers; blocks sit side by side in lanes, and a run of lanes with a header row is a table (typed columns, grips, row filter, kanban board) whose cells hold any blocks |
| Canvas | pages as drag-sized cards (edit in place at any zoom, or full-window), media elements (video with the full transport, PDF paging, images, audio, folders, files, bookmarks), ink and shapes with live restyling, marquee / multi-select / z order, a second undo log routed by focus, HTML export with pan/zoom + tabs and a PNG poster |
| Media | inline images, video (decode + scrub/skim audio + annotate), inline PDF, file attachments — inline in pages and as canvas cards |
| Ink | canvas drawing tools on single keys, on-video annotations (QCView-interop), drawing on PDF pages |
| Page | responsive width (the window's, or a 640/760/880 measure), one path bar (`File › Canvas › Page › Frame`), a mode-switched toolbar, Geist type (Light headings over Medium body copy, Geist Mono code) |
| Import | Markdown, txt, CSV/TSV, HTML, DOCX, Excel `.xlsx`, OpenDocument `.odt`/`.ods`, source-code files, RTF (macOS), Evernote `.enex`, Notion exports |
| Export | Markdown (+`.assets/`), self-contained fluid HTML (readable-width toggle, checkable tasks, lightbox), DOCX, PDF (tables print in column bands like a spreadsheet, header rows repeated), the canvas as HTML (tab strip, video-note filmstrips) or a PNG poster, `.mnpkg` sealed packages (media inside, video streams from the archive) |
| Cross-OS | OS-neutral path mappings for referenced media, multi-file switcher, `.mnd`/`.mnpkg` file associations |
| Merge | File ▸ Merge File Into This Page, or drop a `.mnd`/`.mnpkg` on the canvas — pages copy in as one undo entry (assets follow) |
| Updates | Sparkle (macOS) / WinSparkle (Windows), EdDSA-signed appcasts |

## Requirements

- **Qt 6.11.1** — including the **Qt PDF** module (install via the Qt Maintenance
  Tool; the inline-PDF feature won't build without it). Default macOS path
  `/Users/chris/Qt/6.11.1/macos`.
- **CMake 3.24+** with **Ninja**, a **C++20** compiler (Xcode CLT on macOS / MSVC
  2022 on Windows).
- **KF6SyntaxHighlighting 6.0** — built from source into `external/kf6` once per
  fresh clone: `external/build-ksyntax.sh`.
- **FFmpeg** — vendored prebuilt under `external/ffmpeg` (gitignored).
- **Sparkle** (macOS) — vendored once via `scripts/fetch_sparkle.sh` →
  `external/Sparkle/` (auto-update is a no-op shim if absent).
- **ink-stroke-modeler**, **SoundTouch**, **Hunspell** — fetched automatically
  by CMake (FetchContent); Hunspell is compiled in as a static library.
- **miniz**, **psd_sdk**, **stb_image** — vendored source under `external/`
  (the `.mnpkg` zip backend; the Photoshop and Radiance HDR readers behind the
  preview pass), compiled in; nothing to fetch.
- **Spelling / grammar data** — committed under `app/spell/en/`: a merged
  US + UK SCOWL dictionary and the parse-free subset of LanguageTool's English
  rules. Regenerate with `scripts/build-merged-dictionary.py` and
  `scripts/trim-lt-rules.py` (see `app/spell/en/README-data.md`).

## Build

```sh
just build      # configure + build the debug preset → build/debug
just run        # build + launch
just release    # release preset → build/release
just clean      # wipe build/
```

`just` isn't required — drive CMake directly:

```sh
cmake --preset=debug && cmake --build --preset=debug --parallel
./build/debug/app/minNotes.app/Contents/MacOS/minNotes   # macOS
```

## Repo layout

```
app/core/    C++ model: CanvasDocument (the file), BlockModel (a page), Document
             (SQLite), MediaStore, DocumentManager, PathMap, Converter (1.x → 2.0),
             Exporter (md/HTML/DOCX/PDF), CanvasExporter (HTML + poster), providers
app/notes/   ink stroke engine (canvas ink, media ink, video annotations), MediaHost
app/qml/     QML UI tree (Main, Editor, MediaBlock, tables, dialogs, …)
external/    vendored deps (ffmpeg, kf6, Sparkle, miniz, psd_sdk, stb) + build-ksyntax.sh
packaging/   macOS bundle (Info.plist, entitlements, icon) + Windows installer
scripts/     sign / notarize / appcast tooling
LICENSES/    third-party license texts (see below)
```

The macOS release flow is build → sign → notarize → DMG → appcast, via the
helpers in `scripts/` (`sign-and-notarize.sh`, `update_appcast*.sh`).

## License

minNotes is licensed **GPL-3.0-or-later**. See [`LICENSE`](LICENSE) for the GPL
text and [`LICENSES/`](LICENSES/) for the third-party license texts minNotes
redistributes (Qt LGPL-3.0, FFmpeg LGPL-2.1+, KSyntaxHighlighting MIT,
ink-stroke-modeler Apache-2.0, SoundTouch LGPL-2.1, Hunspell LGPL-2.1 (of its
MPL/GPL/LGPL tri-license), SCOWL word lists (permissive SCOWL notice),
LanguageTool rule data LGPL-2.1, miniz MIT, psd_sdk BSD-2-Clause, stb_image MIT,
Sparkle / WinSparkle MIT, Phosphor MIT, the Geist / Geist Mono / Aspekta / PT Serif / Inter /
Lora / JetBrains Mono typefaces SIL OFL 1.1, SQLite public domain). The same texts ship inside the app
(`Contents/Resources/Licenses` on macOS, `Licenses\` beside the exe on
Windows) and are reproduced at <https://minnotes.app/licenses/>.
[`LICENSES/THIRD_PARTY_NOTICES.txt`](LICENSES/THIRD_PARTY_NOTICES.txt) is the
index plus a categorised summary of the Qt-bundled support libraries.
