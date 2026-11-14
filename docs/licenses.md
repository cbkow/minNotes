---
title: Licenses & Credits
permalink: /licenses/
nav_order: 16
---

# Licenses & Credits

## minNotes

minNotes is free software under the
[GNU General Public License, version 3](https://github.com/cbkow/minNotes/blob/main/LICENSE).
You may use it, share it and change it under that licence's terms. It comes
with no warranty.

The source of minNotes 3 is in the
[minplatform repository](https://github.com/cbkow/minplatform) (`apps/mn` and
the crates beside it). The source of minNotes 2 is on the
[`2.x` branch](https://github.com/cbkow/minNotes/tree/2.x) of the minNotes
repository.

## The full texts are in the application

minNotes ships with a page of third-party notices: every component below and
every Rust crate built into the application, each with the full text of its
licence. Choose **minNotes > Licenses…** on macOS, or **Help > Licenses…** on
Windows, to open it.

That page is generated for each release from what is built into it, so where
it and this page differ, the page in the application is the one that is
right.

## Third-party components

| Component | What it does in minNotes | Licence | Platform |
|---|---|---|---|
| [FFmpeg](https://ffmpeg.org/) | Reads video and sound; as a program beside the application, makes the videos of a web folder | GPL-3.0-or-later (as built) | macOS, Windows |
| [dav1d](https://code.videolan.org/videolan/dav1d) | An AV1 decoder, built into FFmpeg | BSD-2-Clause | macOS |
| [SRT](https://github.com/Haivision/srt) | Built into FFmpeg, unmodified | MPL-2.0 | macOS |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | Built into FFmpeg, for SRT | Apache-2.0 | macOS |
| [OpenEXR](https://openexr.com/) | Reads EXR pictures | BSD-3-Clause | macOS, Windows |
| [Imath](https://imath.readthedocs.io/) | Used by OpenEXR | BSD-3-Clause | macOS, Windows |
| [libdeflate](https://github.com/ebiggers/libdeflate) | Used by OpenEXR | MIT | macOS, Windows |
| [OpenJPH](https://github.com/aous72/OpenJPH) | Used by OpenEXR | BSD-2-Clause | macOS, Windows |
| [Zstandard](https://facebook.github.io/zstd/) | Used by OpenEXR | BSD-3-Clause | macOS, Windows |
| [zlib](https://zlib.net/) | Compression | Zlib | macOS, Windows |
| [libjpeg-turbo](https://libjpeg-turbo.org/) | A JPEG library | IJG, BSD-3-Clause and Zlib | macOS, Windows |
| [LibTIFF](https://libtiff.gitlab.io/libtiff/) | A TIFF library | libtiff | macOS, Windows |
| [XZ Utils (liblzma)](https://tukaani.org/xz/) | Compression | 0BSD | Windows |
| [bzip2](https://sourceware.org/bzip2/) | Compression | bzip2-1.0.6 | Windows |
| [libspng](https://libspng.org/) | A PNG library | BSD-2-Clause | Windows |
| [Sparkle](https://sparkle-project.org/) | The updater | MIT, with bundled components under their own terms | macOS |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | Compiles shaders for Direct3D | NCSA (the LLVM release licence) and MIT | Windows |
| [Microsoft Visual C++ Runtime](https://visualstudio.microsoft.com/license-terms/) | Runtime libraries, redistributed unmodified | Microsoft Software License Terms | Windows |
| [minColor](https://github.com/cbkow/minplatform), after OpenDRT by Jed Smith and AgX by Troy Sobotka | The colour stage for HDR and linear video | GPL-3.0 | macOS, Windows |
| [Geist and Geist Mono](https://vercel.com/font) | The typefaces of the interface, of notes and of code | SIL Open Font License 1.1 | macOS, Windows |
| [Phosphor Icons](https://phosphoricons.com/) | The icons | MIT | macOS, Windows |

libjpeg-turbo: this software is based in part on the work of the Independent
JPEG Group.

The Windows build of FFmpeg has more libraries built into it than the macOS
build. They are listed one by one, with their licences, in the notices of the
Windows application.

## Rust crates

minNotes is written in Rust and builds in several hundred open-source crates,
among them the ones that store a collection (SQLite, through a crate), draw
the window, read PDFs and write the exports. Their names, versions and
licence texts are in the application's notices page. A release is not built
if a crate carries a licence outside the accepted list.

## Shared with QCView

The notes and drawings you make on a video are kept in the format of
[QCView](https://qcview.com), beside the video, so the two applications read
each other's review notes.
