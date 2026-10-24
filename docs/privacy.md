---
title: Privacy
permalink: /privacy/
nav_order: 15
---

# Privacy

minNotes runs entirely on your machine.

- **No accounts, no telemetry, no analytics.** The app never phones home
  about you or your files.
- **Your files are plain files.** A file is a standard SQLite database on
  your disk or share; its media sits in an ordinary folder beside it.
  Nothing is uploaded anywhere.
- **Network access** is limited to things you can see: on macOS, the
  update check (Sparkle fetching the appcast from `minnotes.app`);
  downloading an image when you paste a remote image URL; and — when you
  drop or type a link on the canvas — one request to that page for its
  title and icon. Nothing is fetched again until you ask for a refresh.
- **On Windows, the Microsoft Store** installs and updates minNotes; the app
  itself makes no update requests there. The Store's own handling is covered
  by Microsoft's privacy statement.
- **Spelling and grammar checking run fully on your machine.** The
  dictionary and the rule set ship inside the app; no text ever leaves it.
- **What stays on your machine:** your preferences, a custom dictionary
  of words you added, and temporary working copies of the files you have
  open, kept in the app's own data folder. A Windows Store install keeps it
  in the app's private storage, which uninstalling removes; a folder left
  by the old Windows installer stays until you delete it
  (`%APPDATA%\minNotes`).
- **Exports are yours.** HTML, Markdown, Word, PDF, canvas and package
  exports are written to the location you choose and shared only by you.
  A browser viewing an HTML export remembers its own view settings
  (readable width, checked tasks) in that browser alone.

minNotes is free software — the complete source is at
[github.com/cbkow/minNotes](https://github.com/cbkow/minNotes) under
GPL-3.0-or-later, so all of the above is verifiable. Questions:
[open an issue](https://github.com/cbkow/minNotes/issues).
