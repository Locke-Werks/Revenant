// One receiver's strip in the rack: its colour, its name, where it is tuned,
// its mode, a live meter, a gain, and mute and solo. A click anywhere on it
// that is not one of its controls focuses the receiver.
//
// Everything shown comes from one entry of EngineLink::rackEntries, which
// ui/models/rack_link.cpp builds; the rules behind mute, solo and the gain
// are models/receiver_rack.h.
//
// The meter's scale is models/level_meter.h, fixed rather than tracking the
// signal, because a meter that rescales to the loudest thing shows every
// signal as full. With no measurement yet the bar is empty and the number
// says so, rather than an empty bar reading as silence.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Rectangle {
    id: strip

    required property var entry

    readonly property color tint: entry.colour
    readonly property bool focused: entry.focused

    implicitHeight: body.implicitHeight + 16
    radius: Theme.radius
    color: strip.focused ? Theme.controlHover : Theme.control
    border.width: 1
    border.color: strip.focused ? strip.tint : Theme.border

    // Behind the controls, so a press on a button is the button's.
    MouseArea {
        anchors.fill: parent
        cursorShape: strip.focused ? Qt.ArrowCursor : Qt.PointingHandCursor
        onClicked: engineLink.focusReceiver(strip.entry.key)
    }

    // The receiver's colour down the left edge, which is what ties the strip
    // to its marker on the span. Wider on the focused one.
    Rectangle {
        width: strip.focused ? 6 : 4
        height: parent.height - 2
        x: 1
        y: 1
        radius: Theme.radius
        color: strip.tint
    }

    ColumnLayout {
        id: body

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        anchors.leftMargin: 14
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Label {
                text: strip.entry.label
                color: strip.tint
                font.pixelSize: Theme.sizeBody
                font.bold: true
            }

            Label {
                text: UiRules.modeLabel(strip.entry.mode).toUpperCase()
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
            }

            Item { Layout.fillWidth: true }

            // Heard at the client or not. Mute takes this receiver out of
            // the mix; solo takes every other one out. Neither touches the
            // engine beyond the subscription: a receiver nobody hears is not
            // streamed.
            RButton {
                implicitWidth: 26
                implicitHeight: 20
                flat: true
                checkable: true
                checked: strip.entry.muted
                text: "M"
                tint: Theme.inkWarn
                ink: Theme.inkDim
                onClicked: engineLink.setReceiverMuted(strip.entry.key, !strip.entry.muted)

                Tip {
                    visible: parent.hovered
                    text: strip.entry.muted ? "unmute this receiver" : "mute this receiver"
                }
            }

            RButton {
                implicitWidth: 26
                implicitHeight: 20
                flat: true
                checkable: true
                checked: strip.entry.solo
                text: "S"
                tint: strip.tint
                ink: Theme.inkDim
                onClicked: engineLink.toggleReceiverSolo(strip.entry.key)

                Tip {
                    visible: parent.hovered
                    text: strip.entry.solo ? "stop soloing, so every unmuted receiver is heard"
                                           : "hear this receiver alone, muted or not"
                }
            }

            RButton {
                implicitWidth: 22
                implicitHeight: 20
                flat: true
                text: "×"
                ink: Theme.inkDim
                onClicked: engineLink.removeRackReceiver(strip.entry.key)

                Tip {
                    visible: parent.hovered
                    text: "remove this receiver"
                }
            }
        }

        // Where it is tuned, and its level in figures at the other end of the
        // same row, over the meter that draws it. The level had a row of its
        // own beside the meter, which made every strip a line taller than it
        // needed to be; the rack is shorter docked under the span than it was
        // in a window of its own.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Readout {
                Layout.alignment: Qt.AlignLeft
                widest: "0000.000000 MHz"
                horizontalAlignment: Text.AlignLeft
                text: (strip.entry.frequencyHz / 1.0e6).toFixed(6) + " MHz"
                color: strip.entry.pending || strip.entry.refused ? Theme.inkDim : Theme.ink
                font.pixelSize: Theme.sizeFigure
            }

            Item { Layout.fillWidth: true }

            // A REFUSAL REPLACES THE LEVEL. The engine said it will not make
            // this receiver, so there is no level to wait for, and "opening"
            // would be waiting for good. The chip names it, the engine's
            // sentence is its hover, and remove is beside it because that
            // or a retune is all there is to do.
            StatusChip {
                visible: strip.entry.refused
                label: "refused"
                detail: strip.entry.refusal
            }

            RButton {
                visible: strip.entry.refused
                flat: true
                text: "remove"
                ink: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
                onClicked: engineLink.removeRackReceiver(strip.entry.key)
            }

            Readout {
                visible: !strip.entry.refused
                widest: "no level yet"
                text: strip.entry.pending ? "opening"
                      : UiRules.meterHasReading(strip.entry.levelDbfs)
                        ? strip.entry.levelDbfs.toFixed(0) + " dBFS"
                        : "no level yet"
                font.pixelSize: Theme.sizeSmall
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 4
            radius: 2
            color: Theme.panelSolid

            Rectangle {
                width: parent.width * UiRules.meterFraction(strip.entry.levelDbfs)
                height: parent.height
                radius: 2
                color: strip.entry.heard ? strip.tint : Theme.inkOff
            }
        }

        // The strip's share of the mix. Only the audio section's switch and
        // the player's volume sit above it.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            RSlider {
                Layout.fillWidth: true
                implicitHeight: 18
                from: 0
                to: 1
                stepSize: 0.025
                value: strip.entry.gain
                tint: strip.tint
                enabled: strip.entry.heard
                onMoved: engineLink.setReceiverGain(strip.entry.key, value)
            }

            Readout {
                widest: "-40 dB"
                text: strip.entry.gainText
                font.pixelSize: Theme.sizeSmall
            }
        }
    }
}
