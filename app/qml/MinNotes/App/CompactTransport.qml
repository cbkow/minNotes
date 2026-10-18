import QtQuick

// The COMPACT transport (2026-10-18, QCView's Compact Mode strip brought to minNotes): one 22-px band under an
// inline video block in a page and under a selected canvas video element — play / pause, the frame counter, a
// scrub line with the played part, the playhead and the QCView note markers, and an expand glyph that opens the
// studio (where the full family transport lives). Drives the same editor transport functions as VideoTransport:
// `row` for a page block, `host` (a MediaHost) for a canvas element — `live` = this media owns the shared player.
Rectangle {
    id: ct
    property var editor: null
    property var dec: null
    property var audio: null
    property int row: -1
    property var host: null
    property bool live: false
    readonly property int totalFrames: live && dec ? dec.frameCount : host ? host.frames : (row >= 0 ? (blockModel.structureRevision, blockModel.mediaFrames(row)) : 0)
    height: 22
    color: Theme.colors.surfaceRaised
    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; height: 1; color: Theme.colors.border }

    function activate() { if (host) ct.editor.activateHost(host); else ct.editor.ensureVideoActive(ct.row) }
    function playhead() { return host ? ct.editor.videoPlayheadForKey(host.key) : ct.editor.videoPlayheadFor(ct.row) }
    function choose()   { if (!host && ct.row >= 0 && ct.editor.parkCaretOn) ct.editor.parkCaretOn(ct.row) }   // a bar click CHOOSES its block (walk 7c): the caret parks there, the keys follow
    function play()     { choose(); if (host) ct.editor.playFromHost(host); else ct.editor.playVideo(ct.row); ct.editor.forceActiveFocus() }   // the keys stay with the editor
    function expand()   { choose(); if (host) ct.editor.setActiveTab(host.elementId); else ct.editor.setActiveTab(blockModel.idForRow(ct.row)); ct.editor.forceActiveFocus() }
    readonly property int frame: live && dec ? dec.currentFrame : (ct.editor.videoPlayheadRev, playhead())
    readonly property var playButton: playBtn   // probe
    readonly property var scrubTrack: track
    readonly property var counterItem: counterText

    Row {
        id: strip
        anchors.fill: parent
        anchors.leftMargin: 2; anchors.rightMargin: 4
        spacing: 6
        FlatButton {
            id: playBtn
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 22; implicitHeight: 22; iconSize: 13; radius: 0
            iconName: (ct.live && ct.dec && ct.dec.isPlaying) ? "pause" : "play"
            iconColor: ct.live ? Theme.colors.textBright : "transparent"   // the live player reads through its tones (no opacity games)
            tooltip: (ct.live && ct.dec && ct.dec.isPlaying) ? qsTr("Pause (Space)") : qsTr("Play (Space)"); tooltipSide: "top"
            onClicked: ct.play()
        }
        readonly property real fpsVal: ct.live && ct.dec ? ct.dec.fps : ct.host ? ct.host.fps : (ct.row >= 0 ? blockModel.mediaFps(ct.row) : 0)
        readonly property bool timecode: !!ct.editor && ct.editor.videoTimecode
        function counterText(f) { return timecode ? ct.editor.videoTimecodeFor(f, fpsVal) : "" + Math.max(0, f) }
        TextMetrics { id: counterMetrics; font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall   // (a TextMetrics has no `parent`: name the row)
                      text: strip.counterText(Math.max(0, ct.totalFrames - 1)) }   // the widest value: the last frame (no total — user, 2026-10-18)
        Text {   // the counter: timecode (default) or frames; a CLICK flips every player (user, 2026-10-18). Width reserved at the widest string (no reflow while playing)
            id: counterText
            anchors.verticalCenter: parent.verticalCenter
            width: counterMetrics.width
            text: strip.counterText(ct.frame)
            color: counterMA.containsMouse ? Theme.colors.textBright : Theme.colors.textMuted
            font.family: Theme.font.mono; font.pixelSize: Theme.font.sizeSmall
            MouseArea { id: counterMA; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: { if (ct.editor) { ct.editor.toggleVideoCounter(); ct.editor.forceActiveFocus() } } }
            FlatToolTip { parent: counterText; x: 0; y: counterText.height + 6; visible: counterMA.containsMouse
                          text: strip.timecode ? qsTr("Timecode — click for frames") : qsTr("Frames — click for timecode") }
            horizontalAlignment: Text.AlignRight
        }
        Item {   // the scrub line: played part, playhead, note markers; press / drag scrubs (the slider's gesture, deck audio)
            id: track
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 22 - counterMetrics.width - 22 - 3 * 6 - 6
            height: 22
            readonly property real fraction: ct.totalFrames > 1 ? Math.max(0, Math.min(1, ct.frame / (ct.totalFrames - 1))) : 0
            function xToFrame(x) { return Math.round(Math.max(0, Math.min(1, x / Math.max(1, width))) * Math.max(0, ct.totalFrames - 1)) }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: 2
                        color: scrubMa.containsMouse || scrubMa.pressed ? Theme.colors.textSubtle : Theme.colors.divider }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width * track.fraction; height: 2; color: Theme.colors.textMuted }
            Repeater {   // QCView note markers (the live list for the playing clip, the bar's own store otherwise)
                model: ct.live ? ct.editor.videoNoteArr : (barNotes.revision >= 0 ? barNotes.noteList() : [])
                delegate: Rectangle {
                    required property var modelData
                    x: Math.max(0, Math.min(track.width - width, track.width * modelData.frame / Math.max(1, ct.totalFrames - 1) - width / 2))
                    anchors.verticalCenter: parent.verticalCenter
                    width: 2; height: 10
                    color: modelData.addressed ? Theme.colors.textSubtle : Theme.colors.noteMarker
                }
            }
            Rectangle {   // the playhead
                x: Math.max(0, Math.min(track.width - width, track.width * track.fraction - width / 2))
                anchors.verticalCenter: parent.verticalCenter
                width: 3; height: 12
                color: ct.live ? Theme.colors.textBright : Theme.colors.textMuted   // bright only on the live player
            }
            MouseArea {
                id: scrubMa
                anchors.fill: parent; hoverEnabled: true
                preventStealing: true   // the canvas's marquee takes over from items past the drag threshold — not from a scrub
                cursorShape: Qt.PointingHandCursor
                property bool wasPlaying: false
                onPressed: (m) => {
                    ct.choose(); ct.activate()
                    wasPlaying = !!ct.dec && ct.dec.isPlaying
                    if (ct.dec) { ct.dec.pause(); if (ct.audio) ct.audio.pause() }
                    const f = track.xToFrame(m.x)
                    ct.editor._vidScrubTo(f)
                    if (ct.dec && ct.dec.fps > 0) ct.editor.beginScrubAudio(f / ct.dec.fps)
                }
                onPositionChanged: (m) => {
                    if (!pressed) return
                    const f = track.xToFrame(m.x)
                    ct.editor._vidScrubTo(f)
                    if (ct.dec && ct.dec.fps > 0) ct.editor.scrubAudioMove(f / ct.dec.fps)
                }
                onReleased: {
                    ct.editor.endScrubAudio()
                    if (wasPlaying && ct.dec) { ct.editor._vidSyncForResume(); ct.dec.play(); if (ct.audio && ct.audio.hasAudio) ct.audio.play() }
                    ct.editor.forceActiveFocus()
                }
            }
        }
        FlatButton {   // expand: the studio (the full transport, the notes panel)
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 22; implicitHeight: 22; iconSize: 13; radius: 0
            iconName: "arrows-out-simple"
            tooltip: qsTr("Open in the studio"); tooltipSide: "top"
            onClicked: ct.expand()
        }
    }
    VideoNotesModel {   // the markers before the clip ever activates (VideoTransport's recipe)
        id: barNotes
        mediaPath: ct.host ? ct.host.anchorPath : (ct.row >= 0 && blockModel.structureRevision >= 0) ? blockModel.mediaAnchorPath(ct.row) : ""
        fps: ct.host ? ct.host.fps : (ct.row >= 0 ? blockModel.mediaFps(ct.row) : 0)
    }
}
