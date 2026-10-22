import QtQuick
import QtQuick.Controls

// Project-themed Menu. Ported from the family's QCView ThemedMenu, then given its OWN item delegate (2026-10-21,
// the chromeless window): the app menu now opens from the title bar's dot on BOTH platforms, so the look can't
// depend on the Fusion palette any more (the macOS style ignores it) — the rows, the check mark, the submenu
// caret and the hover fill are drawn here with the Theme tokens. Rules are ThemedMenuSeparator. Always an
// in-scene popup (the style would otherwise pick a native menu on macOS).
Menu {
    id: themedMenu
    popupType: Popup.Item
    padding: 3
    overlap: 1
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.dim.popupMs } }

    background: Rectangle {
        implicitWidth: 240
        color:        Theme.colors.surfaceRaised
        border.color: Theme.colors.border
        border.width: 1
        radius:       Theme.dim.radius
    }

    delegate: MenuItem {
        id: mi
        implicitHeight: 26
        implicitWidth: Math.max(240, label.implicitWidth + 24 + 32)
        leftPadding: 24; rightPadding: 24; topPadding: 0; bottomPadding: 0
        readonly property color fg: !mi.enabled ? Theme.colors.textSubtle : (mi.highlighted ? Theme.colors.textBright : Theme.colors.text)
        contentItem: Text {
            id: label
            text: mi.text
            color: mi.fg
            font.family: Theme.font.family; font.pixelSize: Theme.font.sizeChrome
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        indicator: Item {
            x: 7; width: 12; height: mi.height
            Icon { anchors.centerIn: parent; name: "check"; size: 11; color: mi.fg; visible: mi.checkable && mi.checked }
        }
        arrow: Item {
            x: mi.width - width - 8; width: 10; height: mi.height
            Icon { anchors.centerIn: parent; name: "caret-right"; size: 10; color: mi.fg; visible: !!mi.subMenu }
        }
        background: Rectangle {
            radius: Theme.dim.radiusSmall
            color: mi.highlighted ? Theme.colors.menuHover : Theme.colors.hoverIdle
            Behavior on color { ColorAnimation { duration: Theme.dim.hoverMs } }
        }
    }

    // The palette still reaches anything the delegate doesn't draw (a plain MenuItem dropped in by a caller).
    palette.window:           Theme.colors.surfaceRaised
    palette.windowText:       Theme.colors.text
    palette.base:             Theme.colors.surfaceRaised
    palette.text:             Theme.colors.text
    palette.button:           Theme.colors.surfaceRaised
    palette.buttonText:       Theme.colors.text
    palette.highlight:        Theme.colors.surfaceHover
    palette.highlightedText:  Theme.colors.textBright
    palette.mid:              Theme.colors.border
    palette.midlight:         Theme.colors.surfaceHover
    palette.dark:             Theme.colors.bg
    palette.shadow:           Theme.colors.bg
    palette.disabled.text:       Theme.colors.textMuted
    palette.disabled.windowText: Theme.colors.textMuted
    palette.disabled.buttonText: Theme.colors.textMuted
}
