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
// panels over the span for what direct manipulation cannot express, and names
// what is wrong, when something is, beside its status pill. Under it a row
// holds the gain and the detector's settings. The last click is a
// card over the waterfall. The receivers, the rack, the focused receiver's
// controls and fine-tuning display, RDS, decoding and audio, are one panel
// docked under the span, behind a divider the operator can drag, or popped out
// into the receiver window, a second top-level window made here so both share
// one engine link and one selection. models/receiver_placement.h has why they
// start docked. Every key in both windows is Commands.qml, made here for the
// same reason, and the command palette and the key map it opens hang over
// whichever window is in front.
//
// WHAT THE PARAGRAPH ABOVE USED TO SAY, from its fifth sentence: "The
// receiver's controls, its fine-tuning display, RDS and audio are the receiver
// window, which is a second top-level window made here".
//
// AND WHAT ITS THIRD SENTENCE USED TO SAY, before 2026-09-23: "A banner
// appears under the bar only while something is wrong." Its notices are in
// the top bar now and its row is ControlRow.qml.
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

    // ------------------------------------------------------------------
    // Where the receivers are
    // ------------------------------------------------------------------
    //
    // receiverPlacement is models/receiver_placement_store.h: docked or popped
    // out, shown or not, and the dock's height, remembered across runs.

    // Hides or shows them where they are: the top bar's "receivers" button
    // and its key.
    function toggleReceivers() {
        receiverPlacement.shown = !receiverPlacement.shown
    }

    // Pops them out into their own window, or docks them again: the rack's
    // button and its key. Always shown after, since either is a request to
    // see them somewhere.
    function popReceivers() {
        receiverPlacement.poppedOut = !receiverPlacement.poppedOut
        receiverPlacement.shown = true
    }

    // Shows them, wherever they are, and puts their window in front when
    // they are popped out. keyed says whether a key asked for it, which also
    // wants the window with the keyboard.
    function showReceivers(keyed) {
        receiverPlacement.shown = true
        if (!receiverPlacement.poppedOut)
            return
        if (keyed) {
            receiverWindow.show()
            receiverWindow.raise()
            receiverWindow.requestActivate()
        } else {
            receiverWindow.bringForward()
        }
    }

    // The window is shown exactly while the receivers are popped out and
    // shown, and never before main.cpp has set its swap interval and put it
    // where it was; see the store's note on ready.
    function placeReceiverWindow() {
        if (!receiverPlacement.ready)
            return
        const wanted = receiverPlacement.poppedOut && receiverPlacement.shown
        if (wanted && !receiverWindow.visible) {
            receiverWindow.show()
            receiverWindow.raise()
        } else if (!wanted && receiverWindow.visible) {
            receiverWindow.hide()
        }
    }

    Connections {
        target: receiverPlacement
        function onPoppedOutChanged() { window.placeReceiverWindow() }
        function onShownChanged() { window.placeReceiverWindow() }
        function onReadyChanged() { window.placeReceiverWindow() }
    }

    VrxWindow {
        id: receiverWindow
        commands: keyCommands
        onDockRequested: receiverPlacement.poppedOut = false
    }

    // The receivers themselves, in the dock or in the receiver window.
    ReceiverPanel {
        id: receiverPanel
        parent: receiverPlacement.poppedOut ? receiverWindow.contentItem : dock
        anchors.fill: parent
        visible: receiverPlacement.shown
        poppedOut: receiverPlacement.poppedOut
        onPopToggled: window.popReceivers()
    }

    // A click on a signal brings the receivers forward. EngineLink decides
    // which clicks, models/window_raise.h how when they are in their window.
    Connections {
        target: engineLink
        function onReceiverWindowWanted() {
            window.showReceivers(false)
        }
    }

    // Every key, and what each one does. See Commands.qml.
    Commands {
        id: keyCommands
        mainWindow: window
        receiverWindow: receiverWindow
        receiverPanel: receiverPanel
        topBar: topBar
        controlRow: controlRow
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
            tuneDigit: keyCommands.tuneDigit
        }

        // The open recording's name, position and pace, while there is one.
        RecordingStrip {
            Layout.fillWidth: true
        }

        // The gain and the detector's three settings, always there. Faults
        // are in the top bar, beside the status pill.
        ControlRow {
            id: controlRow
            Layout.fillWidth: true
        }

        // The span over the docked receivers, with a divider between them the
        // operator can drag. SplitView leaves out an item that is not
        // visible, so a hidden or popped-out dock gives the span everything.
        SplitView {
            id: split

            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical

            handle: Rectangle {
                implicitHeight: 5
                color: SplitHandle.pressed ? Theme.accentDim
                       : SplitHandle.hovered ? Theme.border
                       : Theme.panelSolid

                // A grip in the middle, so the divider reads as something
                // that moves rather than as a gap between two displays.
                Rectangle {
                    anchors.centerIn: parent
                    width: 36
                    height: 1
                    color: SplitHandle.pressed ? Theme.accent : Theme.inkOff
                }
            }

            Item {
                SplitView.fillHeight: true
                SplitView.minimumHeight: 160

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

            // The dock. ReceiverPanel is parented here while it is docked.
            Rectangle {
                id: dock

                visible: receiverPlacement.shown && !receiverPlacement.poppedOut
                color: Theme.background
                SplitView.minimumHeight: 240
                SplitView.preferredHeight: UiRules.dockHeight(receiverPlacement.dockHeight, split.height)

                // What the operator dragged it to, kept for the next run. Only
                // while they are dragging, so a window resized or a banner coming
                // and going does not overwrite what they chose.
                onHeightChanged: {
                    if (visible && split.resizing)
                        receiverPlacement.dockHeight = height
                }
            }
        }
    }
}
