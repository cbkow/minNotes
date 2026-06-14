import QtQuick
import QtQuick.Controls

// Project-themed Menu (Windows/Linux in-window menus). Ported from the family's
// QCView ThemedMenu: under the Fusion style, item colors flow via `palette`
// roles — Fusion respects them, so plain Action/MenuItem children need no custom
// contentItem or per-item delegate (accelerator handling + disabled states come
// for free). The popup + submenus read as the same surface as the palette/
// inspector panel (Theme.colors.surfaceRaised). Rules are ThemedMenuSeparator.
Menu {
    id: themedMenu

    background: Rectangle {
        implicitWidth: 220
        color:        Theme.colors.surfaceRaised
        border.color: Theme.colors.border
        border.width: 1
        radius:       Theme.dim.radius
    }

    palette.window:           Theme.colors.surfaceRaised
    palette.windowText:       Theme.colors.text
    palette.base:             Theme.colors.surfaceRaised
    palette.text:             Theme.colors.text
    palette.button:           Theme.colors.surfaceRaised
    palette.buttonText:       Theme.colors.text
    palette.highlight:        Theme.colors.accentMuted
    palette.highlightedText:  Theme.colors.textBright
    palette.mid:              Theme.colors.border
    palette.midlight:         Theme.colors.surfaceHover
    palette.dark:             Theme.colors.bg
    palette.shadow:           Theme.colors.bg

    // Disabled-state group — greyed-out menu items.
    palette.disabled.text:       Theme.colors.textMuted
    palette.disabled.windowText: Theme.colors.textMuted
    palette.disabled.buttonText: Theme.colors.textMuted
}
