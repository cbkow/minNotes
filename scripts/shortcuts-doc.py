#!/usr/bin/env python3
"""Regenerate docs/shortcuts.md from the app's shortcut catalogue (app/qml/MinNotes/App/ShortcutCatalog.js).

The catalogue is the single source: the in-app sheet (Help ▸ Keyboard Shortcuts…, ⌘/) renders it, and this script
writes the docs page from the same list. Run after editing the catalogue:  scripts/shortcuts-doc.py
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "app/qml/MinNotes/App/ShortcutCatalog.js"
DST = ROOT / "docs/shortcuts.md"

HEAD = """---
title: Keyboard Shortcuts
permalink: /shortcuts/
nav_order: 14
---

# Keyboard Shortcuts

macOS uses **⌘** (Command); on Windows use **Ctrl** in its place. **⇧** is
Shift, **⌥** is Alt/Option. The same list is in the app under Help ▸ Keyboard
Shortcuts (⌘/).
"""


def main() -> int:
    text = SRC.read_text(encoding="utf-8")
    m = re.search(r"// CATALOG-BEGIN\s*var sections = (\[.*?\])\s*// CATALOG-END", text, re.S)
    if not m:
        print("catalogue markers not found", file=sys.stderr)
        return 1
    sections = json.loads(m.group(1))
    out = [HEAD]
    for s in sections:
        out.append(f"\n## {s['title']}\n")
        if s.get("note"):
            out.append(f"\n{s['note']}\n")
        cols = s.get("cols", ["Action", "Shortcut"])
        out.append(f"\n| {cols[0]} | {cols[1]} |\n|---|---|\n")
        for row in s["rows"]:
            a, k = row[0], row[1]
            if len(row) > 2:
                a = row[2]   # the docs' own markdown for the action (the markdown-trigger section)
            out.append(f"| {a} | {k} |\n")
    DST.write_text("".join(out), encoding="utf-8")
    print(f"wrote {DST.relative_to(ROOT)}: {len(sections)} sections, {sum(len(s['rows']) for s in sections)} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
