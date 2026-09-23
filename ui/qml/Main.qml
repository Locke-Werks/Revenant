// The main window. Layout and binding only.
//
// Every number shown here is read from a property on EngineLink or on one of
// the render items. Nothing is computed in this file: the reduction
// correction in particular belongs to the item that applied it, because it
// depends on that item's width, and a copy of the arithmetic here would be a
// second answer to the same question.
//
// The one call that looks like an exception is not one. The ruler asks
// EngineLink for the frequency at a fraction of the span, and that is a
// method rather than a property because only a display knows where its ticks
// are. The arithmetic behind it is still in one place, on the far side of
// that call, working from the rationals the wire carries.
//
// WHAT IS ON THIS WINDOW, AND WHAT IS NOT
//
// The span, which is the interface: spectrum, ruler and waterfall filling
// everything under one strip of chrome. The top bar tunes the radio and opens
// panels over the span for what direct manipulation cannot express. A banner
// appears under the bar only while something is wrong. The last click is a
// card over the waterfall. The receiver's controls, its fine-tuning display,
// RDS and audio are the receiver window, which is a second top-level window
// made here so both share one engine link and one selection. Every key in
// both windows is Commands.qml, made here for the same reason, and the
// command palette and the key map it opens hang over whichever is in front.
//
// Until 2026-09-22 this was a column of twenty-odd rows over a spectrum that
// got the height left over, and before that one file of 2792 lines. Each
// section is its own file beside this one and keeps the comments that
// explain its wording; the palette and type are Theme.qml, and the selection
// the two span displays share is TuneSelection.qml.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ApplicationWindow {
    id: window

    width: 1280
    height: 800
    visible: true
    color: Theme.background
    title: engineLink.connected
           ? "Revenant  ·  " + engineLink.endpoint
           : "Revenant  ·  waiting for " + engineLink.endpoint

    // Closing this window ends the session even with the receiver window
    // still open: it is the one with the radio on it, and a receiver window
    // left running on its own would hold the engine's receiver with nothing
    // on screen that says which radio it is on.
    onClosing: Qt.quit()

    TuneSelection {
        id: tuneSelection
    }

    VrxWindow {
        id: receiverWindow
        commands: keyCommands
    }

    // Every key, and what each one does. See Commands.qml.
    Commands {
        id: keyCommands
        mainWindow: window
        receiverWindow: receiverWindow
        topBar: topBar
        spanView: spanView
        mainPalette: commandPalette
        mainKeyMap: keyMapView
    }

    CommandPalette {
        id: commandPalette
        commands: keyCommands
    }

    KeyMapView {
        id: keyMapView
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TopBar {
            id: topBar
            Layout.fillWidth: true
            receiverWindow: receiverWindow
            tuneDigit: keyCommands.tuneDigit
        }

        NoticeBanner {
            Layout.fillWidth: true
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            SpanView {
                id: spanView
                anchors.fill: parent
                anchors.margins: 6
                selection: tuneSelection
            }

            // What the last click resolved to, over the bottom of the
            // waterfall where the eye already is after clicking there.
            Rectangle {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 16
                width: Math.min(parent.width - 32, readout.implicitWidth + close.width + 24)
                height: readout.implicitHeight + 16
                visible: tuneSelection.tuned
                radius: Theme.radius + 2
                color: Theme.panel
                border.width: 1
                border.color: Theme.border

                ClickReadout {
                    id: readout
                    anchors.left: parent.left
                    anchors.right: close.left
                    anchors.top: parent.top
                    anchors.margins: 8
                    selection: tuneSelection
                }

                RButton {
                    id: close
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 4
                    flat: true
                    text: "×"
                    ink: Theme.inkDim
                    onClicked: tuneSelection.dismiss()
                }
            }
        }
    }
}
