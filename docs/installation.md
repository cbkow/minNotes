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


## Windows

Run the installer and follow the prompts.


> Windows may show a SmartScreen prompt on first run — the installer is not
> Authenticode-signed. Choose **More info → Run anyway**.

## Updates

minNotes checks for updates automatically (Sparkle on macOS, WinSparkle on
Windows) and offers new versions in-app. You can also check manually from
the app menu.

## File Types

The installer registers two file types and a URL scheme:

| Type | What it is |
|---|---|
| `.mndb` | A note — a plain SQLite file you can store anywhere |
| `.mnpkg` | A sealed [package](packages.md) with all assets inside |
| `minnotes://` | Deep links that open a note from other apps |

Double-clicking either file opens it in minNotes.
