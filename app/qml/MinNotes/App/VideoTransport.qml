import QtQuick
import QtQuick.Layouts

// The family video transport bar — one shared control used by the inline
// per-video toolbars (Editor's Repeater positions/fades a wrapper around it)
// and by the video studio tab. `live` = `row` is the active player, so the
// controls bind to the shared decoder; otherwise the bar shows the banked
// static state and any control activates the video first (ensureVideoActive
// is a no-op when already active). Transport logic lives on the editor root;
// this is purely the chrome + bindings.
Rectangle {
    id: vt
    property var editor: null     // the Editor root (transport functions)
    property var dec: null        // the shared VideoDecoder
    property var audio: null      // the shared AudioPlayer
    property int row: -1          // this bar's video block row
    property bool live: false

    readonly property int totalFrames: live ? dec.frameCount
                                            : (blockModel.contentRevision, blockModel.mediaFrames(row))

    height: editor ? editor.videoTransportH : 40
    color: "#212121"      // a hair lighter than the page (#1b1b1b)
    Rectangle { width: parent.width; height: 1; color: Theme.colors.border }   // top hairline

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 6; anchors.rightMargin: 8
        spacing: 0

        FlatButton { iconName: "skip-back"; tooltip: qsTr("Jump to start"); tooltipSide: "top"
            onClicked: { vt.editor.ensureVideoActive(vt.row); vt.editor.seekVideoStart() } }
        FlatButton { iconName: "rewind"; tooltip: qsTr("Rewind — hold"); tooltipSide: "top"
            onPressed: { vt.editor.ensureVideoActive(vt.row); vt.editor.startVideoFastSeek(-1) }
            onReleased: vt.editor.stopVideoFastSeek() }
        FlatButton { iconName: "caret-left"; tooltip: qsTr("Step back"); tooltipSide: "top"
            onClicked: { vt.editor.ensureVideoActive(vt.row); vt.editor.stepVideoFrames(-1) } }
        FlatButton { iconName: (vt.live && vt.dec.isPlaying) ? "pause" : "play"
            tooltip: qsTr("Play / Pause"); tooltipSide: "top"; onClicked: vt.editor.playVideo(vt.row) }
        FlatButton { iconName: "caret-right"; tooltip: qsTr("Step forward"); tooltipSide: "top"
            onClicked: { vt.editor.ensureVideoActive(vt.row); vt.editor.stepVideoFrames(1) } }
        FlatButton { iconName: "fast-forward"; tooltip: qsTr("Fast-forward — hold"); tooltipSide: "top"
            onPressed: { vt.editor.ensureVideoActive(vt.row); vt.editor.startVideoFastSeek(1) }
            onReleased: vt.editor.stopVideoFastSeek() }
        FlatButton { iconName: "skip-forward"; tooltip: qsTr("Jump to end"); tooltipSide: "top"
            onClicked: { vt.editor.ensureVideoActive(vt.row); vt.editor.seekVideoEnd() } }

        FlatSlider {
            id: vscrub
            Layout.fillWidth: true
            Layout.leftMargin: 8; Layout.rightMargin: 8
            Layout.alignment: Qt.AlignVCenter
            from: 0; to: Math.max(1, vt.totalFrames - 1)
            fillColor: Theme.colors.textBright   // white progress fill
            property bool wasPlaying: false
            // Pause while scrubbing so each seek lands; resume repositions
            // the streaming decoder to the scrubbed frame first.
            onPressedChanged: {
                if (pressed) {
                    vt.editor.ensureVideoActive(vt.row)
                    wasPlaying = vt.dec.isPlaying
                    vt.dec.pause(); vt.audio.pause()
                    if (vt.dec.fps > 0)   // deck-style scrub audio for the drag
                        vt.editor.beginScrubAudio(value / vt.dec.fps)
                } else {
                    vt.editor.endScrubAudio()
                    if (wasPlaying) {
                        vt.editor._vidSyncForResume(); vt.dec.play()
                        if (vt.audio.hasAudio) vt.audio.play()
                    }
                }
            }
            onMoved: {
                vt.editor.ensureVideoActive(vt.row)
                var f = Math.round(value)
                vt.editor._vidScrubTo(f)
                if (vt.dec.fps > 0) vt.editor.scrubAudioMove(f / vt.dec.fps)
            }
            Connections {
                target: vt.dec
                function onCurrentFrameChanged() {
                    if (vt.live && !vscrub.pressed) vscrub.value = vt.dec.currentFrame
                }
            }
            // Not the live player → park the scrubber at the banked playhead
            // (not 0), so a torn-down video shows where it left off.
            Binding {
                target: vscrub; property: "value"
                value: (vt.editor.videoPlayheadRev, vt.editor.videoPlayheadFor(vt.row))
                when: !vt.live && !vscrub.pressed
            }
        }

        Text {   // frame counter (mirrors ufb)
            text: (vt.live ? vt.dec.currentFrame
                           : (vt.editor.videoPlayheadRev, vt.editor.videoPlayheadFor(vt.row)))
                  + " / " + Math.max(0, vt.totalFrames - 1)
            color: Theme.colors.textMuted
            font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
            Layout.rightMargin: 4
        }

        Text {   // review-speed readout — only when rate ≠ 1x (R cycles, Shift+R resets)
            visible: Math.abs(vt.editor.videoSpeed - 1.0) > 0.001
            text: vt.editor.videoSpeed.toFixed(2).replace(/\.?0+$/, "") + "×"
            color: Theme.colors.accent
            font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
            font.bold: true
            verticalAlignment: Text.AlignVCenter
            Layout.rightMargin: 4
            MouseArea {   // click to cycle, matching the key
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: { vt.editor.ensureVideoActive(vt.row); vt.editor.cycleVideoSpeed() }
            }
        }

        FlatButton { iconName: "repeat"; tooltip: qsTr("Loop"); tooltipSide: "top"
            checked: vt.editor.videoLoop; onClicked: vt.editor.toggleVideoLoop() }
        FlatButton {
            visible: vt.live && vt.audio.hasAudio
            iconName: (vt.audio.muted || vt.audio.volume <= 0) ? "speaker-x" : "speaker-high"
            tooltip: qsTr("Mute"); tooltipSide: "top"; onClicked: vt.editor.toggleVideoMute()
        }
        FlatSlider {
            visible: vt.live && vt.audio.hasAudio
            Layout.preferredWidth: 64; Layout.leftMargin: 2
            Layout.alignment: Qt.AlignVCenter
            from: 0; to: 1
            value: vt.audio.muted ? 0 : vt.audio.volume
            fillColor: Theme.colors.textMuted
            onMoved: { vt.audio.setVolume(value); if (value > 0) vt.audio.setMuted(false) }
        }
    }
}
