import QtQuick

// One block. Editable, type-styled, reports its laid-out height back to the
// model (estimate→settle), and — when given a cursor controller (Arm B) —
// participates in cross-block caret nav (P7) and selection (P8). The cursor is
// LOGICAL and lives in the controller, never in this delegate: a delegate
// claims focus/selection only when its current logicalRow matches the cursor.
Item {
    id: root
    required property int row
    required property int blockType
    required property string blockText
    property real contentWidth: 760

    // Cross-block cursor controller (ArmFlickable provides it; ListView passes
    // null → all cursor logic below no-ops and TextEdit behaves normally).
    property var cursorCtl: null

    // Height MUST reflect whatever is actually drawn, or the Fenwick index is
    // wrong and blocks overlap. For media the visual is the rectangle (the
    // TextEdit is invisible), so use the rectangle's height — not body's.
    implicitHeight: 12 + (root.blockType === 3 ? mediaRect.height : body.implicitHeight)
    width: contentWidth

    onImplicitHeightChanged: blockModel.setMeasuredHeight(row, implicitHeight)
    Component.onCompleted: { blockModel.setMeasuredHeight(row, implicitHeight); body.claimIfFocus(); body.syncSelection() }
    onRowChanged: { body.claimIfFocus(); body.syncSelection() }

    Rectangle {  // media block
        id: mediaRect
        visible: root.blockType === 3
        anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 6 }
        height: visible ? width * mediaAspect : 0
        property real mediaAspect: 0.5
        radius: 4
        color: Qt.hsla((root.row % 23) / 23, 0.45, 0.5, 1)
        Text { anchors.centerIn: parent; color: "white"; text: "▶ " + root.blockText; font.pixelSize: 14 }
    }

    Rectangle {  // code background
        visible: root.blockType === 2
        anchors.fill: body
        anchors.margins: -6
        color: "#1e1e28"
        radius: 4
    }

    TextEdit {
        id: body
        visible: root.blockType !== 3
        anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 6 }
        text: root.blockText
        wrapMode: TextEdit.Wrap
        selectByMouse: true
        persistentSelection: true   // keep cross-block selection painted when not focused
        textFormat: TextEdit.PlainText
        color: root.blockType === 2 ? "#d4d4e8" : "#1a1a22"
        selectionColor: "#3b6ea5"
        font.family: root.blockType === 2 ? "Menlo" : "Helvetica Neue"
        font.pixelSize: root.blockType === 1 ? 26 : 15
        font.bold: root.blockType === 1

        // Guard: suppress the cursor/selection feedback handlers while WE are
        // setting cursorPosition/selection from the controller, so controller →
        // view never bounces back as a spurious view → controller update.
        property bool programmatic: false

        onTextChanged: if (text !== root.blockText) blockModel.setContent(root.row, text)

        // --- P7: claim focus + place caret when this delegate currently shows
        // the cursor row. Fires on recycle (onRowChanged) too, so a block that
        // scrolls into view claims the caret that was logically already in it.
        function claimIfFocus() {
            var c = root.cursorCtl
            if (!c || root.blockType === 3 || root.row !== c.focusRow) return
            var pos = Math.max(0, Math.min(c.focusCol, length))
            programmatic = true
            if (!activeFocus) forceActiveFocus()
            if (cursorPosition !== pos) cursorPosition = pos
            programmatic = false
        }

        // --- P8: paint the slice of the logical selection that falls in THIS
        // block (full if strictly inside the range; partial at the two ends).
        function syncSelection() {
            var c = root.cursorCtl
            if (!c || root.blockType === 3) return
            programmatic = true
            if (root.row < c.loRow || root.row > c.hiRow) {
                if (selectedText.length) deselect()
            } else {
                var s = (root.row === c.loRow) ? Math.min(c.loCol, length) : 0
                var e = (root.row === c.hiRow) ? Math.min(c.hiCol, length) : length
                if (s >= e) { if (selectedText.length) deselect() }
                else select(s, e)
            }
            programmatic = false
        }

        // Mouse click places the caret → adopt it as the logical caret (AFTER
        // the click lands, so the position is correct). A plain click collapses
        // any prior multi-block selection.
        onCursorPositionChanged: {
            var c = root.cursorCtl
            if (programmatic || !c) return
            if (selectionStart === selectionEnd) c.setCaret(root.row, cursorPosition)
        }

        // Controller is authoritative for all caret/selection motion. Intercept
        // every arrow: move within the block (positionAt for vertical), or cross
        // the boundary at the edges. Shift extends (anchor fixed, focus moves).
        Keys.onPressed: (event) => {
            var c = root.cursorCtl
            if (!c) return                              // ListView arm: native TextEdit
            var shift = (event.modifiers & Qt.ShiftModifier) !== 0
            var n = blockModel.count
            var lh = cursorRectangle.height > 0 ? cursorRectangle.height : 18
            var k = event.key

            if (k === Qt.Key_Right) {
                if (cursorPosition < length) c.move(root.row, cursorPosition + 1, shift)
                else if (root.row < n - 1) c.move(root.row + 1, 0, shift)
                event.accepted = true
            } else if (k === Qt.Key_Left) {
                if (cursorPosition > 0) c.move(root.row, cursorPosition - 1, shift)
                else if (root.row > 0) c.move(root.row - 1, blockModel.contentForRow(root.row - 1).length, shift)
                event.accepted = true
            } else if (k === Qt.Key_Down) {
                var atBottom = cursorRectangle.y >= contentHeight - lh * 1.5
                if (!atBottom) c.move(root.row, positionAt(cursorRectangle.x, cursorRectangle.y + lh * 1.5), shift)
                else if (root.row < n - 1) c.move(root.row + 1, 0, shift)
                event.accepted = true
            } else if (k === Qt.Key_Up) {
                var atTop = cursorRectangle.y < lh * 0.5
                if (!atTop) c.move(root.row, positionAt(cursorRectangle.x, cursorRectangle.y - lh * 0.5), shift)
                else if (root.row > 0) c.move(root.row - 1, blockModel.contentForRow(root.row - 1).length, shift)
                event.accepted = true
            } else if ((k === Qt.Key_Backspace || k === Qt.Key_Delete) && c.loRow !== c.hiRow) {
                c.deleteSelection(); event.accepted = true
            }
        }

        // Re-sync paint / focus whenever the logical cursor moves.
        Connections {
            target: root.cursorCtl
            ignoreUnknownSignals: true
            function onFocusRowChanged() { body.claimIfFocus(); body.syncSelection() }
            function onFocusColChanged() { body.claimIfFocus(); body.syncSelection() }
            function onAnchorRowChanged() { body.syncSelection() }
            function onAnchorColChanged() { body.syncSelection() }
        }
    }
}
