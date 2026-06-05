import QtQuick
import QtQuick.Controls

// Arm A — ListView + QAbstractListModel. Qt's delegate recycling gives zones
// 1+2 for free (DESIGN.md §7). The known risk: ListView computes its OWN
// contentHeight from delegates it has seen, so the scrollbar (P3) and
// jump-to-end (P2) ride on an estimate that changes as you scroll. We measure
// exactly that, comparing ListView's scroll fraction against the Fenwick truth.
Item {
    id: root
    property real colWidth: Math.min(width - 40, 760)

    // --- HUD telemetry ---
    readonly property int firstVisible: list.indexAt(list.width / 2, list.contentY) < 0
                                        ? 0 : list.indexAt(list.width / 2, list.contentY)
    readonly property int lastVisible: list.indexAt(list.width / 2, list.contentY + list.height - 1) < 0
                                       ? blockModel.count - 1
                                       : list.indexAt(list.width / 2, list.contentY + list.height - 1)
    readonly property int delegateCount: list.count > 0 ? list.contentItem.children.length : 0
    // Fraction the ListView THINKS we are at vs. the TRUE fraction from Fenwick.
    readonly property real barFraction: list.contentHeight > list.height
        ? list.contentY / (list.contentHeight - list.height) : 0
    readonly property real trueFraction: blockModel.totalHeight > root.height
        ? blockModel.yForRow(firstVisible) / (blockModel.totalHeight - root.height) : 0

    Rectangle { anchors.fill: parent; color: "#ffffff" }   // light page (dark-mode safe)

    function jumpToEnd() { list.positionViewAtEnd() }
    function jumpToStart() { list.positionViewAtBeginning() }
    property alias scrollY: list.contentY
    readonly property real maxScrollY: Math.max(0, list.contentHeight - list.height)

    ListView {
        id: list
        anchors.fill: parent
        model: blockModel
        cacheBuffer: 800   // prefetch ring (zone 2)
        clip: true
        reuseItems: true

        // Wrapper maps model roles (index/blockType/blockContent) onto the
        // shared BlockDelegate inputs, avoiding role/property name clashes.
        delegate: Item {
            width: list.width
            height: bd.implicitHeight
            BlockDelegate {
                id: bd
                row: index
                blockType: model.blockType
                blockText: model.blockContent
                contentWidth: root.colWidth
                x: (parent.width - width) / 2
            }
        }

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }
    }
}
