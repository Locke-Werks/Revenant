// The focused receiver's controls: its dial, its mode, its filter, and the
// fine-tuning display the filter is dragged on.
//
// This was the VFO pane under the waterfall in the main window, then the
// receiver window's. It is the receivers panel's now, ReceiverPanel.qml,
// docked under the span or popped out onto another screen, and its controls
// carry the receiver's colour so they read as the same thing as its marker on
// the span and its strip in the rack.
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
// The brief for this panel asks for squelch by default and AGC behind the
// expansion. The AGC switch is behind the expansion, bound to agcEnabled.
// Squelch has no control here yet, and the expansion says so.
//
// WHAT THIS PARAGRAPH USED TO SAY: "Neither is a parameter the engine offers
// on a receiver today, rpc::VrxParams has no field for either". It had both,
// squelch_dbfs and the three agc fields, and the AGC ones did nothing until
// the engine applied them to what a person hears.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: detail

    readonly property color tint: Theme.receiverColours[engineLink.focusedSlot]
    property bool expanded: false

    // Too narrow for the mode selector and the filter's controls on one row,
    // which is the popped-out window at its default size and the docked strip
    // beside RDS, decoding and audio. The mode then has a row to itself.
    // Measured against the row's own contents: a mode row, the width and its
    // buttons, the level and the filter toggle come to a little over 800.
    readonly property bool compact: width < 840

    // The receiver's dial and the filter display, for the keys in
    // Commands.qml.
    readonly property alias dial: receiverDial
    readonly property alias passband: passband

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
            id: receiverDial
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

            // The rules, while the chip that carries them is not shown.
            Tip {
                visible: parent.hovered && !aftChip.visible
                text: aftChip.detail
            }
        }

        // Shown while AFT is on, saying what the loop is doing, or while the
        // mode rules it out, saying why the box is grey. Not while it is off
        // and could be on: the empty box already says that. WHAT IT USED TO
        // DO: stay on screen reading "AFT off" beside the empty box.
        StatusChip {
            id: aftChip

            visible: !engineLink.aftOffered || engineLink.aftEnabled
            ink: !engineLink.aftEnabled ? Theme.inkDim
                 : engineLink.aftState === "correcting" ? Theme.accent
                 : engineLink.aftState === "holding, signal jumped" ? Theme.inkWarn
                 : Theme.inkDim
            label: !engineLink.aftOffered
                   ? "no AFT on " + UiRules.modeLabel(engineLink.receiverDemod)
                   : engineLink.aftState
                     + (engineLink.aftHasError
                        ? "  " + (engineLink.aftErrorHz >= 0 ? "+" : "")
                          + Math.round(engineLink.aftErrorHz) + " Hz" : "")
            detail: "AFT nudges the receiver so a drifting signal stays in the filter. "
                    + "AM follows the carrier's peak, CW the peak while the key is down, "
                    + "NFM and WFM the centre of the occupied band averaged over two "
                    + "seconds. USB, LSB and DSB have no carrier to follow, so it is "
                    + "not offered there. It moves at most 100 Hz every half second, "
                    + "ignores errors under 30 Hz, holds with no signal, acts only on a "
                    + "signal it has seen in four separate looks, refuses to "
                    + "chase a jump, and stands aside for three seconds whenever you tune. "
                    + "RTTY and FSK are not tracked: their centre is the midpoint of two "
                    + "tones, and the engine has no RTTY mode or shift to derive it from, "
                    + "so on them it would follow one tone or wobble between both."
        }

        // The auto filter, off until ticked, in the same shape as AFT: a
        // tick box and a chip saying what the last fit did, with the rules on
        // hover. models/auto_filter.h has them.
        RCheckBox {
            text: "auto filter"
            enabled: engineLink.autoFilterOffered
            checked: engineLink.autoFilterEnabled
            onToggled: engineLink.autoFilterEnabled = checked

            Tip {
                visible: parent.hovered && !filterChip.visible
                text: filterChip.detail
            }
        }

        // On the same terms as AFT's chip.
        StatusChip {
            id: filterChip

            visible: !engineLink.autoFilterOffered || engineLink.autoFilterEnabled
            ink: !engineLink.autoFilterEnabled ? Theme.inkDim
                 : engineLink.autoFilterState.startsWith("fitted") ? Theme.accent
                 : Theme.inkDim
            label: !engineLink.autoFilterOffered
                   ? "no auto filter on " + UiRules.modeLabel(engineLink.receiverDemod)
                   : engineLink.autoFilterState
            detail: "Auto filter fits the filter to the signal once, when you click a "
                    + "detection to tune it and when you switch it on, from half a second "
                    + "of the display averaged. AM goes out to the outer sideband lines "
                    + "either side of the carrier, USB and LSB from the carrier to the far "
                    + "edge of the signal on the side it is on, CW and narrow data get a "
                    + "tight window on the tone, and NFM, WFM and DSB the measured occupied "
                    + "width. It never moves an edge you are dragging, leaves the filter "
                    + "alone when there is no signal, and stays inside what the channel "
                    + "allows."
        }

        Item { Layout.fillWidth: true }

        RButton {
            flat: true
            text: "remove"
            ink: Theme.inkDim
            onClicked: engineLink.removeReceiver()
        }
    }

    // The mode, then the filter's width and what else is set on it. One row
    // where there is room and two where there is not; see compact above.
    GridLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        columns: detail.compact ? 1 : 2
        columnSpacing: 12
        rowSpacing: 6

        // The mode: the eight choices an operator reaches for on every band
        // as a row, and P25, D-STAR, TETRA and DMR behind a "digital" segment at
        // its end, which reads the digital mode in force when there is one.
        // models/mode_choice.h holds the lists and the labels. Changing one
        // is a remove and an add underneath, because the demodulator is the
        // stage; the receiver keeps its identity across that and the
        // operator sees a mode change.
        RSegmented {
            options: UiRules.rowModes()
            overflow: UiRules.digitalModes()
            overflowText: UiRules.digitalGroupLabel(engineLink.receiverDemod)
            overflowChosen: UiRules.modeIsDigital(engineLink.receiverDemod)
            current: engineLink.receiverDemod
            tint: detail.tint
            onPicked: (mode) => engineLink.setReceiverDemod(mode)
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 8

            // The bandwidth, as a number and two buttons that do what the up
            // and down arrows do over the display. A hundred hertz a press,
            // the arrows' shift step, because a press is a coarser gesture
            // than a key held down.
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
                implicitWidth: Theme.controlHeight
                text: "−"
                onClicked: passband.widenPassband(-100)
            }

            RButton {
                implicitWidth: Theme.controlHeight
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

            // What noise mitigation is on, while the panel that sets it is
            // closed. Nothing at all while none is, so a receiver nobody has
            // asked for noise mitigation on says nothing about it.
            Label {
                Layout.minimumWidth: 0
                visible: !detail.expanded && engineLink.noiseSummary.length > 0
                text: engineLink.noiseSummary
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
                elide: Text.ElideRight
            }

            RButton {
                flat: true
                checkable: true
                checked: detail.expanded
                text: detail.expanded ? "filter ▾" : "filter ▸"
                ink: Theme.inkDim
                tint: detail.tint
                onClicked: detail.expanded = !detail.expanded
            }
        }
    }

    // WHAT THE ROW ABOVE USED TO CARRY AS WELL: the receiver's level in dBFS,
    // to a tenth. The rack strip beside it shows the same level on its meter
    // and in figures, so it was said twice a hand's width apart.

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

        // The receiver AGC, on by default. The engine runs it on the five
        // amplitude-detected modes and levels only what is heard; off holds
        // the gain it had. On nfm and wfm the box is grey, because a
        // discriminator plays at a fixed level and there is nothing for it
        // to switch.
        Label {
            text: "AGC"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        RowLayout {
            spacing: 8
            RCheckBox {
                text: "on"
                enabled: engineLink.agcOffered
                checked: engineLink.agcEnabled
                onToggled: engineLink.agcEnabled = checked
            }
            Label {
                text: engineLink.agcOffered
                      ? (engineLink.agcEnabled
                         ? "holds what you hear at -12 dBFS; decoders are not affected"
                         : "off: the gain is held where it was")
                      : "no AGC on " + UiRules.modeLabel(engineLink.receiverDemod)
                        + ": it plays at a fixed level"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
            }
        }

        Label {
            text: "squelch"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        Label {
            text: "no control here yet"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        // Noise mitigation: four stages, each off until ticked, each greyed
        // out with its reason on a mode that does not offer it. The rules
        // are models/noise_controls.h's and docs/noise.md says what each one
        // does to a signal.
        Label {
            text: "noise blanker"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        RowLayout {
            spacing: 8
            RCheckBox {
                text: "on"
                enabled: engineLink.noiseBlankerOffered
                checked: engineLink.noiseBlanker
                onToggled: engineLink.noiseBlanker = checked
            }
            RSlider {
                Layout.preferredWidth: 120
                visible: engineLink.noiseBlankerOffered
                enabled: engineLink.noiseBlanker
                from: 3
                to: 40
                stepSize: 1
                value: engineLink.noiseBlankerThresholdDb
                onMoved: engineLink.noiseBlankerThresholdDb = value
            }
            Label {
                text: engineLink.noiseBlankerOffered
                      ? engineLink.noiseBlankerThresholdDb.toFixed(0) + " dB over the background"
                      : engineLink.noiseNote
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
            }
        }

        Label {
            text: "notch"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        RowLayout {
            spacing: 8
            RCheckBox {
                text: "on"
                enabled: engineLink.notchOffered
                checked: engineLink.notchEnabled
                onToggled: engineLink.notchEnabled = checked
            }
            // Where the notch sits, in the filter display's frame: hertz
            // from the carrier, across the passband the receiver has.
            RSlider {
                Layout.preferredWidth: 160
                visible: engineLink.notchOffered
                from: engineLink.receiverPassbandLow
                to: engineLink.receiverPassbandHigh
                stepSize: 10
                value: engineLink.notchHz
                onMoved: engineLink.notchHz = Math.round(value)
            }
            Label {
                text: engineLink.notchOffered
                      ? (engineLink.notchAudioHz / 1000).toFixed(2) + " kHz in the audio"
                      : engineLink.notchNote
                color: Theme.inkDim
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
            }
        }

        Label {
            visible: engineLink.notchOffered
            text: "notch depth, width"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        RowLayout {
            visible: engineLink.notchOffered
            spacing: 8
            RSlider {
                Layout.preferredWidth: 100
                from: 3
                to: 80
                stepSize: 1
                value: engineLink.notchDepthDb
                onMoved: engineLink.notchDepthDb = value
            }
            Label {
                text: engineLink.notchDepthDb.toFixed(0) + " dB"
                color: Theme.inkDim
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
            }
            RSlider {
                Layout.preferredWidth: 100
                from: 10
                to: 2000
                stepSize: 10
                value: engineLink.notchWidthHz
                onMoved: engineLink.notchWidthHz = Math.round(value)
            }
            Label {
                text: engineLink.notchWidthHz + " Hz"
                color: Theme.inkDim
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
            }
        }

        Label {
            text: "automatic notch"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        RowLayout {
            spacing: 8
            RCheckBox {
                text: "on"
                enabled: engineLink.autoNotchOffered
                checked: engineLink.autoNotch
                onToggled: engineLink.autoNotch = checked
            }
            Label {
                text: engineLink.autoNotchOffered
                      ? "removes a steady whistle; costs a voice a little, so off without one"
                      : engineLink.autoNotchNote
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
            }
        }

        Label {
            text: "noise reduction"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        RowLayout {
            spacing: 8
            RCheckBox {
                text: "on"
                enabled: engineLink.noiseBlankerOffered
                checked: engineLink.noiseReduction
                onToggled: engineLink.noiseReduction = checked
            }
            RSlider {
                Layout.preferredWidth: 120
                visible: engineLink.noiseBlankerOffered
                enabled: engineLink.noiseReduction
                from: 0
                to: 1
                stepSize: 0.05
                value: engineLink.noiseReductionStrength
                onMoved: engineLink.noiseReductionStrength = value
            }
            Label {
                text: engineLink.noiseBlankerOffered
                      ? "strength " + Math.round(engineLink.noiseReductionStrength * 100) + "%"
                      : engineLink.noiseNote
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
            }
        }
    }

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        // Low enough that the docked strip at its default height holds the
        // dial, the mode, this and the waterfall under it.
        Layout.minimumHeight: 96

        PassbandItem {
            id: passband
            anchors.fill: parent
            link: engineLink
        }

        // The keys are on the edges while the display has focus, and an
        // arrow key does something different with and without it, so the
        // display says which with an outline in the accent.
        Rectangle {
            anchors.fill: parent
            visible: passband.activeFocus
            color: "transparent"
            border.width: 1
            border.color: Theme.accent
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
        Layout.minimumHeight: 56

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

        // Filling the rest of the row and right-aligned in it, so that when
        // the row is short it is this that elides and not the readout.
        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            horizontalAlignment: Text.AlignRight
            // The keys by name from the table, so this line cannot teach a key
            // the display no longer takes.
            //
            // Shorter than it was, "drag an edge, shift-drag to widen both, the
            // wheel tunes, Ctrl+E puts the keys on the edges, F1 lists them",
            // which was cut off at the receiver window's own default width.
            text: "drag an edge  ·  shift-drag both  ·  wheel tunes  ·  "
                  + KeyMap.keysText("filter.keys") + " for keys"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            elide: Text.ElideRight
        }
    }

}
