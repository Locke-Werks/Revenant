// What the window says while the engine has a recording open: its name, how
// far through it the engine is, the pace in force with a control to change
// it, and that it plays once.
//
// A STRIP UNDER THE TOP BAR, THERE ONLY WHILE A RECORDING IS OPEN, above the
// control row: a radio session pays nothing for it, and the bar above keeps
// its width for the dial and the band buttons it already fills at 1280 pixels.
//
// WHAT THIS PARAGRAPH USED TO SAY: "in the shape of the fault strip below
// it". The fault strip went on 2026-09-23 and its row is ControlRow.qml.
//
// A READOUT WITH ONE CONTROL. The pace is the one thing the wire lets a client
// change while a recording plays, Session.setSourcePace, so it gets 1x, 2x, 4x
// and max. There is no seek and no loop on the wire, and
// ui/models/recording_status.h records both; a scrub bar here would be a
// control nothing could apply, so the progress is a two-pixel line rather
// than a slider with a handle.
//
// WHAT THIS NOTE USED TO SAY, before 2026-09-23: "The wire has no seek, no
// pace control and no loop".

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Rectangle {
    id: strip

    visible: recordingLink.playing
    implicitHeight: row.implicitHeight + 10
    color: Theme.panelSolid

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }

    RowLayout {
        id: row

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 10

        Label {
            text: recordingLink.ended ? "ended" : "recording"
            color: recordingLink.ended ? Theme.inkDim : Theme.accent
            font.pixelSize: Theme.sizeSmall
            font.bold: true
        }

        Label {
            Layout.minimumWidth: 0
            Layout.maximumWidth: 420
            text: recordingLink.playingName
            color: Theme.ink
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideMiddle

            HoverHandler { id: nameHover }

            Tip {
                visible: nameHover.hovered && text.length > 0
                text: engineLink.openedSource.uri === undefined ? "" : engineLink.openedSource.uri
            }
        }

        Label {
            text: recordingLink.positionText
            color: recordingLink.ended ? Theme.inkDim : Theme.ink
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeBody
        }

        // How far through, as a line. See the note above for why it has no
        // handle.
        Rectangle {
            Layout.fillWidth: true
            Layout.minimumWidth: 60
            implicitHeight: 2
            color: Theme.border

            Rectangle {
                width: parent.width * recordingLink.fraction
                height: parent.height
                color: recordingLink.ended ? Theme.inkDim : Theme.accent
            }
        }

        Label {
            visible: recordingLink.paceText.length > 0
            text: recordingLink.paceText
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        // Filled from what the engine says is in force, so a pick shows when
        // it has landed and a pace this does not offer fills nothing.
        RSegmented {
            visible: !recordingLink.ended
            options: recordingLink.paceOptions
            current: recordingLink.paceOption
            onPicked: option => recordingLink.setPace(option)
        }

        StatusChip {
            visible: recordingLink.paceFault.length > 0
            label: "pace refused"
            detail: recordingLink.paceFault
            ink: Theme.inkWarn
        }

        Label {
            text: "plays once"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        // What the engine opened, when it is not what the picker read.
        StatusChip {
            visible: recordingLink.mismatch.length > 0
            label: "engine opened another file"
            detail: recordingLink.mismatch
            ink: Theme.inkWarn
        }
    }
}
