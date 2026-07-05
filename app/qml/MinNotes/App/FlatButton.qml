// FlatButton — squared, minimal-padding button matching the Topaz
// Gigapixel reference style. Replaces Qt Controls' default Button
// (which renders as a tall pill with ~16px horizontal padding).
//
// Variants:
//   variant: "default"  — transparent fill, hover lights up
//   variant: "primary"  — accent-blue fill (use for the dominant
//                         action in a strip)
//   variant: "danger"   — red fill (destructive)
//
// Use inside ToolStrip for full-row-height segmented look. Use stand-
// alone with explicit `Layout.preferredHeight` for inline buttons.
//
// Icon-only:  FlatButton { iconName: "x"; tooltip: "Close" }
// Icon+text:  FlatButton { iconName: "play"; text: "Start" }
// Text-only:  FlatButton { text: "Save"; variant: "primary" }

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    /// Phosphor icon name (optional). Rendered before `text`.
    property string iconName: ""
    /// Visible label (optional). Empty = icon-only.
    property string text: ""
    /// Label weight/slant (for lettered buttons like a bold "B" / italic "I").
    property bool boldLabel: false
    property bool italicLabel: false
    /// Tooltip text. Empty = no tooltip.
    property string tooltip: ""
    /// Where the tooltip sits relative to the button: "right" (left rail) | "top".
    property string tooltipSide: "right"
    /// "default" | "primary" | "danger"
    property string variant: "default"
    /// Active/checked appearance — used by SegmentedControl + toggles.
    property bool checked: false
    /// Disable the button (greys + ignores clicks).
    property bool enabled_: true
    /// Icon size. Defaults to Theme.icon.sizeToolbar.
    property int iconSize: Theme.icon.sizeToolbar
    /// Extra horizontal padding inside the row (text breathing room).
    property int padding: 8
    /// Optional icon color override. Empty/transparent = use the
    /// variant's foreground color. Set this when you need to tint
    /// just the glyph (e.g. an accent-blue caret on a transparent
    /// "default" button).
    property color iconColor: "transparent"
    /// Optional label overrides (size + color). 0 / transparent = the
    /// defaults (sizeBody, the variant foreground). Used where a strip
    /// matches another bar's type, e.g. the table-tab toolbar mirroring
    /// the bottom tab strip's 13px muted/bright scheme.
    property int labelSize: 0
    property color labelColor: "transparent"
    /// Optional checked-state fill override. Transparent = the default
    /// accentMuted/accent pair. Set to a grey for toggles that shouldn't
    /// spend accent (e.g. the table-tab Table/Board view switch).
    property color checkedColor: "transparent"

    signal clicked()
    /// Emitted on mouse-down / mouse-up. Use for press-and-hold actions
    /// (e.g. the lightbox's fast-seek shuttle); `clicked` still fires on a
    /// normal press+release for tap actions.
    signal pressed()
    signal released()
    /// True while the cursor is over the button (for hover-driven flyouts).
    readonly property bool hovered: ma.containsMouse

    // Resolved colors per variant + state.
    // No border at any state — checked-default uses an accentMuted
    // fill so the active/selected segment differentiates by colour
    // alone (matches sister-app FlatButton). Hover stays a tonal
    // surfaceHover swap; primary/danger keep their accent/error
    // fills.
    readonly property color _bgIdle: {
        if (!enabled_) return "transparent"
        if (variant === "primary") return Theme.colors.accent
        if (variant === "danger")  return Theme.colors.error
        if (checked) return checkedColor.a > 0 ? checkedColor : Theme.colors.accentMuted
        return "transparent"
    }
    readonly property color _bgHover: {
        if (!enabled_) return _bgIdle
        if (variant === "primary") return Theme.colors.accentHover
        if (variant === "danger")  return Qt.lighter(Theme.colors.error, 1.15)
        if (checked) return checkedColor.a > 0 ? Qt.lighter(checkedColor, 1.25) : Theme.colors.accent
        return Theme.colors.surfaceHover
    }
    readonly property color _fg: {
        if (!enabled_) return Theme.colors.textSubtle
        if (variant === "primary" || variant === "danger") return Theme.colors.textBright
        return checked ? Theme.colors.textBright : Theme.colors.text
    }

    implicitHeight: Theme.dim.toolStripHeight
    implicitWidth: Math.max(
        Theme.dim.toolStripHeight,
        rowContent.implicitWidth + padding * 2)
    radius: 0   // perfectly square corners
    color: ma.containsMouse ? _bgHover : _bgIdle

    Row {
        id: rowContent
        anchors.centerIn: parent
        spacing: 6

        Icon {
            visible: root.iconName.length > 0
            name: root.iconName
            size: root.iconSize
            color: root.iconColor.a > 0 ? root.iconColor : root._fg
            anchors.verticalCenter: parent.verticalCenter
        }
        Label {
            visible: root.text.length > 0
            text: root.text
            color: root.labelColor.a > 0 ? root.labelColor : root._fg
            font.pixelSize: root.labelSize > 0 ? root.labelSize : Theme.font.sizeBody
            font.family: Theme.font.family
            font.bold: root.boldLabel
            font.italic: root.italicLabel
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: root.enabled_ ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked:  { if (root.enabled_) root.clicked() }
        onPressed:  (mouse) => { if (root.enabled_) root.pressed() }
        onReleased: (mouse) => { if (root.enabled_) root.released() }
        // Treat leaving the button while held as a release, so a held
        // fast-seek can't get stuck on if the cursor slides off.
        onExited:   { if (root.enabled_ && pressed) root.released() }
    }

    // Family tooltip. Default RIGHT (the rail hugs the left edge, so below/
    // over would clip or cover the next button); "top" for bottom-row bars,
    // "bottom"/"left" for chrome hugging the top/right edges (the annotation
    // floater).
    FlatToolTip {
        text: root.tooltip
        visible: root.tooltip.length > 0 && ma.containsMouse
        x: root.tooltipSide === "top" || root.tooltipSide === "bottom"
               ? (root.width - implicitWidth) / 2
           : root.tooltipSide === "left" ? -implicitWidth - 6
           : root.width + 6
        y: root.tooltipSide === "top" ? -implicitHeight - 6
         : root.tooltipSide === "bottom" ? root.height + 6
         : (root.height - implicitHeight) / 2
    }
}
