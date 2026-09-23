// RDS.
//
// THE HEALTH IS NOT AN EXTRA, IT IS HALF THE PANE. A decoder that
// never locked and a station that carries no RDS produce the same
// empty struct and want different actions, and so do a decoder
// that faulted and one that is being fed nothing across a retune.
// models/rds_view.h decides which of the four it is, in the order
// core/rpc/types.h asks for, and rdsStatus is always populated
// while the switch is on. There is no state in which this pane is
// blank and says nothing.
//
// The switch exists because the first poll BUILDS the decoder on
// that receiver. It also REBUILDS the receiver, at 171000 S/s, after
// asking the engine on a throwaway receiver whether that rate can be
// granted: 57 kHz does not survive the 48 kHz default and the switch
// refused every time until it did this. See the block above the RDS
// surface in ui/models/engine_link.h and ui/models/composite_probe.h.
//
// WHICH IS WHY THERE ARE TWO SENTENCES HERE AND NOT ONE. rdsStatus is
// about the decoder and always present. The line below it is about the
// AUDIO, which has become the multiplex, and an operator listening to
// the station gets no other account of why it stopped sounding like
// one: audioActive is still true and the level meter still moves.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    Layout.fillWidth: true
    spacing: 2
    visible: engineLink.receiverId > 0 || engineLink.rdsWanted

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            text: "rds"
            color: engineLink.rdsWanted ? Theme.inkTune : Theme.inkDim
            font.pixelSize: Theme.sizeTitle
            font.bold: true

            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                cursorShape: Qt.PointingHandCursor
                onClicked: engineLink.rdsWanted = !engineLink.rdsWanted
            }
        }

        // A SETTING AND NEVER AN INFERENCE. core/rpc/types.h and
        // core/decode/rds_groups.h both say it: the PI cannot
        // decide the region, because the US call sign range
        // collides with European country codes, and getting it
        // wrong is silent. So it is two words the operator picks,
        // and the one in force is the filled one.
        RSegmented {
            visible: engineLink.rdsWanted
            options: ["rds", "rbds"]
            current: engineLink.rdsRegion
            onPicked: (region) => engineLink.rdsRegion = region
        }

        // The station, which is the whole point and is only shown
        // once groups are actually arriving.
        Label {
            visible: engineLink.rdsDecoding
                     && engineLink.rdsIdentity.length > 0
            text: engineLink.rdsIdentity
            color: Theme.ink
            font.pixelSize: Theme.sizeTitle
            font.bold: true
        }

        Label {
            visible: engineLink.rdsDecoding && engineLink.rdsPs.length > 0
            text: "“" + engineLink.rdsPs + "”"
                  + (engineLink.rdsPsSegments < engineLink.rdsPsSegmentsTotal
                     ? "  " + engineLink.rdsPsSegments + "/"
                       + engineLink.rdsPsSegmentsTotal + " segments"
                     : "")
            color: Theme.ink
            font.pixelSize: Theme.sizeTitle
        }

        Label {
            visible: engineLink.rdsDecoding
                     && engineLink.rdsProgrammeType.length > 0
            text: engineLink.rdsProgrammeType
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        // TP and TA, each behind its own validity flag, because
        // core/rpc/types.h is explicit that "no traffic
        // announcement" and "no 0A group has arrived" are
        // different claims and the struct's default is the second
        // one.
        Label {
            visible: engineLink.rdsDecoding && engineLink.rdsTpValid
                     && engineLink.rdsTp
            text: "TP"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            font.bold: true
        }

        Label {
            visible: engineLink.rdsDecoding && engineLink.rdsTaValid
                     && engineLink.rdsTa
            text: "TA"
            color: Theme.inkWarn
            font.pixelSize: Theme.sizeBody
            font.bold: true
        }

        Item { Layout.fillWidth: true }

        // The physical layer, which is what says whether to
        // believe any of the above. The bit rate is nominally
        // 1187.5 and the offset is the transmitter's drift within
        // the six hertz clause 1.1 allows, so both are printed to
        // the precision the measurement has.
        Label {
            visible: engineLink.rdsDecoding
            text: engineLink.rdsGroups + " groups  ·  "
                  + engineLink.rdsBitRateHz.toFixed(2) + " bit/s  ·  "
                  + engineLink.rdsCarrierOffsetHz.toFixed(1) + " Hz offset"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
    }

    // RadioText on its own row, because it is up to 64 characters
    // and sharing a row with the call sign would elide one of
    // them away.
    Label {
        Layout.fillWidth: true
        visible: engineLink.rdsDecoding
                 && engineLink.rdsRadioText.length > 0
        text: engineLink.rdsRadioText
              + (engineLink.rdsRtSegments < engineLink.rdsRtSegmentsTotal
                 ? "   (" + engineLink.rdsRtSegments + "/"
                   + engineLink.rdsRtSegmentsTotal + " segments)"
                 : "")
        color: Theme.ink
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }

    // ALWAYS PRESENT WHILE THE SWITCH IS ON. This is the sentence
    // that tells a decoder that never locked from a station with
    // no RDS, and a refused poll from either. An empty pane cannot
    // tell you which, which is the whole reason this row is not
    // gated on there being something to show.
    Label {
        Layout.fillWidth: true
        visible: engineLink.rdsWanted && engineLink.rdsStatus.length > 0
        text: engineLink.rdsStatus
        color: engineLink.rdsIsFault ? Theme.inkWarn : Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
        maximumLineCount: 3
        elide: Text.ElideRight
    }

    // WHAT THE AUDIO HAS BECOME, and only while somebody is listening
    // to it. The receiver hands out the 171 kHz composite multiplex
    // while RDS is on, which is the subcarrier the decoder needs and
    // is not programme audio, so a listener hears the station replaced
    // by noise with nothing on screen accounting for it.
    //
    // Gated on audioWanted rather than shown whenever the rate is
    // raised. An operator who is not listening does not need to be
    // told what the audio sounds like, and the rate is the switch
    // working rather than a condition to warn about.
    //
    // inkWarn, because it is a consequence the operator did not ask
    // for and has to act on to undo: the fix is the RDS switch, which
    // puts the receiver back to programme audio on the way off.
    Label {
        Layout.fillWidth: true
        visible: engineLink.rdsCompositeReceiver && engineLink.audioWanted
        text: "the audio on this receiver is the FM multiplex while RDS is "
              + "on, not programme audio. Turn rds off to hear the station."
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }
}
