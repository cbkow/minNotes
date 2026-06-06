// FlatToolTip — the family tooltip: opaque dark fill, squared corners, a single
// hairline border, Inter at the small size. Matches QCView / MinRender.
//
// FlatButton bakes this in; use the freestanding form on any hover target:
//   SomeItem { FlatToolTip { visible: parent.hovered; text: "…" } }

import QtQuick
import QtQuick.Controls.Basic

ToolTip {
    id: root
    delay: 500
    timeout: 5000

    background: Rectangle {
        color: "#0e0e0e"
        border.color: Theme.colors.border
        border.width: 1
        radius: 0
    }
    contentItem: Text {
        text: root.text
        color: Theme.colors.text
        font.family: Theme.font.family
        font.pixelSize: Theme.font.sizeSmall
    }
}
