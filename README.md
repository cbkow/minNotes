# minNotes

A fast, block-based notes editor for macOS and Windows. Documents are SQLite
databases (`.mndb`) with a flat list of blocks; the editor is a virtualized Qt
Quick surface that owns the document model directly.

## What's in the box

| Surface | Stack |
|---|---|
| App shell, editor, dialogs, menus | Qt 6.11 / QML / Qt Quick Controls 2 |
| Document store (`.mndb`), blocks, undo | SQLite (C++ `app/core/`) |
| Block types | paragraphs, headings, code (syntax-highlighted), quotes, task/choice lists, tables + kanban, dividers |
| Media | inline images, video (decode + scrub + annotate), inline PDF, file attachments |
| Ink | sketch blocks + on-video annotations (QCView-interop), ink-stroke-modeler smoothing |
| Cross-OS | OS-neutral path mappings for referenced media, multi-document tabs |
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
- **ink-stroke-modeler** — fetched automatically by CMake (FetchContent).

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
app/core/    C++ document model: BlockModel, Document (SQLite), MediaStore,
             DocumentManager, PathMap, image/PDF/video providers
app/notes/   ink stroke engine (sketch + video annotations)
app/qml/     QML UI tree (Main, Editor, MediaBlock, tables, dialogs, …)
external/    vendored deps (ffmpeg, kf6, Sparkle, aspekta font) + build-ksyntax.sh
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
ink-stroke-modeler Apache-2.0, Sparkle / WinSparkle MIT, Phosphor MIT, Aspekta
SIL OFL, SQLite public domain).
[`LICENSES/THIRD_PARTY_NOTICES.txt`](LICENSES/THIRD_PARTY_NOTICES.txt) is the
index plus a categorised summary of the Qt-bundled support libraries.
