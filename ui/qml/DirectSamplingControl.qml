// Direct sampling, beside the gain it switches off.
//
// Drawn only for a source that has the feature, which is an RTL-SDR, so a
// recording or a synthetic scene does not grow a control that could do
// nothing. The four segments are the modes core/source/direct_sampling.h
// defines; the readout after them is the ADC branch in force now, which under
// auto moves with the centre and is the only way to see which side of the
// tuner's floor the radio is on.
//
// PICKING ONE REOPENS THE SOURCE, which costs every receiver. That is the
// engine's terms rather than this control's: the mode decides what the
// engine will tune. The tooltip says so, so the cost is known before the
// click rather than discovered after it. The choice is kept per radio.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

RowLayout {
    id: direct

    spacing: 6

    readonly property var opened: engineLink.openedSource
    readonly property bool offered: engineLink.connected && engineLink.sourceOpen
                                    && opened.directSamplingAvailable === true
    readonly property bool v4: opened.upconverter === true
    readonly property string mode: opened.directSampling === undefined
                                   ? "off" : String(opened.directSampling)
    readonly property string branch: engineLink.sourceDirectBranch

    visible: offered

    HoverHandler { id: hover }

    Label {
        text: "direct"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
    }

    RSegmented {
        options: ["off", "i", "q", "auto"]
        current: direct.mode
        onPicked: (option) => engineLink.setDirectSampling(option)
    }

    // The branch in force, which is only news when it differs from the mode:
    // under auto, or a fixed branch that has not taken yet.
    Readout {
        widest: "Q branch"
        text: direct.branch === "q" ? "Q branch"
              : direct.branch === "i" ? "I branch"
              : "tuner"
        color: direct.branch === "off" ? Theme.inkDim : Theme.ink
        horizontalAlignment: Text.AlignLeft
    }

    Tip {
        visible: hover.hovered
        text: (direct.v4
               ? "RTL-SDR Blog V4: this board reaches HF through its own upconverter, so off "
                 + "and auto both use the tuner path at every frequency. q and i bypass the "
                 + "tuner and are unlikely to hear anything on a V4.\n\n"
               : "q takes the antenna straight into the ADC, bypassing the tuner, which is "
                 + "how an RTL-SDR v3 hears HF up to 28.8 MHz. The tuner's gain does nothing "
                 + "there and images appear. auto uses q below the tuner's lowest frequency "
                 + "and returns to the tuner 2 MHz above it.\n\n")
              + "Changing the mode reopens the radio, which removes every receiver."
    }
}
