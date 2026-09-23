// The receiver window: where the receivers go when they are popped out of the
// main window, for a second screen.
//
// A SECOND TOP-LEVEL WINDOW IN THE SAME PROCESS, not a second process. It
// holds nothing of its own but the command palette and the key map that open
// over it: the panel it shows, ReceiverPanel.qml, is moved into it by Main.qml
// when the receivers pop out and back into the main window when they dock.
//
// WHAT THIS HEADER USED TO SAY: that it was the receivers' only home, "a
// wholly separate window" the owner asked for on 2026-09-22, and that "A pane
// under the waterfall could do neither". On 2026-09-23 the owner asked for
// them docked in the main window by default and popped out on demand, which
// is what this window is for now; models/receiver_placement.h has the call.
//
// transientParent is cleared so the window is a peer of the main one rather
// than a child kept above it: it has its own taskbar entry and can sit behind
// the spectrum on the same screen or alone on another. Where it was is
// remembered in main.cpp. Closing it docks the receivers again rather than
// hiding them, so closing a window never loses the controls in it.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Window {
    id: receivers

    objectName: "vrxWindow"
    title: "Revenant  ·  receivers"
    width: 1040
    height: 680
    minimumWidth: 720
    minimumHeight: 460
    color: Theme.background
    transientParent: null
    visible: false

    // Commands.qml, which this window's palette runs its entries through.
    property var commands: null

    readonly property alias commandPalette: commandPalette
    readonly property alias keyMapView: keyMapView

    // The window's close button was pressed. Main.qml docks the receivers.
    signal dockRequested()

    onClosing: receivers.dockRequested()

    // Brings the window to the front after a click on the span that tuned or
    // focused a receiver: shown or restored if it has to be, raised, and
    // activated only when it was shown or restored. What each step is for,
    // and what Windows does with it, is models/window_raise.h.
    function bringForward() {
        const plan = UiRules.receiverWindowRaise(receivers.visible,
                                                 receivers.visibility === Window.Minimized)
        if (plan.show)
            receivers.show()
        if (plan.restore)
            receivers.showNormal()
        if (plan.raise)
            receivers.raise()
        if (plan.activate)
            receivers.requestActivate()
    }

    CommandPalette {
        id: commandPalette
        commands: receivers.commands
    }

    KeyMapView {
        id: keyMapView
    }
}
