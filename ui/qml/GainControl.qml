// The front end's gain, in the control row under the top bar.
//
// MOVED HERE FROM THE RADIO PANEL on 2026-09-23, when the owner asked for the
// row that used to hold the fault strip to hold a gain slider and the
// detector's sliders instead. Gain is the control an operator reaches for
// while watching the band, often, and a panel over the span hid the band it
// was being judged against. The radio panel no longer draws one, so there is
// one slider and one handle for the tuner's gain.
//
// Everything it says about the stage is the link's, and the arithmetic is
// ui/models/gain_control.h with its cases in ui/tests: where the handle sits,
// the step one key moves, the ticks at the tuner's own steps, and why a source
// with no stage has none to set.
//
// LABELLED WITH THE STAGE'S OWN NAME rather than "gain", because that is what
// it drives: on an R820T the one stage moves the LNA, the mixer and the VGA
// together, and a device with three separate stages would want three controls
// rather than one lying label. "+2" beside it admits a device reporting more
// stages than this row drives.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

RowLayout {
    id: gain

    spacing: 6

    readonly property bool hasStage: engineLink.connected && engineLink.sourceOpen
                                     && engineLink.sourceGainStage !== ""

    // Why there is nothing to set, word and sentence, both empty while there
    // is a stage. A recording, a synthetic scene and no source at all are
    // three reasons, and a slider greyed out with none is a broken control.
    readonly property var absence: UiRules.gainAbsence(
        engineLink.connected && engineLink.sourceOpen, gain.hasStage,
        engineLink.openedSource.backend === undefined ? "" : String(engineLink.openedSource.backend))

    HoverHandler { id: hover }

    Label {
        Layout.minimumWidth: 0
        text: gain.hasStage ? engineLink.sourceGainStage : "gain"
        color: gain.hasStage ? Theme.inkDim : Theme.inkOff
        font.pixelSize: Theme.sizeSmall
        elide: Text.ElideRight
    }

    RSlider {
        id: slider

        Layout.preferredWidth: 110
        from: 0.0
        to: 1.0

        // Zero on a continuous stage, which Slider reads as no stepping. See
        // gain_fraction_step for why the size comes off the count of steps
        // and not off the distance between two of them.
        stepSize: engineLink.sourceGainStep
        snapMode: Slider.SnapAlways
        enabled: gain.hasStage && !engineLink.sourceGainAuto

        // THE HANDLE FOLLOWS THE DEVICE, NOT THE POINTER. The binding is
        // restored whenever the link answers, so a step the tuner rounded to
        // shows up as the handle settling onto it rather than staying where
        // it was let go.
        value: engineLink.sourceGainFraction

        onMoved: engineLink.setSourceGainFraction(value)

        // A tick at each step the tuner has, under the groove. Unevenly
        // spaced because the steps are: they show where the tuner can land.
        Repeater {
            model: gain.hasStage ? engineLink.sourceGainTicks : []

            Rectangle {
                required property var modelData

                width: 1
                height: 3
                x: slider.leftPadding + 6 + Number(modelData) * (slider.availableWidth - 12)
                y: slider.topPadding + slider.availableHeight / 2 + 4
                color: slider.enabled ? Theme.inkDim : Theme.inkOff
            }
        }
    }

    // What the device took, and nothing before it has said. A number here
    // that was only ever a request is the lie sourceGainKnown exists to
    // prevent, so an untouched stage reads "unset" rather than a default.
    Readout {
        // The longest of the words gain_absence gives, so a recording's
        // reason fits as well as a figure does.
        widest: "recording"
        text: !gain.hasStage ? gain.absence.word
              : engineLink.sourceGainAuto ? "auto"
              : engineLink.sourceGainKnown ? engineLink.sourceGainDb.toFixed(1) + " dB"
              : "unset"
        color: gain.hasStage && (engineLink.sourceGainKnown || engineLink.sourceGainAuto)
               ? Theme.ink : Theme.inkDim
        horizontalAlignment: Text.AlignLeft
        elide: Text.ElideRight
    }

    // Offered only where the device will do it. Whether it should is the
    // operator's call: README.md has the measurement of what this dongle's
    // own AGC did to the detector's track list.
    RButton {
        visible: gain.hasStage && engineLink.sourceGainHasAuto
        flat: true
        text: engineLink.sourceGainAuto ? "manual" : "auto"
        ink: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        onClicked: engineLink.setSourceGainAuto(!engineLink.sourceGainAuto)
    }

    Label {
        visible: gain.hasStage && engineLink.sourceGainStages > 1
        text: "+" + (engineLink.sourceGainStages - 1)
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
    }

    // A refused gain change, in the source's own words on hover. A synthetic
    // scene says its emitter levels are set against the noise, which tells an
    // operator what to do instead.
    StatusChip {
        visible: engineLink.sourceGainFault !== ""
        label: "gain refused"
        detail: engineLink.sourceGainFault
        ink: Theme.inkWarn
    }

    Tip {
        parent: slider
        visible: hover.hovered
        delay: 400
        text: !gain.hasStage ? gain.absence.sentence
              : engineLink.sourceGainAuto
                ? "The device's own AGC is choosing the gain. Nothing on the wire says what it chose."
              : engineLink.sourceGainKnown
                ? "The step the tuner took, which is where the handle sits. Ticks are its steps."
              : "Nothing on the wire reports the source's gain until one is set from here."
    }
}
