// The receiver window: the rack of receiver strips, the focused receiver's
// controls and fine-tuning display, and its RDS, decoding and audio.
//
// A SECOND TOP-LEVEL WINDOW IN THE SAME PROCESS, not a second process and not
// a pane. The owner asked on 2026-09-22 for the receiver to be a wholly
// separate window, and the design brief this client was started from puts the
// rack on one screen and the spectrum full-bleed on another. A pane under the
// waterfall could do neither, and each receiver control it drew took height
// from the waterfall.
//
// transientParent is cleared so the window is a peer of the main one rather
// than a child kept above it: it has its own taskbar entry and can sit behind
// the spectrum on the same screen or alone on another. Where it was and
// whether it was open are remembered in main.cpp, and the top bar's
// "receivers" button brings it back after it is closed.

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

    // Commands.qml, which this window's palette runs its entries through.
    property var commands: null

    // What the keys reach into here.
    readonly property alias receiverDial: detail.dial
    readonly property alias passband: detail.passband
    readonly property alias commandPalette: commandPalette
    readonly property alias keyMapView: keyMapView

    // Closes the window when it is open, and opens it and puts it in front
    // when it is not: the top bar's "receivers" button and its key.
    function toggle() {
        if (receivers.visible) {
            receivers.close()
        } else {
            receivers.show()
            receivers.raise()
        }
    }

    CommandPalette {
        id: commandPalette
        commands: receivers.commands
    }

    KeyMapView {
        id: keyMapView
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 14

        ReceiverRack {
            Layout.preferredWidth: 260
            Layout.minimumWidth: 260
            Layout.maximumWidth: 260
            Layout.fillHeight: true
        }

        Rectangle {
            Layout.fillHeight: true
            implicitWidth: 1
            color: Theme.border
        }

        ColumnLayout {
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
            // so RDS and audio stay at the bottom where they were.
            Item {
                Layout.fillHeight: true
                visible: engineLink.receiverId === 0
            }

            RdsPane { Layout.minimumWidth: 0 }
            DecodePane { Layout.minimumWidth: 0 }
            AudioPane { Layout.minimumWidth: 0 }
        }
    }
}
