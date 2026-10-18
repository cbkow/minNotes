---
title: Installation
permalink: /installation/
nav_order: 2
---

# Installation

Download the latest release from the
[releases page](https://github.com/cbkow/minNotes/releases/latest):

| Platform | File |
|---|---|
| macOS 13+ (Apple Silicon) | `minNotes-<version>-macOS.dmg` |
| Windows 10/11 (x64) | `minNotes-<version>-x64.exe` |

## macOS

Open the `.dmg` and drag **minNotes** to Applications. The app is signed
and notarized — it launches without warnings, online or off.

> **Updating from 1.0.3:** the macOS 1.0.3 build shipped without its
> updater, so it cannot offer you 2.0. Download the `.dmg` and replace the
> app by hand once; updates work normally from 2.0 on.

## Windows

Run the installer and follow the prompts.

> Windows may show a SmartScreen prompt on first run — the installer is not
> Authenticode-signed. Choose **More info → Run anyway**.

## Updates

minNotes checks for updates automatically (Sparkle on macOS, WinSparkle on
Windows) and offers new versions in-app. You can also check manually from
the app menu.

## Upgrading from 1.x

minNotes 2.0 changes the file format. A 1.x file opens as a canvas with
one page holding the document, read-only, and **Save As** writes a 2.0
file beside it; the original is never modified. 1.x builds cannot open 2.0
files — see [Files & Pages](basics.md#opening-10-files).

## File Types

The installer registers two file types and a URL scheme:

| Type | What it is |
|---|---|
| `.mnd` | A file — a plain SQLite database you can store anywhere |
| `.mnpkg` | A sealed [package](packages.md) with all assets inside |
| `minnotes://` | Deep links that open a file from other apps |

Double-clicking either file opens it in minNotes.
