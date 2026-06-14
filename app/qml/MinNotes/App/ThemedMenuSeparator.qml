import QtQuick
import QtQuick.Controls

// Menu separator recolored to the Theme (ported from QCView). Fusion's default
// rule is a fixed shade darker than our surface fill, and it isn't palette-
// driven, so ThemedMenu's palette can't reach it — a custom contentItem is the
// only lever. Draws a 1px rule in Theme.colors.divider (the rail-divider token),
// matching Fusion's padding so the menu's vertical rhythm is unchanged.
MenuSeparator {
    padding: 5
    verticalPadding: 1
    contentItem: Rectangle {
        implicitWidth: 188
        implicitHeight: 1
        color: Theme.colors.divider
    }
}
