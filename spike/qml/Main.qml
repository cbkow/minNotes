import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    visible: true
    width: 1180
    height: 920
    title: "minNotes — virtualization spike"

    property int arm: 2   // 0 = ListView (A), 1 = Flickable (B), 2 = Editor (C).
                          // Default C: the passive-surface prototype where the
                          // model solely owns caret/selection (no focus desync).
    function activeArm() { return armLoader.item }
    // Clean P7/P8 default (Arm B, Uniform 10k) is built in the BlockModel
    // constructor so it's correct from the first frame. Switch via the toolbar.

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 10

            Label { text: "Arm:" }
            TabBar {
                id: armTabs
                currentIndex: win.arm
                onCurrentIndexChanged: { win.arm = currentIndex; hud.reset() }
                TabButton { text: "A · ListView"; width: 110 }
                TabButton { text: "B · Flickable"; width: 110 }
                TabButton { text: "C · Editor"; width: 110 }
            }

            ToolSeparator {}

            Label { text: "Blocks:" }
            ComboBox {
                id: nBox
                model: ["10000", "100000", "500000"]
                currentIndex: 0
            }
            Label { text: "Dist:" }
            ComboBox {
                id: distBox
                model: ["Uniform", "Mixed", "Adversarial"]
                currentIndex: 0
            }
            Button {
                text: "Rebuild"
                onClicked: {
                    blockModel.rebuild(parseInt(nBox.model[nBox.currentIndex]), distBox.currentIndex)
                    if (win.activeArm()) win.activeArm().jumpToStart()
                    hud.reset()
                }
            }

            ToolSeparator {}

            Button { text: "⤓ End"; onClicked: if (win.activeArm()) win.activeArm().jumpToEnd() }
            Button { text: "⤒ Start"; onClicked: if (win.activeArm()) win.activeArm().jumpToStart() }
            Button {
                text: "▶ Bench"
                // Repeatable sustained-scroll sweep: resets the HUD window and
                // animates a full top→bottom scroll, so p50/p99/best/worst reflect
                // continuous motion (keep the window focused so ProMotion holds 120Hz).
                onClicked: {
                    var a = win.activeArm()
                    if (!a) return
                    a.scrollY = 0
                    hud.reset()
                    benchAnim.target = a
                    benchAnim.to = a.maxScrollY
                    benchAnim.restart()
                }
            }

            ToolSeparator {}

            Button {
                text: "+ block"
                onClicked: if (win.activeArm()) blockModel.insertBlock(win.activeArm().firstVisible)
            }
            Button {
                text: "− block"
                onClicked: if (win.activeArm()) blockModel.removeBlock(win.activeArm().firstVisible)
            }

            Item { Layout.fillWidth: true }
        }
    }

    Loader {
        id: armLoader
        anchors.fill: parent
        sourceComponent: win.arm === 0 ? armA : (win.arm === 1 ? armB : armC)
    }
    Component { id: armA; ArmListView {} }
    Component { id: armB; ArmFlickable {} }
    Component { id: armC; ArmEditor {} }

    NumberAnimation {
        id: benchAnim
        property: "scrollY"
        from: 0
        duration: 5000
        easing.type: Easing.InOutSine
        onFinished: hud.snapshot((win.arm === 0 ? "A" : win.arm === 1 ? "B" : "C")
                                 + " " + nBox.model[nBox.currentIndex]
                                 + " " + distBox.model[distBox.currentIndex])
    }

    Hud {
        id: hud
        anchors { top: parent.top; right: parent.right; margins: 12 }
        z: 100
        armName: win.arm === 0 ? "A · ListView" : (win.arm === 1 ? "B · Flickable" : "C · Editor")
        firstVisible: armLoader.item ? armLoader.item.firstVisible : 0
        lastVisible: armLoader.item ? armLoader.item.lastVisible : 0
        delegateCount: armLoader.item ? armLoader.item.delegateCount : 0
        barFraction: armLoader.item ? armLoader.item.barFraction : 0
        trueFraction: armLoader.item ? armLoader.item.trueFraction : 0
        selSummary: (armLoader.item && armLoader.item.selSummary !== undefined)
                    ? armLoader.item.selSummary : "caret/selection: Arm B only"
    }
}
