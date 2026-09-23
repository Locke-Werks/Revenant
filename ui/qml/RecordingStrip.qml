// What the window says while the engine has a recording open: its name, how
// far through it the engine is, what paces it, and that it plays once.
//
// A STRIP UNDER THE TOP BAR, THERE ONLY WHILE A RECORDING IS OPEN, in the
// shape of the fault strip below it: a radio session pays nothing for it, and
// the bar above keeps its width for the dial and the band buttons it already
// fills at 1280 pixels.
//
// A READOUT AND NOT A TRANSPORT. The wire has no seek, no pace control and no
// loop, and ui/models/recording_status.h records each of those; a scrub bar or
// a speed menu here would be a control nothing could apply. The progress is a
// two-pixel line for that reason rather than a slider with a handle.

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
            ToolTip.visible: nameHover.hovered
            ToolTip.text: engineLink.openedSource.uri === undefined ? "" : engineLink.openedSource.uri
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
