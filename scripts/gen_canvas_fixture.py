#!/usr/bin/env python3
"""Generate tests/fixtures/canvas-probe.md (canvas B10): mixed headings, dividers (row 0, adjacent to a
heading, doubled, closing a heading-less run), tables, code, lists, and a heading-less 1500-block tail
(one card past the row caps). Run from the repo root."""
import os
out = ["---", ""]
for i in range(40):
    out += [f"## Section {i}: canvas probe", "",
            f"Paragraph one of section {i} with **bold**, *italic*, `code` and a [link](https://example.com) so the overlays exist.", "",
            f"Paragraph two of section {i} is plain and a little longer so that it wraps onto a second line at the page width.", ""]
    if i % 3 == 0:
        out += ["| Item | Owner | Status | Done |", "|---|---|---|---|"]
        out += [f"| Item {i}-{r} | Owner {r} | Status {r % 3} | [{'x' if r % 2 else ' '}] |" for r in range(4)]
        out += [""]
    if i % 4 == 0:
        out += ["```python", f"def section_{i}(samples):", "    return sorted(samples)[len(samples) // 2]", "```", ""]
    if i % 5 == 0:
        out += [f"- [ ] Task {i}.1", f"- [x] Task {i}.2", "1. First", "2. Second", "> A quote", ""]
    if i == 7:
        out += ["---", ""]                       # a divider right before the next heading: no empty card
    if i == 12:
        out += ["---", "", "A heading-less run after a divider.", "", "Its second paragraph.", "", "---", "", "---", ""]   # closed runs, a doubled divider
out += ["---", ""]
out += [f"Tail paragraph {k} of the heading-less run." + ("" if k % 7 else " It wraps onto a second line because it is deliberately a little longer than the others.") for k in range(1500)]
out = [l for pair in zip(out[-1500:], [""] * 1500) for l in pair] if False else out
text = "\n".join(out[:-1500]) + "\n" + "\n\n".join(out[-1500:]) + "\n"
path = os.path.join(os.path.dirname(__file__), "..", "tests", "fixtures", "canvas-probe.md")
open(path, "w").write(text)
print(path, text.count("\n"), "lines")
