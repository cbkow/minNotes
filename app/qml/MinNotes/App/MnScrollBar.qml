// MnScrollBar — themed ScrollBar with a flat thumb, transparent track, and
// policy-honouring visibility. Ported from the family's UfbScrollBar/MrScrollBar:
// derived from QtQuick.Templates.ScrollBar (NOT Basic/Fusion/native) so we own
// the look and geometry ourselves, independent of the active Quick Controls
// style. The stock styles either fade the thumb to near-invisible on dark, or
// pull colour from a palette we don't drive — and the native styles are
// __notCustomizable. Substituting our own T.ScrollBar sidesteps all of that.
//
// Crucially this sets implicitWidth/implicitHeight per orientation: that's what
// keeps the attached layout pinning the bar to the correct edge (an inline
// ScrollBar with a custom contentItem but no implicitWidth collapses and gets
// mis-positioned under customizable styles).
//
// Usage mirrors plain ScrollBar:
//   Flickable { ScrollBar.vertical: MnScrollBar {} }
//   ListView  { ScrollBar.horizontal: MnScrollBar { policy: ScrollBar.AlwaysOn } }
//
// Default policy is AsNeeded — the bar only appears when content overflows.
// Inner list/grid delegates should reserve a gutter of Theme.dim.scrollBarWidth
// so the thumb sits in empty space rather than over content.

import QtQuick
import QtQuick.Templates as T

T.ScrollBar {
    id: root

    policy: T.ScrollBar.AsNeeded

    // Templates ScrollBar defaults hoverEnabled to false where
    // Qt.styleHints.useHoverEffects is false (notably macOS/trackpad). Force it
    // on so the thumb can react to hover.
    hoverEnabled: true

    // Templates ScrollBar doesn't enforce policy visibility (Basic/Fusion do it
    // via opacity transitions). Bind visible directly so AsNeeded hides the bar
    // when content fits and AlwaysOff hides it unconditionally.
    visible: policy === T.ScrollBar.AlwaysOn
          || (policy === T.ScrollBar.AsNeeded && size < 1.0)

    // Fixed thickness per orientation. Locking it keeps adjacent content from
    // reflowing when the bar appears/disappears — and (the key fix) gives the
    // attached layout a real width so it pins the bar to the correct edge.
    implicitWidth:  orientation === Qt.Vertical   ? Theme.dim.scrollBarWidth : 0
    implicitHeight: orientation === Qt.Horizontal ? Theme.dim.scrollBarWidth : 0

    // Track — a slight fill a step above the page so the gutter reads subtly.
    background: Rectangle { color: Theme.colors.surfaceRaised }

    contentItem: Rectangle {
        implicitWidth:  root.orientation === Qt.Vertical   ? Theme.dim.scrollBarWidth : 0
        implicitHeight: root.orientation === Qt.Horizontal ? Theme.dim.scrollBarWidth : 0
        radius: 0   // flat, square thumb (matches the app's flat language)
        // Hover tracked on the thumb itself (the control's hover area covers the
        // full, often-transparent track) so only what's under the cursor brightens.
        // Pressed brightens a notch above hover — but not to full white.
        color: root.pressed ? Qt.lighter(Theme.colors.textMuted, 1.3)
             : thumbHover.hovered ? Theme.colors.textMuted
                                  : Theme.colors.textSubtle

        HoverHandler {
            id: thumbHover
            cursorShape: Qt.ArrowCursor
        }
    }
}
