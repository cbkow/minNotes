// Theme — central design tokens, singleton. `Theme.colors.text` etc. from any
// QML file. Dark, flat, blue-accent — matching the ufb / MinRender family.
pragma Singleton
import QtQuick

QtObject {
    id: theme

    readonly property QtObject colors: QtObject {
        // ── Tonal ladder (product pass 2026-07-11, mirrors QCView's
        // Resolve-lean system): recess < page < card < raised < hover.
        // Tone separates planes; borders only where same-tone surfaces meet.
        readonly property color bg:          "#121211"   // window shell
        readonly property color bgAlt:       "#181817"   // the "desk" right of the page (the overflow margin field)
        readonly property color bgStrip:  "#151514"   // the two tab rows: halfway between the shell (bg) and the field (bgAlt) — user ruling 2026-10-18
        readonly property color bgAlt2:      "#1d1d1c"   // desk zebra stripe — every other block's span on the desk
        readonly property color surface:     "#121211"   // the document "page"
        readonly property color surfaceRecess: "#0e0e0e" // inset wells — inputs, code, thumbnail beds, tooltip
        readonly property color card:        "#1a1a19"   // content cards on the page/panels (kanban, notes, threads)
        readonly property color surfaceHover: "#252525"  // flat-button hover fill
        readonly property color surfaceRaised: "#202020" // raised panels (inspector, transport, tab strip)
        readonly property color codeBg:      "#0e0e0e"   // code block fill — a recessed well (was #121212, invisible against the page)
        readonly property color border:      "#2a2a2a"
        readonly property color borderStrong: "#333333"  // a border that must read on its own (QCView token, 2026-10-21)
        readonly property color divider:     "#333333"   // hr / separators / the SELECTION fill
        readonly property color menuHover:   "#3a3a3a"   // the app menu's highlighted row (user 2026-10-22: a LIGHTER grey — surfaceHover #252525 vanished on the raised #202020)
        readonly property color frameNeutral: "#4a4a4a"  // an untinted frame's grey (user 2026-10-22: a dark grey, not the muted text tone)
        // Raised affordances (QCView's affordanceIdle / affordanceHover, adopted 2026-10-21 for the window's own
        // chrome — the title bar's dot, segments and caption buttons): a step up from surfaceHover on hover.
        readonly property color affordanceIdle:  "#262626"
        readonly property color affordanceHover: "#323232"
        // The idle end of a hover RAMP: the hover grey at zero alpha. "transparent" is BLACK at zero alpha, so a
        // ColorAnimation from it passes through a dark tint before the grey — "hover goes dark then light"
        // (user, 2026-10-21). Ramp alpha only.
        readonly property color hoverIdle:        Qt.rgba(0x25 / 255, 0x25 / 255, 0x25 / 255, 0)
        readonly property color affordanceIdleClear: Qt.rgba(0x32 / 255, 0x32 / 255, 0x32 / 255, 0)

        readonly property color text:        "#E4E3E2"   // body (default type colour)
        readonly property color textBright:  "#f0f0f0"   // headings / selected chrome (sub-white: no glow on dark, matches the highlighter's light text)
        readonly property color textMuted:   "#8a8a8a"   // quotes, bullets
        readonly property color textSubtle:  "#5e5e5e"
        readonly property color codeText:    "#d4d4e8"
        // Inline code chip — a touch LIGHTER than the page so it reads as raised
        // (the code-block fill is darker); distinct from surrounding prose. Text
        // in the family blue rather than the default lavender.
        readonly property color inlineCodeBg:   "#1d2733"   // blue-tinted dark chip
        readonly property color inlineCodeText: "#4aa8ff"   // accent-family blue

        readonly property color accent:      "#0189f1"   // caret / primary — SEMANTIC moments only (live/insertion/link), never "toggle is on"
        readonly property color accentHover: "#1b95f1"   // primary button hover
        readonly property color accentMuted: "#10395b"   // primary-adjacent fills (NOT the default checked state — that's divider)
        readonly property color error:       "#c04040"   // danger button
        readonly property color success:     "#4cb050"   // toast/outcome voice (family value, from QCView)
        readonly property color warn:        "#f5a623"   // toast/outcome voice
        readonly property color selectionBg: "#2a568c"   // text-selection fill (saturated cobalt)
        // Opaque-block range membership (media/table/divider inside a document
        // selection): selectionBg as a translucent wash OVER the block.
        readonly property color selectionWash: Qt.rgba(0.165, 0.337, 0.549, 0.45)
        // Multi-block selection: a faint full-row band under EVERY selected
        // row, so a range that spans blocks reads at block grain.
        readonly property color selectionBand: Qt.rgba(0.165, 0.337, 0.549, 0.16)
        // Spell / grammar underlines: 4×2 dotted tiles (2 px on, 2 px off) as
        // embedded PNGs — Image.Tile them under the text baseline. Squared, no
        // wave (the corner rule). Red = spelling, the quote-bar blue = grammar.
        readonly property string squiggleSpelling: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAQAAAACCAYAAAB/qH1jAAAAE0lEQVR42mO44OHxGoQZYABdAADAHAktZzCF/wAAAABJRU5ErkJggg=="
        readonly property string squiggleGrammar:  "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAQAAAACCAYAAAB/qH1jAAAAE0lEQVR42mNwy1vwGoQZYABdAAC4jAj928XigAAAAABJRU5ErkJggg=="
        readonly property color quoteBar:    "#3a5e86"
        // QCView note markers — the SAME violet QCView uses on its timeline
        // pins, so "a QCView note" is one color across both apps.
        readonly property color noteMarker:  "#b48ef0"
    }

    readonly property QtObject dim: QtObject {
        readonly property int padding:        8
        readonly property int paddingLoose:   16
        // The humanist pass (user ruling 2026-10-21 — minNotes leaves the family's squared look on purpose): three radii.
        readonly property int radiusSmall:    3    // buttons, segments, chips, inputs, menu rows, scrollbar thumbs, handles
        readonly property int radius:         6    // popups, flyouts, menus, dialogs, toasts, cards, lanes, media frames
        readonly property int radiusLarge:    10   // reserved (nothing yet) — so a fourth value is never invented ad hoc
        readonly property int hoverMs:        100  // the hover / pressed colour ramp
        readonly property int popupMs:        80   // a popup's fade-and-rise
        readonly property int scrollBarWidth: 14   // macOS always-on scrollbar width
        readonly property int columnWidth:    760    // the readable-measure CAP (2.0 stage 1C: the page follows the window under it)
        readonly property int docTopPad:      28     // breathing room above block 0
        readonly property int toolStripHeight: 34    // flat-button row height / rail width
        readonly property int rulerHeight: 22        // the Inspector's top bar (was the PageRuler's band);
                                                     // the Inspector's top bar matches it (2026-08-20)
        // The three top rows (2.0 chrome re-organisation, user ruling 2026-10-18): files · components · toolbar.
        readonly property int tabRowH:        30     // the PATH BAR (== the toolbar row; the reduced tab size went with the tab strips, 2026-10-18)
        readonly property int toolbarH:       30     // the toolbar row
        readonly property int chromeBtn:      28     // a toolbar button's edge (inside the 30-px row)
        readonly property int chromeIcon:     16     // a toolbar button's icon
    }

    // ── Fonts (bundled, loaded via FontLoader so the family resolves the same
    // on every OS). Text = Inter, mono = JetBrains Mono, icons = Phosphor. ──
    readonly property FontLoader interFont:           FontLoader { source: Qt.resolvedUrl("fonts/Inter_18pt-Regular.ttf") }
    readonly property FontLoader interBoldFont:       FontLoader { source: Qt.resolvedUrl("fonts/Inter_18pt-Bold.ttf") }
    readonly property FontLoader interItalicFont:     FontLoader { source: Qt.resolvedUrl("fonts/Inter_18pt-Italic.ttf") }
    readonly property FontLoader interBoldItalicFont: FontLoader { source: Qt.resolvedUrl("fonts/Inter_18pt-BoldItalic.ttf") }  // so bold+italic has a real face
    readonly property FontLoader monoFontLoader:  FontLoader { source: Qt.resolvedUrl("fonts/JetBrainsMono-Regular.ttf") }
    // GEIST TRIAL (2026-10-23): one sans + one mono for EVERY role — chrome, document body, paragraphs
    // (the serif trial yields), code. Geist 1.8.2 (Vercel, OFL), static faces so bold / italic / bold-italic
    // resolve to real cuts. Flip `geist` to false to get Inter / Aspekta / PT Serif / JetBrains Mono back.
    readonly property bool geist: true
    readonly property FontLoader geistFont:           FontLoader { source: Qt.resolvedUrl("fonts/Geist-Regular.ttf") }
    readonly property FontLoader geistBoldFont:       FontLoader { source: Qt.resolvedUrl("fonts/Geist-Bold.ttf") }
    readonly property FontLoader geistItalicFont:     FontLoader { source: Qt.resolvedUrl("fonts/Geist-Italic.ttf") }
    readonly property FontLoader geistBoldItalicFont: FontLoader { source: Qt.resolvedUrl("fonts/Geist-BoldItalic.ttf") }
    // The weight trial (2026-10-23): Light for headings, Medium for body copy — the cuts in between.
    readonly property FontLoader geistLightFont:        FontLoader { source: Qt.resolvedUrl("fonts/Geist-Light.ttf") }
    readonly property FontLoader geistLightItalicFont:  FontLoader { source: Qt.resolvedUrl("fonts/Geist-LightItalic.ttf") }
    readonly property FontLoader geistMediumFont:       FontLoader { source: Qt.resolvedUrl("fonts/Geist-Medium.ttf") }
    readonly property FontLoader geistMediumItalicFont: FontLoader { source: Qt.resolvedUrl("fonts/Geist-MediumItalic.ttf") }
    readonly property FontLoader geistSemiBoldFont:     FontLoader { source: Qt.resolvedUrl("fonts/Geist-SemiBold.ttf") }
    // Qt files every cut under the one family "Geist" (verified 2026-10-23), so a weight number picks the cut.
    // `--heading-weight=N` / `--body-weight=N` (300 … 700) override the trial's picks without a rebuild.
    readonly property string headingWeightArg: { const a = Qt.application.arguments.find(function(x) { return x.indexOf("--heading-weight=") === 0 }); return a ? a.substring(17) : "" }
    readonly property string bodyWeightArg:    { const a = Qt.application.arguments.find(function(x) { return x.indexOf("--body-weight=") === 0 }); return a ? a.substring(14) : "" }
    readonly property FontLoader geistMonoFont:           FontLoader { source: Qt.resolvedUrl("fonts/GeistMono-Regular.ttf") }
    readonly property FontLoader geistMonoBoldFont:       FontLoader { source: Qt.resolvedUrl("fonts/GeistMono-Bold.ttf") }
    readonly property FontLoader geistMonoItalicFont:     FontLoader { source: Qt.resolvedUrl("fonts/GeistMono-Italic.ttf") }
    readonly property FontLoader geistMonoBoldItalicFont: FontLoader { source: Qt.resolvedUrl("fonts/GeistMono-BoldItalic.ttf") }
    // Aspekta (document body). The static 400/700 faces share the legacy family
    // "Aspekta 400" (Regular + Bold), so font.bold resolves the 700 face — needed
    // for headings. No italic face: italic body text synthesizes (oblique).
    readonly property FontLoader aspektaFont:     FontLoader { source: Qt.resolvedUrl("fonts/Aspekta-400.ttf") }
    readonly property FontLoader aspektaBoldFont: FontLoader { source: Qt.resolvedUrl("fonts/Aspekta-700.ttf") }
    // Lora (serif) for quote blocks — all four faces so bold/italic/
    // bold-italic inside a quote render with a real face, not a fallback.
    // TRIAL (2026-10-18): `--serif-font=<file.ttf>` loads another serif (its italic / bold files beside it, named
    // <stem>-Italic / <stem>-Bold, are loaded when present); `--serif-font=<Family Name>` uses an installed family.
    readonly property string serifArg: { const a = Qt.application.arguments.find(function(x) { return x.indexOf("--serif-font=") === 0 }); return a ? a.substring(13) : "" }
    readonly property bool serifArgIsFile: /\.(ttf|otf)$/i.test(serifArg)
    readonly property string serifStem: serifArgIsFile ? serifArg.replace(/(-Regular)?(\[[^\]]*\])?\.(ttf|otf)$/i, "") : ""
    readonly property FontLoader serifFont:           FontLoader { source: theme.serifArgIsFile ? "file://" + theme.serifArg : Qt.resolvedUrl("fonts/Lora-Regular.ttf") }
    readonly property FontLoader serifBoldFont:       FontLoader { source: theme.serifArgIsFile ? "file://" + theme.serifStem + "-Bold.ttf" : Qt.resolvedUrl("fonts/Lora-Bold.ttf") }
    readonly property FontLoader serifItalicFont:     FontLoader { source: theme.serifArgIsFile ? "file://" + theme.serifStem + "-Italic" + (theme.serifArg.indexOf("[") >= 0 ? theme.serifArg.substring(theme.serifArg.indexOf("[")) : ".ttf") : Qt.resolvedUrl("fonts/Lora-Italic.ttf") }
    readonly property FontLoader serifBoldItalicFont: FontLoader { source: theme.serifArgIsFile ? "" : Qt.resolvedUrl("fonts/Lora-BoldItalic.ttf") }
    // PT Serif (OFL) — the body-copy serif trial's alternative (2026-10-18), all four faces.
    readonly property FontLoader ptSerifFont:           FontLoader { source: Qt.resolvedUrl("fonts/PTSerif-Regular.ttf") }
    readonly property FontLoader ptSerifBoldFont:       FontLoader { source: Qt.resolvedUrl("fonts/PTSerif-Bold.ttf") }
    readonly property FontLoader ptSerifItalicFont:     FontLoader { source: Qt.resolvedUrl("fonts/PTSerif-Italic.ttf") }
    readonly property FontLoader ptSerifBoldItalicFont: FontLoader { source: Qt.resolvedUrl("fonts/PTSerif-BoldItalic.ttf") }
    readonly property string serifSizeArg: { const a = Qt.application.arguments.find(function(x) { return x.indexOf("--serif-size=") === 0 }); return a ? a.substring(13) : "" }   // trial
    readonly property FontLoader phosphorFont:         FontLoader { source: Qt.resolvedUrl("fonts/Phosphor.ttf") }
    readonly property FontLoader phosphorFillFont:     FontLoader { source: Qt.resolvedUrl("fonts/Phosphor-Fill.ttf") }
    readonly property FontLoader phosphorDuotoneFont:  FontLoader { source: Qt.resolvedUrl("fonts/Phosphor-Duotone.ttf") }
    readonly property FontLoader phosphorThinFont:     FontLoader { source: Qt.resolvedUrl("fonts/Phosphor-Thin.ttf") }

    readonly property QtObject icon: QtObject {
        readonly property string family:        theme.phosphorFont.name
        readonly property string familyFill:    theme.phosphorFillFont.name
        readonly property string familyDuotone: theme.phosphorDuotoneFont.name
        readonly property string familyThin:    theme.phosphorThinFont.name
        readonly property int sizeSmall:   14
        readonly property int sizeToolbar: 18
        readonly property int sizeMedium:  20
        readonly property int sizeLarge:   24
    }

    readonly property QtObject font: QtObject {
        readonly property string family: theme.geist ? theme.geistFont.name : theme.interFont.name        // Inter (chrome) — Geist in the trial
        readonly property string body:   theme.geist ? theme.geistFont.name : theme.aspektaFont.name      // Aspekta (document body) — Geist in the trial
        readonly property string mono:   theme.geist ? theme.geistMonoFont.name : theme.monoFontLoader.name   // JetBrains Mono (bundled) — Geist Mono in the trial
        readonly property string serif:  theme.serifArg !== "" && !theme.serifArgIsFile ? theme.serifArg
                                       : theme.serifArg === "lora" ? theme.serifFont.name : theme.ptSerifFont.name   // PT Serif (the 2026-10-18 pick) — Lora via --serif-font=lora, or the trial's --serif-font
        readonly property int sizeSerif: theme.serifSizeArg !== "" ? parseInt(theme.serifSizeArg) : 15   // body paragraphs in the serif (PT Serif's x-height wants a point over the sans's 14)
        readonly property bool bodySerif: !theme.geist               // TRIAL 2026-10-18: body copy (paragraphs, lists, tasks) in the serif; headings / quotes / cells / chrome stay sans. OFF in the Geist trial (one sans everywhere)
        // The weight trial (2026-10-23): headings THIN (Light 300), body copy MEDIUM (500); chrome stays Regular,
        // code stays Regular (Geist Mono ships only its Regular / Bold cuts here), table header cells stay Bold.
        readonly property int headingWeight: theme.headingWeightArg !== "" ? parseInt(theme.headingWeightArg) : (theme.geist ? Font.Light : Font.Bold)
        readonly property int bodyWeight:    theme.bodyWeightArg !== "" ? parseInt(theme.bodyWeightArg) : (theme.geist ? Font.Medium : Font.Normal)
        readonly property int sizeSmall: 12  // captions
        readonly property int sizeChrome: 13 // panel/toolbar labels (was a bare literal ~20 places)
        readonly property int sizeBody: 14   // a notch above the 13px kanban/tab tier
        readonly property int sizeMono: 13
    }
}
