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
// AUDIO, which the receiver now hands out as the multiplex. The window
// plays that multiplex's programme band in mono, and the chip is the
// only account of why a stereo station went mono.
//
// WHAT THE PARAGRAPH ABOVE USED TO SAY, from its third sentence: "The line
// below it is about the AUDIO, which has become the multiplex, and an
// operator listening to the station gets no other account of why it stopped
// sounding like one". Until 2026-09-23 the multiplex went to the device as
// it came, and the owner's device refused its 171000 S/s outright.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: rds

    Layout.fillWidth: true
    spacing: 4
    // OFFERED ONLY WHERE IT CAN WORK. The section is there for a wfm
    // receiver whose granted filter passes the 57 kHz subcarrier, and absent
    // for every other receiver rather than greyed and explained: a switch that
    // can only refuse is a paragraph of refusal waiting to happen, and that is
    // what this pane had become. models/composite_probe.h has the rule.
    //
    // AND WHEREVER THE SWITCH IS ALREADY ON. It is sticky across a mode change,
    // so a receiver changed from wfm to nfm with RDS on still has the decoder
    // asking and the audio still the multiplex. Hiding the switch then would
    // leave it armed with no way to turn it off, so the section stays, with a
    // chip saying why nothing is decoding.
    readonly property bool offered: engineLink.receiverId > 0
                                    && UiRules.rdsOffered(engineLink.receiverDemod,
                                                          engineLink.receiverGrantedLow,
                                                          engineLink.receiverGrantedHigh)

    visible: offered || engineLink.rdsWanted

    // Narrow enough that the physical layer's line goes under the station.
    readonly property bool compact: width < 700

    // The decoder belongs to the focused receiver, so its switch and its
    // chips wear that receiver's colour, as the detail and decode sections do.
    readonly property color tint: Theme.receiverColours[engineLink.focusedSlot]

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 8

        RButton {
            flat: true
            checkable: true
            checked: engineLink.rdsWanted
            text: "rds"
            tint: rds.tint
            ink: Theme.inkDim
            font.bold: true
            onClicked: engineLink.rdsWanted = !engineLink.rdsWanted
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

        // ALWAYS PRESENT WHILE THE SWITCH IS ON. This is the chip that
        // tells a decoder that never locked from a station with no RDS, and a
        // refused poll from either, in a word or two; the sentence that says
        // so in full is its tooltip, verbatim. An empty pane cannot tell you
        // which, which is the whole reason this is not gated on there being
        // something to show. It used to be that sentence, printed in full
        // under the switch whatever it said.
        StatusChip {
            visible: engineLink.rdsWanted && engineLink.rdsLabel.length > 0
            label: engineLink.rdsLabel
            detail: engineLink.rdsStatus
            ink: engineLink.rdsIsFault ? Theme.inkWarn
                 : engineLink.rdsDecoding ? rds.tint
                 : Theme.inkDim
        }

        // WHAT THE AUDIO HAS BECOME, and only while somebody is listening
        // to it. The receiver hands out the 171 kHz composite multiplex
        // while RDS is on, which is the subcarrier the decoder needs, and
        // the mix plays its programme band: filtered to 15 kHz, clear of
        // the pilot, and de-emphasised at 75 us. That is the station in
        // mono. audio/audio_mix.h has it.
        //
        // Gated on audioWanted rather than shown whenever the rate is
        // raised. An operator who is not listening does not need to be
        // told what the audio sounds like, and the rate is the switch
        // working rather than a condition to warn about.
        //
        // WHAT THIS BLOCK USED TO SAY: that the multiplex "is not programme
        // audio, so a listener hears the station replaced by noise", and
        // that the chip was inkWarn for a consequence the operator had to
        // act on to undo. The consequence is the stereo image now, and the
        // chip is dim.
        // AN EMERGENCY WARNING GROUP IS SAID IN THE ROW, in the loudest ink,
        // beside the decoder's own state and ahead of the station's name. EN
        // 50067 has 9A sent very infrequently unless there is an emergency or
        // a test, so its arriving at all is the fact; the payload is each
        // country's own and sits in the detail as hexadecimal only.
        StatusChip {
            visible: engineLink.rdsEwsSent
            label: engineLink.rdsEwsLabel
            detail: engineLink.rdsEwsDetail
            ink: Theme.inkBad
        }

        StatusChip {
            visible: engineLink.rdsCompositeReceiver && engineLink.audioWanted
            label: "audio is mono"
            detail: "while RDS is on this receiver hands out the FM multiplex, and "
                    + "what plays is its programme band in mono, filtered to 15 kHz and "
                    + "de-emphasised. Turn rds off for stereo."
            ink: Theme.inkDim
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
        Readout {
            visible: engineLink.rdsDecoding && !rds.compact
            widest: physical.widest
            text: physical.text
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
    }

    // The same line on a row of its own where the one above has no room for
    // it: the docked strip's column beside the receiver.
    Readout {
        id: physical

        Layout.alignment: Qt.AlignRight
        visible: engineLink.rdsDecoding && rds.compact
        widest: "0000000 groups  ·  0000.00 bit/s  ·  -000.0 Hz offset"
        text: engineLink.rdsGroups + " groups  ·  "
              + engineLink.rdsBitRateHz.toFixed(2) + " bit/s  ·  "
              + engineLink.rdsCarrierOffsetHz.toFixed(1) + " Hz offset"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
    }

    // What the station says it is carrying: the programme type, its name, and
    // the RadioText, on a row of their own, because RadioText is up to 64
    // characters and sharing a row with the call sign would elide one of them
    // away. The programme type moved down here beside its name when the name
    // arrived, which the first row had no room left for.
    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: engineLink.rdsDecoding
                 && (engineLink.rdsProgrammeType.length > 0 || engineLink.rdsPtynShown
                     || engineLink.rdsRadioText.length > 0)
        spacing: 8

        Label {
            visible: engineLink.rdsProgrammeType.length > 0
            text: engineLink.rdsProgrammeType
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        // The programme type name, which refines the PTY beside it, run by
        // run so a segment that came through a repaired block is drawn in the
        // warning ink and underlined rather than read as the station's.
        Row {
            visible: engineLink.rdsPtynShown
            spacing: 0

            Label {
                text: "“"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody
            }

            Repeater {
                model: engineLink.rdsPtynRuns

                delegate: Label {
                    required property var modelData

                    text: modelData.text
                    color: modelData.corrected ? Theme.inkWarn : Theme.inkDim
                    font.underline: modelData.corrected
                    font.pixelSize: Theme.sizeBody
                }
            }

            Label {
                text: "”" + (engineLink.rdsPtynNote.length > 0
                             ? "  " + engineLink.rdsPtynNote : "")
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody
            }
        }

        StatusChip {
            visible: engineLink.rdsPtynShown && engineLink.rdsPtynCorrectedDetail.length > 0
            label: "corrected"
            detail: engineLink.rdsPtynCorrectedDetail
            ink: Theme.inkWarn
        }

        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            visible: engineLink.rdsRadioText.length > 0
            text: engineLink.rdsRadioText
                  + (engineLink.rdsRtSegments < engineLink.rdsRtSegmentsTotal
                     ? "   (" + engineLink.rdsRtSegments + "/"
                       + engineLink.rdsRtSegmentsTotal + " segments)"
                     : "")
            color: Theme.ink
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideRight
        }
    }

    // TMC, only where a service is on air or announced. Counts, and the
    // confirmed payloads behind a toggle as hexadecimal with no field named,
    // because the layout of events and locations is not implemented.
    // models/rds_services.h says which payloads count as confirmed.
    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: engineLink.rdsTmcShown
        spacing: 8

        StatusChip {
            label: engineLink.rdsTmcLabel
            detail: engineLink.rdsTmcDetail
            ink: rds.tint
        }

        Readout {
            visible: engineLink.rdsTmcCounts.length > 0
            widest: "0000000 groups  ·  000 confirmed  ·  000 heard once"
            text: engineLink.rdsTmcCounts
            font.pixelSize: Theme.sizeSmall
        }

        RButton {
            id: payloadsToggle

            visible: engineLink.rdsTmcPayloads.length > 0
            flat: true
            checkable: true
            text: "payloads"
            ink: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        Item { Layout.fillWidth: true }
    }

    Column {
        Layout.fillWidth: true
        visible: engineLink.rdsTmcShown && payloadsToggle.checked
                 && engineLink.rdsTmcPayloads.length > 0
        spacing: 1

        Repeater {
            model: engineLink.rdsTmcPayloads

            delegate: Text {
                required property string modelData

                text: modelData
                color: Theme.ink
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
            }
        }

        Text {
            visible: engineLink.rdsTmcNotListed > 0
            text: engineLink.rdsTmcNotListed + " more confirmed, not listed"
            color: Theme.inkDim
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeSmall
        }
    }
}
