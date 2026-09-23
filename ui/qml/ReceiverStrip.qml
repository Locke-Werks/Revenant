// One receiver's strip in the rack: its colour, its name, where it is tuned,
// its mode, a live meter, and mute and solo.
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

    property color tint: Theme.accent
    property bool focused: false

    implicitHeight: body.implicitHeight + 16
    radius: Theme.radius
    color: Theme.control
    border.width: 1
    border.color: strip.focused ? Qt.darker(strip.tint, 1.6) : Theme.border

    // The receiver's colour down the left edge, which is what ties the strip
    // to its marker on the span.
    Rectangle {
        width: 4
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
                text: "VFO " + engineLink.receiverId
                color: strip.tint
                font.pixelSize: Theme.sizeBody
                font.bold: true
            }

            Label {
                text: engineLink.receiverDemod.toUpperCase()
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
            }

            Item { Layout.fillWidth: true }

            // Mute is the player's, since one receiver's audio is all the
            // player carries; see AudioPane.qml for why that is.
            RButton {
                implicitWidth: 26
                flat: true
                checkable: true
                checked: audioPlayer.muted
                text: "M"
                tint: Theme.inkWarn
                ink: Theme.inkDim
                onClicked: audioPlayer.muted = !audioPlayer.muted
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: "mute"
            }

            RButton {
                implicitWidth: 26
                flat: true
                enabled: false
                text: "S"
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: "solo, once the window holds more than one receiver"
            }
        }

        Label {
            text: (engineLink.receiverCenterHz / 1.0e6).toFixed(6) + " MHz"
            color: Theme.ink
            font.family: Theme.monoFont
            font.pixelSize: 15
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 5
                radius: 2
                color: Theme.panelSolid

                Rectangle {
                    width: parent.width * UiRules.meterFraction(engineLink.receiverLevelDbfs)
                    height: parent.height
                    radius: 2
                    color: strip.tint
                }
            }

            Readout {
                widest: "no level yet"
                text: UiRules.meterHasReading(engineLink.receiverLevelDbfs)
                      ? engineLink.receiverLevelDbfs.toFixed(0) + " dBFS"
                      : "no level yet"
                color: Theme.inkDim
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
            }
        }
    }
}
