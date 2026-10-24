---
title: Installation
permalink: /installation/
nav_order: 2
---

# Installation

| Platform | Where |
|---|---|
| macOS 13+ (Apple Silicon) | `minNotes-<version>-macOS.dmg` from the [releases page](https://github.com/cbkow/minNotes/releases/latest) |
| Windows 10/11 (x64) | The [Microsoft Store](https://apps.microsoft.com/detail/9P2S2W9TLL9D) |

## macOS

Open the `.dmg` and drag **minNotes** to Applications. The app is signed
and notarized — it launches without warnings, online or off.

> **Updating from 1.0.3:** the macOS 1.0.3 build shipped without its
> updater, so it cannot offer you 2.0. Download the `.dmg` and replace the
> app by hand once; updates work normally from 2.0 on.

## Windows

Install minNotes from the Microsoft Store. The Store build is signed by
Microsoft, so there is no SmartScreen prompt, and it installs without
administrator rights.

> **Moving from the old installer (2.0.5 and earlier):** Windows releases
> now come only from the Store; the installer builds will not be offered an
> update. Uninstall the old version (**Settings → Apps → minNotes**), then
> install from the Store. Your files are untouched, and your preferences and
> custom dictionary words carry over.

## Updates

On macOS, minNotes checks for updates automatically (Sparkle) and offers
new versions in-app; you can also check manually from the app menu. On
Windows the Microsoft Store delivers updates.

## Upgrading from 1.x

minNotes 2.0 changes the file format. A 1.x file opens as a canvas with
one page holding the document, read-only, and **Save As** writes a 2.0
file beside it; the original is never modified. 1.x builds cannot open 2.0
files — see [Files & Pages](basics.md#opening-10-files).

## File Types

minNotes registers two file types and a URL scheme:

| Type | What it is |
|---|---|
| `.mnd` | A file — a plain SQLite database you can store anywhere |
| `.mnpkg` | A sealed [package](packages.md) with all assets inside |
| `minnotes://` | Deep links that open a file from other apps |

Double-clicking either file opens it in minNotes.
