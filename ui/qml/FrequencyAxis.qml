// The frequency axis, in absolute hertz, under the spectrum and the waterfall.
// Aligned with the two items above rather than merely near them:
// all three fill this same width, so a fraction of this item is the
// same fraction of the span they drew, and a tick sits over the
// column it names.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Item {
    id: axis

    Layout.fillWidth: true
    Layout.preferredHeight: 22
    visible: engineLink.connected && engineLink.spectrumEnabled

    // Five where there is room, which is the count
    // tools/cli/main.cpp draws, and fewer where there is not. A
    // label is seven digits and a point at 11 pixels, and the last
    // one carries " MHz" as well, so 110 apart leaves clear air
    // between them. Two labels that touch are worse than three that
    // do not: a frequency axis is read by matching a tick to a
    // column, and a smudge is not a tick.
    readonly property int tickCount:
        Math.max(2, Math.min(5, Math.floor(axis.width / 110)))

    // Absolute megahertz at a fraction of the drawn span, as a tick label.
    //
    // engineLink.connected and engineLink.bins are read here so that a
    // binding calling this follows the link: frequencyAtFraction is a
    // method, and QML captures the properties a binding touches while it
    // evaluates, which cannot see inside a C++ call. Without one of them in
    // the expression the axis would keep the previous engine's numbers
    // after a reconnect, which is the one failure an axis must not have.
    function tickMhz(fraction) {
        if (!engineLink.connected || engineLink.bins <= 0)
            return ""
        return (engineLink.frequencyAtFraction(fraction) / 1.0e6).toFixed(4)
    }

    function fractionAt(index) {
        return axis.tickCount > 1 ? index / (axis.tickCount - 1) : 0.0
    }

    Repeater {
        model: axis.tickCount

        Rectangle {
            required property int index

            width: 1
            height: 4
            y: 0
            // The last tick belongs at the right-hand edge of the
            // last column, which is one pixel outside the item, so
            // it is pulled back in rather than drawn off the end.
            x: Math.min(Math.round(axis.fractionAt(index) * axis.width),
                        axis.width - 1)
            color: Theme.inkDim
        }
    }

    Repeater {
        model: axis.tickCount

        Label {
            required property int index

            readonly property real fraction: axis.fractionAt(index)

            // The unit rides on the last label, the way the CLI's
            // axis line ends in one, rather than taking a label
            // position of its own.
            text: axis.tickMhz(fraction)
                  + (index === axis.tickCount - 1 ? " MHz" : "")
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            y: 6
            // Centred on the tick, then pulled back inside the item
            // rather than clipped, so the two end labels stay whole.
            x: Math.max(0, Math.min(axis.width - width,
                                    fraction * axis.width - width / 2))
        }
    }
}
