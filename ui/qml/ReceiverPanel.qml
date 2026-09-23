// The receivers: the rack of receiver strips, the focused receiver's controls
// and fine-tuning display, and its RDS, decoding and audio.
//
// ONE PANEL, DRAWN IN ONE OF TWO PLACES. Docked under the span in the main
// window, which is where it starts, or popped out into the receiver window
// for a second screen. Main.qml moves this one item between the two, so the
// fine-tuning waterfall keeps its history and the rack its scroll position
// across a pop; models/receiver_placement.h has the owner's call behind it.
//
// AND IN ONE OF TWO ARRANGEMENTS. Docked, the panel is a strip much wider than
// it is tall, and RDS, decoding and audio stacked under the fine-tuning
// display would leave that display no height, so a wide panel gives them a
// column of their own on the right. The popped-out window's shape stacks them
// under it as the window always did. The same three items are moved between
// the two places rather than drawn twice.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Item {
    id: panel

    // In the receiver window rather than docked, which the rack's button reads.
    property bool poppedOut: false

    // The rack's pop-out button was pressed.
    signal popToggled()

    // What the keys reach into here: the receiver's dial and its filter
    // display. See Commands.qml.
    readonly property alias dial: detail.dial
    readonly property alias passband: detail.passband

    readonly property bool sideBySide: UiRules.receiverPanelSideBySide(width, height)

    RowLayout {
        anchors.fill: parent
        anchors.margins: panel.poppedOut ? 12 : Theme.gap
        spacing: 12

        ReceiverRack {
            Layout.preferredWidth: 260
            Layout.minimumWidth: 260
            Layout.maximumWidth: 260
            Layout.fillHeight: true
            poppedOut: panel.poppedOut
            onPopToggled: panel.popToggled()
        }

        Rectangle {
            Layout.fillHeight: true
            implicitWidth: 1
            color: Theme.border
        }

        ColumnLayout {
            id: stacked

            Layout.fillWidth: true
            Layout.fillHeight: true

            // Never wider than the window leaves it. The rows inside ask for
            // their own widths, and without this a row that asked for more than
            // there was pushed the whole column past the right-hand edge.
            Layout.minimumWidth: 0
            clip: true
            spacing: 10

            ReceiverDetail {
                id: detail
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.fillHeight: true
            }

            // Filling the space the detail leaves when there is no receiver,
            // so RDS and audio stay at the bottom where they were. Wide as well
            // as tall: a column whose children all keep their own width is no
            // wider than they are, and the empty column then left the right
            // third of the docked strip bare.
            Item {
                Layout.fillHeight: true
                Layout.fillWidth: true
                visible: engineLink.receiverId === 0
            }
        }

        Rectangle {
            visible: panel.sideBySide
            Layout.fillHeight: true
            implicitWidth: 1
            color: Theme.border
        }

        ColumnLayout {
            id: beside

            // Fixed rather than preferred, so the receiver's column gets what
            // is left whatever its own rows ask for, and its compact rows
            // follow the width it was given rather than the one they asked for.
            readonly property real fixedWidth:
                Math.max(380, Math.min(560, (panel.width - 290) * 0.42))

            visible: panel.sideBySide
            Layout.fillHeight: true
            Layout.preferredWidth: fixedWidth
            Layout.minimumWidth: fixedWidth
            Layout.maximumWidth: fixedWidth
            clip: true
            spacing: 10
        }
    }

    // RDS, decoding and audio, in whichever of the two places the arrangement
    // puts them. A parent binding rather than two copies: each section holds
    // state an operator is using, a scrolled decode log or a combo box open.
    ColumnLayout {
        id: sections

        parent: panel.sideBySide ? beside : stacked
        Layout.fillWidth: true
        Layout.fillHeight: panel.sideBySide
        Layout.minimumWidth: 0
        spacing: 10

        RdsPane {
            Layout.minimumWidth: 0
        }

        DecodePane {
            id: decode
            Layout.minimumWidth: 0
            fills: panel.sideBySide
        }

        // Beside the receiver, what the log leaves goes to the bottom of the
        // column, so the audio section sits at the foot of it whatever else
        // is shown.
        Item {
            visible: panel.sideBySide && !(decode.visible && decode.growing)
            Layout.fillHeight: true
        }

        AudioPane {
            Layout.minimumWidth: 0
        }
    }
}
