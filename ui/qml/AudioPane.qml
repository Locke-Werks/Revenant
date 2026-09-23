// Audio.
//
// ONE RECEIVER AT A TIME, AND IT IS THE PANE'S. The engine serves an
// audio subscription per receiver and this window takes exactly one,
// on whichever receiver the pane above holds. That is stated here
// rather than left for an operator to infer from the absence of a
// second control: it is the same decision the pane already makes,
// that one pane shows one receiver, extended to the one pair of
// speakers a machine has. Mixing two streams into them is a mixer
// with a gain and a pan per source, and nothing here makes that
// decision on the operator's behalf.
//
// WHY THIS SECTION OUTLIVES THE PANE ABOVE IT. Its visibility is not
// receiverId alone. A receiver removed out from under a live stream
// is the one failure on this path with no symptom of its own, and
// the words explaining it would disappear with the pane exactly when
// they are needed. The same holds for a device that refused the
// format.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    Layout.fillWidth: true
    spacing: 2
    // audioWanted is in here and not only receiverId. The switch is
    // sticky across a retune, a mode change and a reconnect, which
    // is what makes listening survive a rebuild, and a sticky
    // switch that cannot be seen is one the operator cannot turn
    // off: clearing the receiver would hide it armed, and the next
    // receiver they tuned would start making noise with no control
    // on screen that explains why. Observed 2026-09-20 by clearing
    // a receiver while listening.
    visible: engineLink.receiverId > 0
             || engineLink.audioWanted
             || engineLink.audioEndedReason.length > 0
             || engineLink.audioFault.length > 0
             || audioPlayer.fault.length > 0

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            text: "audio"
            color: Theme.inkTune
            font.pixelSize: Theme.sizeTitle
            font.bold: true
        }

        // The switch. It is what the operator asked for and stays
        // on across a retune, a mode change and a reconnect;
        // audioActive beside it is whether there is a stream.
        Label {
            text: engineLink.audioWanted ? "listening" : "listen"
            color: engineLink.audioWanted ? Theme.inkTune : Theme.inkDim
            font.pixelSize: Theme.sizeBody
            font.bold: engineLink.audioWanted

            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                cursorShape: Qt.PointingHandCursor
                onClicked: engineLink.audioWanted = !engineLink.audioWanted
            }
        }

        // Which receiver, named rather than assumed. The switch can
        // be on with nothing subscribed, which is the ordinary
        // state before anything is tuned.
        Label {
            text: engineLink.audioActive
                  ? "vfo " + engineLink.audioReceiverId
                  : (engineLink.audioWanted ? "no receiver" : "")
            color: engineLink.audioActive ? Theme.ink : Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        Label {
            text: audioPlayer.muted ? "muted" : "mute"
            color: audioPlayer.muted ? Theme.inkWarn : Theme.inkDim
            font.pixelSize: Theme.sizeBody
            font.bold: audioPlayer.muted

            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                cursorShape: Qt.PointingHandCursor
                onClicked: audioPlayer.muted = !audioPlayer.muted
            }
        }

        Slider {
            id: volumeSlider

            Layout.preferredWidth: 110
            from: 0.0
            to: 1.0
            enabled: !audioPlayer.muted

            // A step, and it is load-bearing rather than
            // cosmetic. The volume is persisted on every distinct
            // value and QSettings on Windows reaches the registry
            // per setValue, so a continuous slider would be one
            // registry write per frame of a drag. A hundredth is
            // below anything audible on a perceptual curve and
            // bounds a full-travel drag at a hundred writes.
            stepSize: 0.01

            // Seeded once rather than bound, because a binding to
            // audioPlayer.volume is broken by the first drag
            // anyway and a half-live binding is worse than none.
            // This control is the only writer.
            Component.onCompleted: value = audioPlayer.volume
            onMoved: audioPlayer.volume = value
        }

        Label {
            text: Math.round(audioPlayer.volume * 100) + "%"
            color: audioPlayer.muted ? Theme.inkDim : Theme.ink
            font.pixelSize: Theme.sizeBody
        }

        ComboBox {
            id: deviceBox

            Layout.preferredWidth: 220
            model: audioPlayer.devices
            font.pixelSize: Theme.sizeBody
            onActivated: audioPlayer.device = currentIndex

            // NOT currentIndex: audioPlayer.device. ComboBox writes
            // its own currentIndex when the user picks, and a
            // direct assignment to a property destroys the binding
            // on it, so the picker followed audioPlayer.device
            // exactly until the first selection and never again.
            // After that, a device pulled out of its socket moved
            // the property and left the picker showing a device
            // that has gone.
            //
            // A Binding object is the standard answer: it WRITES
            // the value rather than installing a binding on the
            // property, so the ComboBox's own assignment does not
            // destroy it and it reasserts on the next change.
            Binding {
                target: deviceBox
                property: "currentIndex"
                value: audioPlayer.device
                restoreMode: Binding.RestoreNone
            }

            // And a reassert for the case the Binding cannot see:
            // refresh_devices can rebuild the list without moving
            // the selection, ComboBox resets currentIndex to 0 on
            // a model change, and audioPlayer.device has not
            // changed so nothing above re-fires.
            onCountChanged: currentIndex = audioPlayer.device
        }

        Item { Layout.fillWidth: true }

        // WHAT IS COMING OUT OF THE SPEAKER, in a word or two. Five
        // of the six are silence and they are five different
        // things: see FrameSource in ui/audio/audio_ring.h for the
        // first five and the source property in
        // ui/audio/audio_player.h for "format mismatch", which is
        // the player's own. It follows the frames reaching the card
        // and not the newest chunk off the wire, so it is in step
        // with what is audible.
        Label {
            text: audioPlayer.source
            color: audioPlayer.source === "audio" ? Theme.inkTune
                   : audioPlayer.source === "squelched" ? Theme.inkDim
                   : audioPlayer.source === "waiting" ? Theme.inkDim
                   : Theme.inkWarn
            font.pixelSize: Theme.sizeBody
            font.bold: audioPlayer.source === "audio"
        }

        // The three depths, so the derivation is on screen. The
        // grant is what the engine holds and what the ring is sized
        // from; the last figure is the sink's own buffer, which is
        // the only one of the three that is always latency.
        Label {
            visible: engineLink.audioActive
            text: engineLink.audioGrantedMillis + " ms granted  ·  "
                  + audioPlayer.bufferedMillis + "/" + audioPlayer.ringMillis
                  + " ms buffered  ·  " + audioPlayer.sinkMillis + " ms out"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
    }

    // The counters, and only when there is something to say. Split
    // by cause, because a wire drop, a starved card and a resync
    // sound alike and have three different fixes.
    Label {
        Layout.fillWidth: true
        visible: engineLink.audioActive
                 && (audioPlayer.gapEvents > 0 || audioPlayer.starvedFrames > 0
                     || audioPlayer.overrunFrames > 0
                     || engineLink.audioFramesDropped > 0)
        text: "gaps " + audioPlayer.gapEvents + " ("
              + audioPlayer.gapWireFrames + " wire, "
              + audioPlayer.gapUpstreamFrames + " upstream frames, "
              + audioPlayer.gapFilledFrames + " filled with silence)"
              + "  ·  resyncs " + audioPlayer.resyncs
              + "  ·  starved " + audioPlayer.starvedFrames + " frames"
              + "  ·  overran " + audioPlayer.overrunFrames + " frames"
              + "  ·  engine dropped " + engineLink.audioFramesDropped
              + " in " + engineLink.audioDropEvents + " events"
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeSmall
        elide: Text.ElideRight
    }

    // THE RECEIVER WENT AWAY. This is the sentence the whole ended
    // callback exists for: audio that simply stops is what a quiet
    // channel sounds like, so the engine says so and this is where
    // its words go. inkBad rather than inkWarn because the stream
    // is over and will not come back on its own.
    Label {
        Layout.fillWidth: true
        visible: engineLink.audioEndedReason.length > 0
        text: "the audio stopped: " + engineLink.audioEndedReason
              + ". Tune a receiver and click listen again."
        color: Theme.inkBad
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }

    // The engine refused the subscription, in its own words.
    Label {
        Layout.fillWidth: true
        visible: engineLink.audioFault.length > 0
        text: engineLink.audioFault
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }

    // The sound card refused, or went away. Kept apart from the
    // line above it because one is fixed by picking another output
    // and the other is not.
    Label {
        Layout.fillWidth: true
        visible: audioPlayer.fault.length > 0
        text: audioPlayer.fault
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }

    // Something the player adapted, which is not a fault and is not
    // coloured as one. Today the only one is a mono stream copied
    // onto a device that takes no mono, which is the ordinary case
    // on a machine whose default output is stereo only.
    Label {
        Layout.fillWidth: true
        visible: audioPlayer.note.length > 0
        text: audioPlayer.note
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }
}
