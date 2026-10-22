// The shortcut catalogue (2026-10-21): ONE list that the in-app sheet (ShortcutsSheet.qml, Help ▸ Keyboard
// Shortcuts… / ⌘/) renders and that scripts/shortcuts-doc.py turns into docs/shortcuts.md — edit HERE, then run the
// script; never edit the docs page by hand. Keys are written in macOS glyphs (⌘ ⇧ ⌥); `display()` rewrites them for
// Windows. The literal between the CATALOG markers is strict JSON (the script json-parses it). A row is
// [action, keys] — the markdown-trigger section's rows carry a third entry, the docs' code-formatted action (the
// trailing space is the trigger), which the sheet ignores.
.pragma library

// CATALOG-BEGIN
var sections = [
  { "title": "Files", "rows": [
    ["New file", "⌘N"],
    ["Open…", "⌘O"],
    ["Save", "⌘S"],
    ["Save As…", "⌘⇧S"],
    ["Close file", "⌘W"],
    ["Find in file (Edit ▸ Find in File…: pages, cards, PDFs, video notes)", "⌘F"],
    ["Walk the hits · jump · close", "↑ / ↓ · Return · Esc"],
    ["Keyboard shortcuts (this sheet)", "⌘/"]
  ]},
  { "title": "View", "rows": [
    ["Zoom in / out (the page, a PDF, or the canvas — whichever is in front)", "⌘+ / ⌘−"],
    ["Zoom to 100%", "⌘0"],
    ["In a page: toggle Full Width · on the canvas: fit everything", "⌘1"],
    ["Always on Top — the window above every other app's (the toolbar's pin)", "⌥⌘T"],
    ["Back / Forward through the views you visited (canvas, page, frame, file) — also the chevrons left of the path and the mouse's back / forward buttons", "⌥⌘← / ⌥⌘→"],
    ["Zoom about the pointer", "⌘-wheel, or pinch"],
    ["Pan the canvas with the hand", "hold Space and drag"],
    ["Pan anywhere", "middle-button drag"]
  ]},
  { "title": "Canvas", "rows": [
    ["Select / Hand / Page tools", "V / H / N"],
    ["Text box · plain text", "T · ⇧T"],
    ["Pen / Rectangle / Oval / Line / Arrow / Eraser", "P / R / O / L / A / E"],
    ["Open the selected element — the page view, the studio, the PDF view, an image's lightbox", "double-click · Return · the open glyph"],
    ["Preview the selected image, video, PDF or audio in the lightbox (‹ › or ← / → step through them; PageUp / PageDown page a PDF)", "Space or Return (Space or Esc closes)"],
    ["Cancel a stroke → drop the tool → clear the selection", "Esc (repeat)"],
    ["Delete the selection", "Delete / Backspace"],
    ["Add / remove one from the selection", "⇧Click"],
    ["Select several (marquee)", "drag on empty canvas"],
    ["Paste files, an image, text or a link as cards", "⌘V"],
    ["Page a selected PDF", "PageUp / PageDown"],
    ["Step the selection forward / back one layer", "⌘] / ⌘["],
    ["Bring to front / send to back", "⌥⌘] / ⌥⌘["]
  ]},
  { "title": "PDF View (full-window)", "rows": [
    ["Pen / Rectangle / Oval / Line / Arrow / Eraser (strokes stay on that page)", "P / R / O / L / A / E"],
    ["Back to the pointer", "V"],
    ["Scroll with the hand while a tool is armed", "hold Space and drag"],
    ["Undo / redo a stroke", "⌘Z / ⌘⇧Z"],
    ["Drop the tool → back to the canvas", "Esc (repeat)"]
  ]},
  { "title": "Editing", "rows": [
    ["Undo (the page's or the canvas's history)", "⌘Z"],
    ["Redo", "⌘⇧Z or ⌘Y"],
    ["Cut / Copy / Paste", "⌘X / ⌘C / ⌘V"],
    ["Copy as Markdown", "⌘⇧C"],
    ["Insert a choice chip (To do / Doing / Done)", "⌥⌘C"],
    ["Indent / outdent a list item", "Tab / ⇧Tab, or ⌘] / ⌘["],
    ["Next / previous lane (in a split row)", "Tab / ⇧Tab"],
    ["New line in a code block (Enter on a blank last line exits)", "⇧Enter"],
    ["Step out of the page to the canvas (nothing selected)", "Esc"]
  ]},
  { "title": "Inline Formatting", "rows": [
    ["Bold", "⌘B"],
    ["Italic", "⌘I"],
    ["Underline", "⌘U"],
    ["Strikethrough", "⌘⇧X"],
    ["Inline code", "⌘E"],
    ["Link", "⌘K"],
    ["Highlight (toggle the armed colour)", "⌘⇧H"],
    ["Clear formatting", "⌘\\"]
  ]},
  { "title": "Tables", "rows": [
    ["Next / previous cell (adds a row past the last cell)", "Tab / ⇧Tab"],
    ["Down the column (the last row adds one; an empty last row leaves the table)", "Enter"],
    ["Insert a row below", "⌘Enter"],
    ["Cycle a checkmark cell", "Space"],
    ["Select the block → its cell → the table → the page", "⌘A (repeat)"],
    ["Whole row or column (on a grip)", "Click"],
    ["Span of rows or columns (on a grip)", "⇧Click"],
    ["Add / remove a row or column (on a grip)", "⌘Click"],
    ["Copy a grip selection as a table", "⌘C"],
    ["Clear a grip selection's contents", "Backspace / Delete"],
    ["A selected row → the whole table → the caret below it", "Esc"]
  ]},
  { "title": "Video (the studio, an inline video with the caret, or a selected card)", "rows": [
    ["Play / pause (Space too, in the studio — elsewhere Space is the lightbox)", "K, W or S"],
    ["Preview an inline video or image in the lightbox", "Space (with the caret on it)"],
    ["Step a frame back / forward", "← / → (or Q / E)"],
    ["Rewind / fast-forward (hold)", "J / L (or A / D)"],
    ["Jump to start / end", "Home / End"],
    ["Cycle review speed (0.5×–2×)", "R"],
    ["Reset speed to 1×", "⇧R"],
    ["Loop", "V"],
    ["Mute", "M"],
    ["Volume", "↑ / ↓"],
    ["Timecode ↔ frames", "click the counter"]
  ]},
  { "title": "In the Page (markdown-style)", "cols": ["Type", "Becomes"], "mono": true,
    "note": "Typed at the start of a block — see [Editing](editing.md):", "rows": [
    ["# … ######", "Heading 1–6", "`# ` … `###### `"],
    [">", "Quote", "`> `"],
    ["- / * / +", "Bullet list", "`- ` / `* ` / `+ `"],
    ["1.", "Numbered list", "`1. `"],
    ["- [ ]  /  - [/]  /  - [x]", "Task: to-do / doing / done", "`- [ ] ` / `- [/] ` / `- [x] `"],
    ["``` + Enter", "Code block", "```` ``` ```` + `Enter`"],
    ["--- + Enter", "Divider", "`---` + `Enter`"]
  ]}
]
// CATALOG-END

// The keys for this platform: macOS glyphs as written; Windows spells the modifiers out (⌘ → Ctrl, ⌥ → Alt,
// ⇧ → Shift), "+"-joined before a key, bare before a "-" ("⌘-wheel" → "Ctrl-wheel") or a "+ / −" zoom pair.
function display(keys, mac) {
    if (mac) return keys
    return keys.replace(/[⌘⌥⇧](?=[⌘⌥⇧])/g, function(g) { return name(g) + "+" })
               .replace(/[⌘⌥⇧](?=-)/g, function(g) { return name(g) })
               .replace(/[⌘⌥⇧](?=[+−])/g, function(g) { return name(g) + " " })
               .replace(/[⌘⌥⇧]/g, function(g) { return name(g) + "+" })
}
function name(g) { return g === "⌘" ? "Ctrl" : g === "⌥" ? "Alt" : "Shift" }
