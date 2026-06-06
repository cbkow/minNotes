// Theme — central design tokens, singleton. `Theme.colors.text` etc. from any
// QML file. Dark, flat, blue-accent — matching the ufb / MinRender family.
pragma Singleton
import QtQuick

QtObject {
    id: theme

    readonly property QtObject colors: QtObject {
        readonly property color bg:          "#161616"   // window shell
        readonly property color surface:     "#1b1b1b"   // the document "page"
        readonly property color codeBg:      "#121212"   // code block fill
        readonly property color border:      "#2a2a2a"
        readonly property color divider:     "#333333"   // hr / separators

        readonly property color text:        "#dcdcdc"   // body
        readonly property color textBright:  "#ffffff"   // headings
        readonly property color textMuted:   "#8a8a8a"   // quotes, bullets
        readonly property color textSubtle:  "#5e5e5e"
        readonly property color codeText:    "#d4d4e8"
        // Inline code chip — a touch LIGHTER than the page so it reads as raised
        // (the code-block fill is darker); distinct from surrounding prose. Text
        // in the family blue rather than the default lavender.
        readonly property color inlineCodeBg:   "#1d2733"   // blue-tinted dark chip
        readonly property color inlineCodeText: "#4aa8ff"   // accent-family blue

        readonly property color accent:      "#0189f1"   // caret
        readonly property color selectionBg: "#2a568c"   // text-selection fill (saturated cobalt)
        readonly property color quoteBar:    "#3a5e86"
    }

    readonly property QtObject dim: QtObject {
        readonly property int padding:        8
        readonly property int paddingLoose:   16
        readonly property int radius:         4
        readonly property int scrollBarWidth: 9
        readonly property int columnWidth:    760    // reading-measure cap
        readonly property int docTopPad:      28     // breathing room above block 0
    }

    readonly property QtObject font: QtObject {
        readonly property string family: Qt.platform.os === "osx" ? "Helvetica Neue"
                                       : Qt.platform.os === "windows" ? "Segoe UI" : "sans-serif"
        readonly property string mono: Qt.platform.os === "windows" ? "Consolas"
                                     : Qt.platform.os === "osx" ? "Menlo" : "monospace"
        readonly property int sizeBody: 15
        readonly property int sizeMono: 14
    }
}
