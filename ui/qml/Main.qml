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

    // The default, inside what the screen has: at 150% a 1080p screen is
    // 1280 by 720 logical pixels, under the old fixed 800.
    width: Math.min(1280, Screen.desktopAvailableWidth - 40)
    height: Math.min(800, Screen.desktopAvailableHeight - 60)
    visible: true
    color: Theme.background
    title: engineLink.connected
           ? "Revenant  ·  " + engineLink.endpoint
           : engineLink.engineStarting
             ? "Revenant  ·  starting the engine"
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
        // Hidden behind a lease notice too, so no receiver control is usable
        // while the engine is someone else's. The placement itself is kept,
        // so the window comes back where it was.
        const wanted = receiverPlacement.poppedOut && receiverPlacement.shown
                       && engineLink.leaseNotice === ""
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

    Connections {
        target: engineLink
        function onLeaseChanged() { window.placeReceiverWindow() }
    }

    VrxWindow {
        id: receiverWindow
        commands: keyCommands
        onDockRequested: receiverPlacement.poppedOut = false
        paneContents: receiverPanel.contents
    }

    // The receivers themselves, in the dock or in the receiver window.
    // ReceiverPanel.qml makes their panes; the main window's layout or the
    // receiver window's draws them.
    ReceiverPanel {
        id: receiverPanel
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

    // Burst timeline (feature #4): its own window, opened from the top bar.
    BurstWindow {
        id: burstWindow
    }

    Shortcut {
        sequence: "Ctrl+Shift+B"
        context: Qt.ApplicationShortcut
        enabled: UiRules.shortcutLive(true, engineLink.leaseNotice)
        onActivated: burstWindow.toggle()
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
            id: recordingStrip
            Layout.fillWidth: true
        }

        // The gain and the detector's three settings, always there. Faults
        // are in the top bar, beside the status pill.
        ControlRow {
            id: controlRow
            Layout.fillWidth: true
        }

        // Every pane, tiled; models/pane_layout.h. Any pane goes against any
        // edge of any other by dragging its title strip there.
        PaneHost {
            id: mainHost

            Layout.fillWidth: true
            Layout.fillHeight: true
            store: mainLayout
            contents: {
                const out = Object.assign({}, receiverPanel.contents)
                out["span"] = spanView.spanPane
                return out
            }
        }
    }

    // The receiver panes, which are in this window, in the receiver window,
    // or hidden, and the sections with nothing to show, which step aside.
    readonly property var receiverPanes: Object.keys(receiverPanel.contents)
    readonly property bool receiversHere: receiverPlacement.shown && !receiverPlacement.poppedOut
    readonly property bool receiversThere: receiverPlacement.shown && receiverPlacement.poppedOut

    Binding {
        target: mainLayout
        property: "hidden"
        value: window.receiversHere ? receiverPanel.empty : window.receiverPanes
    }

    Binding {
        target: receiverLayout
        property: "hidden"
        value: window.receiversThere ? receiverPanel.empty : window.receiverPanes
    }

    // The window never gets smaller than every pane at its minimum plus the
    // chrome above them, so no pane is ever cut off.
    // Never more than the screen, though: a minimum the screen cannot hold
    // has Windows and Qt fight over the window's size every frame. When the
    // panes need more than the screen, a pane's own scale is the way down.
    minimumWidth: Math.min(Math.max(mainLayout.minimumWidth, 480),
                           Screen.desktopAvailableWidth - 16)
    minimumHeight: Math.min(mainLayout.minimumHeight + topBar.height + recordingStrip.height
                            + controlRow.height,
                            Screen.desktopAvailableHeight - 48)

    // The spectrum and the waterfall, which the layout above draws.
    SpanView {
        id: spanView
        selection: tuneSelection
    }

    // The single-user lease. Opaque and on top, and it takes every click and
    // key, because the controls under it belong to a session the engine has
    // refused.
    // In the overlay layer, above any popup or menu, so one open when the
    // notice appears is covered rather than left usable; the named ones are
    // closed outright.
    Rectangle {
        id: leaseNotice
        parent: Overlay.overlay
        anchors.fill: parent
        z: 1000000
        visible: engineLink.leaseNotice !== ""
        color: Theme.background
        focus: visible
        onVisibleChanged: {
            if (!visible)
                return
            commandPalette.close()
            burstWindow.hide()
            forceActiveFocus()
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onWheel: wheel => wheel.accepted = true
        }
        Keys.onPressed: event => event.accepted = true

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 16

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: engineLink.leaseNotice
                color: Theme.ink
                font.pixelSize: Theme.sizeFigure
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                Layout.maximumWidth: leaseNotice.width - 80
            }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 12
                Button {
                    text: engineLink.leaseAction
                    onClicked: engineLink.takeOverLease()
                }
                Button {
                    text: "Quit"
                    onClicked: Qt.quit()
                }
            }
        }
    }
}
