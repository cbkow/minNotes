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
    color: Theme.colors.surfaceRaised   // the raised-chrome plane (tonal ladder)
    Rectangle { width: parent.width; height: 1; color: Theme.colors.border }   // top hairline

    RowLayout {
        id: transportRow
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

        TextMetrics {   // the counter's widest possible string for this clip
            id: counterMetrics
            font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
            text: Math.max(0, vt.totalFrames - 1) + " / " + Math.max(0, vt.totalFrames - 1)
        }
        Text {   // frame counter (mirrors ufb) — width RESERVED at the clip max
                 // so growing digits don't reflow the row (the slider would
                 // shrink and the note markers would creep during playback)
            text: (vt.live ? vt.dec.currentFrame
                           : (vt.editor.videoPlayheadRev, vt.editor.videoPlayheadFor(vt.row)))
                  + " / " + Math.max(0, vt.totalFrames - 1)
            color: Theme.colors.textMuted
            font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
            Layout.preferredWidth: counterMetrics.width
            horizontalAlignment: Text.AlignRight
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

        FlatButton {   // notes/annotations visibility — ONE switch shared with the studio
            visible: vt.live && vt.editor.videoNoteArr.length > 0
            iconName: vt.editor.annotationsHidden ? "eye" : "eye-slash"
            checked: vt.editor.annotationsHidden
            tooltip: vt.editor.annotationsHidden ? qsTr("Show notes") : qsTr("Hide notes")
            tooltipSide: "top"
            onClicked: vt.editor.annotationsHidden = !vt.editor.annotationsHidden
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

    // Per-bar note store, so markers show BEFORE the video ever activates
    // (the root vnotes only follows the screen-owning clip). Read-only —
    // nothing here mutates; its file watcher keeps parked bars fresh when
    // the studio or QCView saves the sidecar.
    VideoNotesModel {
        id: barNotes
        mediaPath: (vt.row >= 0 && blockModel.contentRevision >= 0)
                   ? blockModel.mediaLocalPath(vt.row) : ""
        fps: vt.row >= 0 ? blockModel.mediaFps(vt.row) : 0
    }

    // QCView note markers: solid triangles straddling the bar's top edge
    // (poking over the image a bit), tips pointing down at the timeline.
    // x is the exact spot the HANDLE's center occupies at the note's frame,
    // so the playhead parks precisely under a marker. The live bar reads the
    // root list (instant on studio edits); parked bars read their own store
    // (revision read load-bearing — rule 1e). Click = jump to the note's
    // frame PAUSED (notes pin to one frame — land on it, read it).
    Repeater {
        model: vt.live ? vt.editor.videoNoteArr
                       : (barNotes.revision >= 0 ? barNotes.noteList() : [])
        delegate: Item {
            required property var modelData
            readonly property int nf: modelData.frame
            readonly property real frac: nf / Math.max(1, vt.totalFrames - 1)
            width: 12; height: 12
            x: transportRow.x + vscrub.x + vscrub.leftPadding
               + frac * (vscrub.availableWidth - vscrub.handle.width)
               + vscrub.handle.width / 2 - width / 2
            y: -5
            Icon {
                anchors.fill: parent
                name: "caret-down"; weight: "fill"; size: 12
                // QCView's timeline-pin violet — "a QCView note" is one
                // color across both apps; addressed notes dim to grey.
                color: modelData.addressed ? Theme.colors.textSubtle
                                           : Theme.colors.noteMarker
            }
            MouseArea {
                anchors.fill: parent
                anchors.margins: -2
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    vt.editor.ensureVideoActive(vt.row)
                    vt.dec.pause(); vt.audio.pause()
                    vt.editor._vidScrubTo(nf)
                }
            }
        }
    }
}
