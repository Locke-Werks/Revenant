// The focused receiver's controls: its dial, its mode, its filter, and the
// fine-tuning display the filter is dragged on.
//
// This was the VFO pane under the waterfall in the main window. It is the
// receiver window's now, so it can sit on another screen with the spectrum
// full-bleed on this one, and its controls carry the receiver's colour so they
// read as the same thing as its marker on the span and its strip in the rack.
//
// The display is the air around the receiver, from the receiver's own
// display tap with none of its filter in it, and the two rules over it
// are that receiver's filter edges. The edges are dragged here, which is
// the one interaction in this window that changes what the engine is
// doing rather than what the window is showing.
//
// WHAT THE FIRST SENTENCE USED TO SAY: "The display is the receiver's OWN
// passband, transformed by the engine", which was the fine stream after the
// filter, so the filter's shape was in the picture as well as over it.
//
// The controls appear when a receiver exists and not before. There is
// nothing to draw and nothing to drag without one, and an empty
// pane with handles in it invites a gesture that cannot do
// anything.
//
// WHAT IS NOT HERE. The brief for this panel asks for squelch by default and
// AGC behind the expansion. Neither is a parameter the engine offers on a
// receiver today, rpc::VrxParams has no field for either, so there is no
// control to draw; the expansion says so rather than offering one that could
// only refuse.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: detail

    readonly property color tint: Theme.receiverColours[0]
    property bool expanded: false

    spacing: 6
    visible: engineLink.receiverId > 0

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 12

        // The receiver's own dial, in the receiver's colour. Its limits are
        // the span, because a receiver outside the span is one the engine
        // removes. A step is a move by hand, so it goes through tuneReceiver
        // and not the detection entry point: there is no measurement behind
        // where the operator wheeled it to.
        FrequencyDial {
            value: engineLink.receiverCenterHz
            low: engineLink.spanLowHz
            high: engineLink.spanHighHz
            tint: detail.tint
            onStepped: (hz) => engineLink.tuneReceiver(hz, "")
            onTyped: (text) => {
                const hz = engineLink.parseHz(text)
                if (hz > 0)
                    engineLink.tuneReceiver(hz, "")
            }
        }

        // Automatic frequency tracking, off until ticked. Disabled on the
        // modes with no centre to aim at, and the chip beside it says what
        // the loop is doing, with its rules and its limits on hover. Left of
        // the spacer, so the chip's text changing moves nothing else.
        RCheckBox {
            text: "AFT"
            enabled: engineLink.aftOffered
            checked: engineLink.aftEnabled
            onToggled: engineLink.aftEnabled = checked
        }

        StatusChip {
            ink: !engineLink.aftEnabled ? Theme.inkDim
                 : engineLink.aftState === "correcting" ? Theme.accent
                 : engineLink.aftState === "holding, signal jumped" ? Theme.inkWarn
                 : Theme.inkDim
            label: !engineLink.aftOffered ? "no AFT on " + engineLink.receiverDemod
                   : !engineLink.aftEnabled ? "AFT off"
                   : engineLink.aftState
                     + (engineLink.aftHasError
                        ? "  " + (engineLink.aftErrorHz >= 0 ? "+" : "")
                          + Math.round(engineLink.aftErrorHz) + " Hz" : "")
            detail: "AFT nudges the receiver so a drifting signal stays in the filter. "
                    + "AM follows the carrier's peak, CW the peak while the key is down, "
                    + "NFM and WFM the centre of the occupied band averaged over two "
                    + "seconds. USB, LSB and DSB have no carrier to follow, so it is "
                    + "not offered there. It moves at most 100 Hz every half second, "
                    + "ignores errors under 30 Hz, holds with no signal, refuses to "
                    + "chase a jump, and stands aside for three seconds whenever you tune. "
                    + "RTTY and FSK are not tracked: their centre is the midpoint of two "
                    + "tones, and the engine has no RTTY mode or shift to derive it from, "
                    + "so on them it would follow one tone or wobble between both."
        }

        Item { Layout.fillWidth: true }

        RButton {
            flat: true
            text: "remove"
            ink: Theme.inkDim
            onClicked: engineLink.removeReceiver()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 12

        // The mode, as the eight choices an operator actually reaches for.
        // Changing one is a remove and an add underneath, because the
        // demodulator is the stage; the receiver keeps its identity across
        // that and the operator sees a mode change.
        RSegmented {
            options: ["am", "nfm", "wfm", "usb", "lsb", "dsb", "cw", "raw"]
            current: engineLink.receiverDemod
            tint: detail.tint
            onPicked: (mode) => engineLink.setReceiverDemod(mode)
        }

        // The bandwidth, as a number and two buttons that do what the up and
        // down arrows do over the display. A hundred hertz a press, the
        // arrows' shift step, because a press is a coarser gesture than a key
        // held down.
        Label {
            text: "width"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        Readout {
            widest: "0000.00 kHz"
            text: ((engineLink.receiverPassbandHigh - engineLink.receiverPassbandLow) / 1000)
                  .toFixed(2) + " kHz"
            color: Theme.ink
        }

        RButton {
            text: "−"
            onClicked: passband.widenPassband(-100)
        }

        RButton {
            text: "+"
            onClicked: passband.widenPassband(100)
        }

        RButton {
            flat: true
            text: "default"
            ink: Theme.inkDim
            onClicked: passband.resetPassband()
        }

        Item { Layout.fillWidth: true }

        Readout {
            widest: "-000.0 dBFS"
            text: engineLink.receiverLevelDbfs > -199
                  ? engineLink.receiverLevelDbfs.toFixed(1) + " dBFS" : ""
        }

        RButton {
            flat: true
            text: detail.expanded ? "filter ▾" : "filter ▸"
            ink: Theme.inkDim
            onClicked: detail.expanded = !detail.expanded
        }
    }

    // What the filter actually is, one gesture away: the demodulation rate
    // the audio is made at, the channel's limit on an edge, and the engine's
    // grant when it differs from the request. The display's span is not on
    // this list: it is half the display rate, off the frame's own geometry,
    // and steps only at a rung. (This used to say the demodulation rate "sets
    // the display's span", which it did while the pane was the fine stream.)
    GridLayout {
        Layout.fillWidth: true
        visible: detail.expanded
        columns: 2
        columnSpacing: 12
        rowSpacing: 2

        Label {
            text: "demodulation rate"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        Label {
            text: engineLink.receiverDemodRate > 0
                  ? (engineLink.receiverDemodRate / 1000).toFixed(1) + " kS/s"
                  : "not reported yet"
            color: Theme.ink
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeSmall
        }

        Label {
            text: "edge limit"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        Label {
            text: engineLink.receiverEdgeLimit > 0
                  ? "±" + (engineLink.receiverEdgeLimit / 1000).toFixed(3) + " kHz"
                  : "not reported yet"
            color: Theme.ink
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeSmall
        }

        Label {
            text: "granted"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        Label {
            text: (engineLink.receiverGrantedLow / 1000).toFixed(3) + " to "
                  + (engineLink.receiverGrantedHigh / 1000).toFixed(3) + " kHz"
                  + (engineLink.receiverClamped ? ", clamped by the channel" : "")
            color: engineLink.receiverClamped ? Theme.inkWarn : Theme.ink
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeSmall
        }

        Label {
            text: "squelch, AGC"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        Label {
            text: "not parameters the engine offers on a receiver yet"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
    }

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 140

        PassbandItem {
            id: passband
            anchors.fill: parent
            link: engineLink
        }

        Plate {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 4
            visible: !engineLink.passbandActive
            text: "waiting for the first passband frame"
        }

        Row {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 6
            spacing: 6

        // WHY THIS RECEIVER IS WRONG FOR THIS SIGNAL, when it is.
        //
        // Two mismatches bit on 2026-09-20 and neither said anything.
        // A click on a 145 kHz detection produced a 16 kHz NFM
        // receiver, and every number on screen was individually
        // correct: the detection row said 145 kHz, the readout above
        // said 16 kHz, and nothing put them beside each other. A
        // second receiver asked for 200 kHz and got 71, which the
        // overlay answered with a dimmer shade of the same colour.
        //
        // The arithmetic and the wording are in
        // models/receiver_match.h with their own cases in ui/tests.
        // It is a chip on the display now, with the sentence as its
        // tooltip: as a row it was on screen whenever a receiver was tuned
        // to anything narrower than its filter, which is most detection
        // clicks. This decides the colour, and it is inkWarn rather than
        // inkBad: the receiver works, it is pointed at the wrong
        // shape of thing, and that is something the operator fixes
        // with the mode buttons or the handles above.
            StatusChip {
                visible: engineLink.receiverFitLabel.length > 0
                label: engineLink.receiverFitLabel
                detail: engineLink.receiverFitText
            }

            // What the engine said about this receiver the last time it
            // refused or rebuilt it, in its own words on hover.
            StatusChip {
                visible: engineLink.receiverFault.length > 0
                label: "engine note"
                detail: engineLink.receiverFault
            }
        }
    }

    // The receiver's own history, on the same frames as the display above
    // and at the same width, so a carrier's column here is under its peak
    // there. Kept across receiver moves; see render/passband_waterfall_item.h.
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 80

        PassbandWaterfallItem {
            id: passbandWaterfall
            anchors.fill: parent
            link: engineLink
        }

        // Where the history ends, as on the span waterfall, so an empty
        // lower half reads as not yet filled rather than as silence.
        Rectangle {
            visible: passbandWaterfall.historyFraction > 0.0
                     && passbandWaterfall.historyFraction < 1.0
            y: Math.round(parent.height * passbandWaterfall.historyFraction)
            width: parent.width
            height: 1
            color: Theme.border
        }
    }

    // The readout, which is the whole of what a drag says back.
    // Live while one is running and the granted pair when one is
    // not, so the strip never goes blank and never shows a stale
    // gesture. The colour is the one thing this file decides: the
    // item reports that an edge is against the channel limit and
    // the window chooses how loudly to say so.
    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 8

        Label {
            Layout.minimumWidth: 0
            text: passband.readout
            color: passband.atLimit ? Theme.inkWarn
                   : engineLink.receiverClamped ? Theme.inkWarn : Theme.ink
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeBody
            font.bold: passband.dragging
            elide: Text.ElideRight
        }

        Item { Layout.fillWidth: true }

        Label {
            Layout.minimumWidth: 0
            text: "drag an edge, shift-drag to widen both, [ ] \\ select, "
                  + "arrows move, up and down widen, home resets, the wheel tunes"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            elide: Text.ElideRight
        }
    }

}
