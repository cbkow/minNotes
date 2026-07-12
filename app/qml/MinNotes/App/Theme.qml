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
        readonly property color surface:     "#121211"   // the document "page"
        readonly property color surfaceRecess: "#0e0e0e" // inset wells — inputs, code, thumbnail beds, tooltip
        readonly property color card:        "#1a1a19"   // content cards on the page/panels (kanban, notes, threads)
        readonly property color surfaceHover: "#252525"  // flat-button hover fill
        readonly property color surfaceRaised: "#202020" // raised panels (inspector, transport, tab strip)
        readonly property color codeBg:      "#0e0e0e"   // code block fill — a recessed well (was #121212, invisible against the page)
        readonly property color border:      "#2a2a2a"
        readonly property color divider:     "#333333"   // hr / separators / the SELECTION fill

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
        readonly property color quoteBar:    "#3a5e86"
        // QCView note markers — the SAME violet QCView uses on its timeline
        // pins, so "a QCView note" is one color across both apps.
        readonly property color noteMarker:  "#b48ef0"
    }

    readonly property QtObject dim: QtObject {
        readonly property int padding:        8
        readonly property int paddingLoose:   16
        readonly property int radius:         0    // squared corners everywhere (user ruling); was 4
        readonly property int scrollBarWidth: 14   // macOS always-on scrollbar width
        readonly property int columnWidth:    760    // prose reading measure (comfortable)
        readonly property int docTopPad:      28     // breathing room above block 0
        readonly property int toolStripHeight: 34    // flat-button row height / rail width
    }

    // ── Fonts (bundled, loaded via FontLoader so the family resolves the same
    // on every OS). Text = Inter, mono = JetBrains Mono, icons = Phosphor. ──
    readonly property FontLoader interFont:           FontLoader { source: Qt.resolvedUrl("fonts/Inter_18pt-Regular.ttf") }
    readonly property FontLoader interBoldFont:       FontLoader { source: Qt.resolvedUrl("fonts/Inter_18pt-Bold.ttf") }
    readonly property FontLoader interItalicFont:     FontLoader { source: Qt.resolvedUrl("fonts/Inter_18pt-Italic.ttf") }
    readonly property FontLoader interBoldItalicFont: FontLoader { source: Qt.resolvedUrl("fonts/Inter_18pt-BoldItalic.ttf") }  // so bold+italic has a real face
    readonly property FontLoader monoFontLoader:  FontLoader { source: Qt.resolvedUrl("fonts/JetBrainsMono-Regular.ttf") }
    // Aspekta (document body). The static 400/700 faces share the legacy family
    // "Aspekta 400" (Regular + Bold), so font.bold resolves the 700 face — needed
    // for headings. No italic face: italic body text synthesizes (oblique).
    readonly property FontLoader aspektaFont:     FontLoader { source: Qt.resolvedUrl("fonts/Aspekta-400.ttf") }
    readonly property FontLoader aspektaBoldFont: FontLoader { source: Qt.resolvedUrl("fonts/Aspekta-700.ttf") }
    // Lora (serif) for quote blocks — all four faces so bold/italic/
    // bold-italic inside a quote render with a real face, not a fallback.
    readonly property FontLoader serifFont:           FontLoader { source: Qt.resolvedUrl("fonts/Lora-Regular.ttf") }
    readonly property FontLoader serifBoldFont:       FontLoader { source: Qt.resolvedUrl("fonts/Lora-Bold.ttf") }
    readonly property FontLoader serifItalicFont:     FontLoader { source: Qt.resolvedUrl("fonts/Lora-Italic.ttf") }
    readonly property FontLoader serifBoldItalicFont: FontLoader { source: Qt.resolvedUrl("fonts/Lora-BoldItalic.ttf") }
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
        readonly property string family: theme.interFont.name        // Inter (chrome)
        readonly property string body:   theme.aspektaFont.name      // Aspekta (document body)
        readonly property string mono:   theme.monoFontLoader.name   // JetBrains Mono (bundled)
        readonly property string serif:  theme.serifFont.name        // Lora (quotes)
        readonly property int sizeSmall: 12
        readonly property int sizeChrome: 13 // panel/tab/toolbar labels (was a bare literal ~20 places)
        readonly property int sizeBody: 14   // a notch above the 13px kanban/tab tier
        readonly property int sizeMono: 13
    }
}
