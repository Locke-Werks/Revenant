// Audio.
//
// EVERY HEARD RECEIVER, MIXED. The engine serves an audio subscription
// per receiver and this window takes one on every receiver in the rack
// that is heard, and the player sums them with each strip's gain. This
// section is the master: the listen switch, the output and its volume,
// and the focused receiver's stream described in full. Mute, solo and
// gain per receiver are on the strips. docs/ui-spectrum.md, "The
// receiver rack", has what the mix decides and what it leaves open.
//
// WHAT THIS PARAGRAPH USED TO SAY: "ONE RECEIVER AT A TIME, AND IT IS
// THE PANE'S. The engine serves an audio subscription per receiver and
// this window takes exactly one, on whichever receiver the pane above
// holds", and that "Mixing two streams into them is a mixer with a gain
// and a pan per source". The rack is that mixer, with a gain and no pan.
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
import Revenant

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
    //
    // HIDDEN ON A RECEIVER THAT MAKES NO AUDIO: raw, D-STAR and TETRA
    // hand out complex baseband for a decoder, and the engine refuses
    // audio on them, so the section would only ever show the refusal.
    // The link does not subscribe there either. The switch keeps its
    // state underneath and is on screen again the moment the receiver
    // is back in a mode that makes audio, before anything plays. A P25
    // receiver shows it, and what plays is the decoded voice, silent
    // between calls and through an encrypted one.
    // WHAT THIS USED TO SAY: "raw, P25, D-STAR and TETRA hand out
    // complex baseband".
    visible: engineLink.receiverId > 0
             ? engineLink.audioOffered
             : (engineLink.audioWanted
                || engineLink.audioEndedReason.length > 0
                || engineLink.audioFault.length > 0
                || audioPlayer.fault.length > 0)

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 8

        Label {
            text: "audio"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeTitle
            font.bold: true
        }

        // The switch. It is what the operator asked for and stays
        // on across a retune, a mode change and a reconnect;
        // audioActive beside it is whether there is a stream.
        RButton {
            flat: true
            checkable: true
            checked: engineLink.audioWanted
            text: engineLink.audioWanted ? "listening" : "listen"
            tint: Theme.receiverColours[engineLink.focusedSlot]
            ink: Theme.inkDim
            onClicked: engineLink.audioWanted = !engineLink.audioWanted
        }

        // Which receiver, named rather than assumed, by the label its
        // strip carries. The switch can be on with nothing subscribed,
        // which is the ordinary state before anything is tuned, and the
        // focused receiver can be the one not heard, muted or soloed away.
        Label {
            text: engineLink.audioActive
                  ? "RX " + (engineLink.focusedSlot + 1)
                  : !engineLink.audioWanted ? ""
                  : engineLink.rackCount > 0 ? "focused receiver not heard"
                  : "no receiver"
            color: engineLink.audioActive ? Theme.ink : Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        RButton {
            flat: true
            checkable: true
            checked: audioPlayer.muted
            text: audioPlayer.muted ? "muted" : "mute"
            tint: Theme.inkWarn
            ink: Theme.inkDim
            onClicked: audioPlayer.muted = !audioPlayer.muted
        }

        RSlider {
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
            Component.onCompleted: value = audioPlayer.volume
            onMoved: audioPlayer.volume = value

            // WHAT THE COMMENT ABOVE USED TO SAY, in full: "a binding to
            // audioPlayer.volume is broken by the first drag anyway and a
            // half-live binding is worse than none. This control is the
            // only writer." The volume keys write it too, so a change that
            // did not come from this handle moves it here, except while the
            // handle is held, when it is the operator's.
            Connections {
                target: audioPlayer
                function onVolumeChanged() {
                    if (!volumeSlider.pressed)
                        volumeSlider.value = audioPlayer.volume
                }
            }
        }

        Label {
            text: Math.round(audioPlayer.volume * 100) + "%"
            color: audioPlayer.muted ? Theme.inkDim : Theme.ink
            font.pixelSize: Theme.sizeBody
        }

        RComboBox {
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

        // WHAT IS COMING OUT OF THE SPEAKER, in a word. Four of the
        // five are silence and they are four different things: see
        // FrameSource in ui/audio/audio_ring.h. It follows the frames
        // reaching the card and not the newest chunk off the wire, so
        // it is in step with what is audible. There was a sixth, the
        // player's own "format mismatch", until the sink opened at the
        // device's format on 2026-09-23.
        Readout {
            widest: "squelched"
            font.family: Theme.uiFont
            text: audioPlayer.source
            color: audioPlayer.source === "audio" ? Theme.inkTune
                   : audioPlayer.source === "squelched" ? Theme.inkDim
                   : audioPlayer.source === "waiting" ? Theme.inkDim
                   : Theme.inkWarn
            font.pixelSize: Theme.sizeBody
            font.bold: audioPlayer.source === "audio"
        }

    }

    // The three depths, so the derivation is on screen. The
    // grant is what the engine holds and what the ring is sized
    // from; the last figure is the sink's own buffer, which is
    // the only one of the three that is always latency.
    //
    // On a line of its own under the controls rather than at the end of
    // their row: in the receiver window that row is narrower than it was
    // across the main window, and the depths were the part squeezed out.
    Label {
        Layout.fillWidth: true
        font.family: Theme.monoFont
        visible: engineLink.audioActive
        text: engineLink.audioGrantedMillis + " ms granted  ·  "
              + audioPlayer.bufferedMillis + "/" + audioPlayer.ringMillis
              + " ms buffered  ·  " + audioPlayer.sinkMillis + " ms out"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
    }

    // Chips, each naming a problem with its sentence on hover. These were
    // five rows of text under the controls; the words are unchanged.
    Flow {
        Layout.fillWidth: true
        spacing: 6

        // The counters, and only when there is something to say. Split
        // by cause, because a wire drop, a starved card and a resync
        // sound alike and have three different fixes.
        StatusChip {
            visible: engineLink.audioActive
                     && (audioPlayer.gapEvents > 0 || audioPlayer.starvedFrames > 0
                         || audioPlayer.overrunFrames > 0
                         || engineLink.audioFramesDropped > 0)
            label: "dropouts"
            detail: "gaps " + audioPlayer.gapEvents + " ("
                  + audioPlayer.gapWireFrames + " wire, "
                  + audioPlayer.gapUpstreamFrames + " upstream frames, "
                  + audioPlayer.gapFilledFrames + " filled with silence)"
                  + "  ·  resyncs " + audioPlayer.resyncs
                  + "  ·  starved " + audioPlayer.starvedFrames + " frames"
                  + "  ·  overran " + audioPlayer.overrunFrames + " frames"
                  + "  ·  engine dropped " + engineLink.audioFramesDropped
                  + " in " + engineLink.audioDropEvents + " events"
            ink: Theme.inkWarn
        }

        // THE RECEIVER WENT AWAY. This is the sentence the whole ended
        // callback exists for: audio that simply stops is what a quiet
        // channel sounds like, so the engine says so and this is where
        // its words go. inkBad rather than inkWarn because the stream
        // is over and will not come back on its own.
        StatusChip {
            visible: engineLink.audioEndedReason.length > 0
            label: "audio stopped"
            detail: "the audio stopped: " + engineLink.audioEndedReason
                  + ". Tune a receiver and click listen again."
            ink: Theme.inkBad
        }

        // The engine refused the subscription, in its own words.
        StatusChip {
            visible: engineLink.audioFault.length > 0
            label: "audio refused"
            detail: engineLink.audioFault
            ink: Theme.inkWarn
        }

        // The sound card refused, or went away. Kept apart from the
        // line above it because one is fixed by picking another output
        // and the other is not.
        StatusChip {
            visible: audioPlayer.fault.length > 0
            label: "sound card"
            detail: audioPlayer.fault
            ink: Theme.inkWarn
        }

        // Something the player adapted, which is not a fault and is not
        // coloured as one: the focused receiver resampled to the device's
        // rate, which is every receiver RDS has raised to 171000 S/s and
        // every P25 receiver's 8000, and a mono stream copied to every
        // channel of a stereo device, which is the ordinary case.
        StatusChip {
            visible: audioPlayer.note.length > 0
            label: "adapted"
            detail: audioPlayer.note
            ink: Theme.inkDim
        }
    }
}
