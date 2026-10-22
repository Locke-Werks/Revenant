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
//
// AND, SINCE 2026-10-22, A THIRD. The owner asked for a skinny tall window to
// fold away what it can and keep the receiver readable, so under 640 wide the
// panel is one column: a short rack over the receiver, whose filter and noise
// controls collapse behind their expander, over the sections. A dock beside
// the span in a tall main window is usually this shape as well. A wide short
// window takes the three columns whatever its aspect.

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

    // models/receiver_placement.h: 0 narrow, 1 stacked, 2 wide.
    readonly property int arrangement: UiRules.receiverPanelLayout(width, height)
    readonly property bool narrow: arrangement === 0
    readonly property bool sideBySide: arrangement === 2

    GridLayout {
        anchors.fill: parent
        anchors.margins: panel.poppedOut ? 12 : Theme.gap
        columns: panel.narrow ? 1 : 5
        rowSpacing: 8
        columnSpacing: 12

        // A share of the column when narrow, scrolled inside, so the
        // receiver under it keeps most of a tall window. Beside it otherwise,
        // a little wider than the old fixed 260 when there is room, so a
        // strip's figures are not cut off at a large display scale.
        ReceiverRack {
            Layout.fillWidth: panel.narrow
            Layout.fillHeight: !panel.narrow
            Layout.preferredWidth: panel.narrow ? -1 : Math.max(260, Math.min(320, panel.width * 0.24))
            Layout.minimumWidth: panel.narrow ? 0 : 260
            Layout.maximumWidth: panel.narrow ? Number.POSITIVE_INFINITY : 320
            Layout.preferredHeight: panel.narrow ? Math.max(150, panel.height * 0.3) : -1
            Layout.maximumHeight: panel.narrow ? Math.max(150, panel.height * 0.3) : Number.POSITIVE_INFINITY
            poppedOut: panel.poppedOut
            onPopToggled: panel.popToggled()
        }

        Rectangle {
            Layout.fillHeight: !panel.narrow
            Layout.fillWidth: panel.narrow
            implicitWidth: 1
            implicitHeight: 1
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
                narrow: panel.narrow
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
            implicitHeight: 1
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

        CallsPane {
            Layout.minimumWidth: 0
        }

        // P25 trunk following.
        TrunkPanel {
            Layout.minimumWidth: 0
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
