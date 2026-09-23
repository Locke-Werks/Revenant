// The frequency ruler, between the spectrum and the waterfall.
//
// It replaces the axis that used to sit under the waterfall. Between the two
// displays it is next to both of the things it labels, and the eye going from
// a carrier on the trace to its history below crosses the scale on the way.
//
// All three share one horizontal mapping: the frequencies at the outer edges
// of the first and last bins, which is what EngineLink::frequencyAtFraction
// answers at 0 and 1 and what both displays stretch across their width. The
// ticks, their spacing and their labels are models/ruler.h, which has the
// cases that keep labels from colliding and ticks on their columns.
//
// Every receiver in the rack is marked here too, each in its own colour, so
// the ruler doubles as the tuning reference: where each filter sits, against
// the numbers.
//
// The wheel over it does what the wheel over either display does, into the
// same backlog, and a click is a click on the frequency under the pointer:
// it focuses the receiver whose band it is inside, or tunes the focused one,
// and a double click adds a receiver. Hovering says so.

import QtQuick
import QtQuick.Controls
import Revenant

Item {
    id: ruler

    // A click, with the frequency under it.
    signal picked(double hz)

    // The second click of a double click, with the frequency under it.
    signal added(double hz)

    // The edges, read with the properties that move them so the bindings
    // below follow a retune and a reconnect. frequencyAtFraction is a method,
    // and QML cannot see inside a C++ call to know what it depends on.
    readonly property bool live: engineLink.connected && engineLink.bins > 0
    readonly property double lowHz: live ? edge(0.0, engineLink.sourceCenterHz,
                                                engineLink.tuneGrantedHz, engineLink.sourceEpoch) : 0
    readonly property double highHz: live ? edge(1.0, engineLink.sourceCenterHz,
                                                 engineLink.tuneGrantedHz, engineLink.sourceEpoch) : 0

    // The last three arguments are unused and are there to be read: a
    // binding that calls this re-evaluates when any of them changes, which
    // is every event that moves the span under the displays.
    function edge(fraction, centre, granted, epoch) {
        return engineLink.frequencyAtFraction(fraction)
    }

    readonly property var ticks: UiRules.rulerTicks(lowHz, highHz, width,
                                                   metrics.averageCharacterWidth,
                                                   unit.width + 10)

    function xOf(hz) {
        return UiRules.rulerX(hz, ruler.lowHz, ruler.highHz, ruler.width)
    }

    implicitHeight: 24

    FontMetrics {
        id: metrics
        font.family: Theme.monoFont
        font.pixelSize: Theme.sizeSmall
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    // Every receiver's passband, then its tuned frequency, under the ticks,
    // each in its rack colour. The focused one is the stronger mark and the
    // held ones fainter, the same split the span displays draw.
    Repeater {
        model: ruler.live ? engineLink.rackCount : 0

        Item {
            id: band

            required property int index
            readonly property var entry: engineLink.rackEntries[index] || ({})
            readonly property color tint: entry.colour || Theme.inkOff
            readonly property bool focused: entry.focused || false
            readonly property double lowX: ruler.xOf(entry.lowHz || 0)
            readonly property double highX: ruler.xOf(entry.highHz || 0)
            readonly property double centreX: ruler.xOf(entry.frequencyHz || 0)

            anchors.fill: parent
            z: focused ? 1 : 0

            Rectangle {
                visible: band.highX > band.lowX
                x: Math.max(0, band.lowX)
                width: Math.max(2, Math.min(ruler.width, band.highX) - x)
                height: parent.height
                color: Qt.rgba(band.tint.r, band.tint.g, band.tint.b, band.focused ? 0.22 : 0.12)
                border.width: 0

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: band.focused ? 2 : 1
                    color: band.tint
                }
            }

            Rectangle {
                x: band.centreX - (band.focused ? 1 : 0.5)
                width: band.focused ? 2 : 1
                height: parent.height
                color: band.tint
                opacity: band.focused ? 1.0 : 0.7
            }
        }
    }

    Repeater {
        model: ruler.ticks

        Item {
            id: tick

            required property var modelData

            x: modelData.x
            height: ruler.height

            Rectangle {
                x: -0.5
                width: 1
                height: tick.modelData.major ? 7 : 4
                color: tick.modelData.major ? Theme.inkDim : Theme.inkOff
            }

            Text {
                visible: tick.modelData.label.length > 0
                text: tick.modelData.label
                x: -width / 2
                y: 8
                color: Theme.inkDim
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
            }
        }
    }

    Text {
        id: unit

        visible: ruler.live
        x: 4
        y: 8
        text: UiRules.rulerUnit(ruler.lowHz, ruler.highHz)
        color: Theme.inkOff
        font.family: Theme.uiFont
        font.pixelSize: Theme.sizeSmall
    }

    MouseArea {
        id: pointer

        anchors.fill: parent
        enabled: ruler.live
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: (mouse) => ruler.picked(
                       engineLink.frequencyAtFraction(mouse.x / Math.max(1, ruler.width)))
        onDoubleClicked: (mouse) => ruler.added(
                             engineLink.frequencyAtFraction(mouse.x / Math.max(1, ruler.width)))

        // The click rule, where the click is made. See UiRules.spanClickHint.
        ToolTip.visible: containsMouse
        ToolTip.delay: 900
        ToolTip.text: UiRules.spanClickHint()
        onWheel: (wheel) => {
            const eighths = UiRules.scrollEighths(wheel.angleDelta.x, wheel.angleDelta.y)
            if (eighths !== 0 && engineLink.sourceCanRetune)
                engineLink.takeScrollTune(eighths)
        }
    }
}
