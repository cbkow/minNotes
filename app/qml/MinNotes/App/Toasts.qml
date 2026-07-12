// Toasts — the app's outcome voice (product pass 3, QCView's Toast recipe).
// One transient message at a time for operations that would otherwise
// succeed silently: Save, Save As, path mappings, image copy…
//
//   Toasts.show(qsTr("Saved"))                    // success (default)
//   Toasts.show(qsTr("Nothing to export"), 1)     // warning
//   Toasts.show(qsTr("Copy failed"), 2)           // error
//
// State-only singleton; the visual lives in Main.qml (pure QML scene —
// no OS-popup compositing trick needed here, unlike QCView's player HWND).
// A new show() replaces the current message and restarts the clock.
pragma Singleton
import QtQuick

QtObject {
    id: root

    property string message: ""
    property int kind: 0          // 0 = success, 1 = warning, 2 = error
    property bool active: false

    function show(msg, k) {
        message = msg
        kind = (k === undefined) ? 0 : k
        active = true
        _hide.restart()
    }
    function dismiss() { active = false; _hide.stop() }

    readonly property Timer _hide: Timer {
        interval: 3400
        onTriggered: root.active = false
    }
}
