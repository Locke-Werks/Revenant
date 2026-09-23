// The one strip of chrome above the span: tuning on the left, and on the
// right the handful of things an operator reaches for less often, each of
// which opens a panel over the span rather than a row under the bar.
//
// WHY THE REST OF THE OLD COLUMN IS NOT HERE. The window used to stack
// twenty-odd rows above the spectrum, each drawn whenever its condition held,
// and the spectrum got whatever height was left. The owner's words for it on
// 2026-09-22 were cluttered and confusing, against a design brief of
// "intuitive but not simple, powerful but not overwhelming". So the rows are
// now one gesture away instead of always present: the radio, the detection
// thresholds and the bookmarks in panels, the diagnostics behind the status
// pill, and faults in a banner that exists only while they do.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Rectangle {
    id: bar

    // The receiver window, so the bar can say whether it is open and open it
    // again after it was closed.
    property var receiverWindow: null

    readonly property var status: UiRules.status({
        "connected": engineLink.connected,
        "engineRunning": engineLink.engineRunning,
        "sourceOpen": engineLink.sourceOpen,
        "sourceBehind": engineLink.sourceBehind,
        "frontEndFault": engineLink.frontEndFault,
        "detectionFault": engineLink.detectionFault.length > 0,
        "tuneFault": engineLink.tuneFault.length > 0,
        "sourceFault": engineLink.sourceFault.length > 0,
        "receiverGone": engineLink.receiverGoneText.length > 0,
        "clamped": engineLink.clamped,
        "stranded": engineLink.strandedReceiverText.length > 0,
        "framesDroppedByEngine": engineLink.framesDroppedByEngine,
        "frameRate": engineLink.frameRate
    })

    implicitHeight: 52
    color: Theme.panelSolid

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 12

        TuneBar {
            Layout.minimumWidth: 0
        }

        // While there is no engine the tuning controls are hidden, and the
        // bar says what it is waiting for in their place.
        Label {
            visible: !engineLink.connected
            text: "waiting for an engine at " + engineLink.endpoint
            color: Theme.inkWarn
            font.pixelSize: Theme.sizeTitle
            font.bold: true
        }

        Item { Layout.fillWidth: true }

        RButton {
            id: marksButton
            flat: true
            text: "marks"
            ink: Theme.inkDim
            visible: engineLink.connected
            onClicked: marksPanel.opened ? marksPanel.close() : marksPanel.open()

            Popover {
                id: marksPanel
                parent: marksButton
                BookmarkRow {
                    Layout.preferredWidth: 640
                }
            }
        }

        RButton {
            id: detectionsButton
            flat: true
            text: "detections"
            ink: Theme.inkDim
            visible: engineLink.connected && engineLink.spectrumEnabled
            onClicked: detectionsPanel.opened ? detectionsPanel.close() : detectionsPanel.open()

            Popover {
                id: detectionsPanel
                parent: detectionsButton
                DetectionControls {}
            }
        }

        RButton {
            id: radioButton
            flat: true
            text: engineLink.sourceOpen ? "radio" : "radio: none open"
            ink: engineLink.sourceOpen ? Theme.inkDim : Theme.inkWarn
            visible: engineLink.connected
            onClicked: radioPanel.opened ? radioPanel.close() : radioPanel.open()

            Popover {
                id: radioPanel
                parent: radioButton
                SourcePicker {}
            }
        }

        // The receiver window. Checked while it is open, and a click brings it
        // back after it was closed, which is the only way back to it.
        RButton {
            flat: true
            checkable: true
            checked: bar.receiverWindow !== null && bar.receiverWindow.visible
            text: "receivers"
            ink: Theme.inkDim
            tint: Theme.receiverColours[0]
            onClicked: {
                if (bar.receiverWindow === null)
                    return
                if (bar.receiverWindow.visible) {
                    bar.receiverWindow.close()
                } else {
                    bar.receiverWindow.show()
                    bar.receiverWindow.raise()
                }
            }
        }

        // The status pill: a dot and a word or two, and everything behind it
        // in the drawer. models/status_summary.h decides what it says.
        RButton {
            id: pill

            readonly property color level: bar.status.level >= 3 ? Theme.inkBad
                                          : bar.status.level === 2 ? Theme.inkWarn
                                          : bar.status.level === 1 ? Theme.ink
                                          : Theme.accent

            flat: true
            leftPadding: 22
            alignment: Text.AlignLeft

            // As wide as the longest headline, so the buttons to its left do
            // not move each time the rate's digits change.
            Layout.preferredWidth: widest.advanceWidth + 36
            text: bar.status.headline
            ink: bar.status.level >= 2 ? pill.level : Theme.inkDim
            onClicked: drawer.opened ? drawer.close() : drawer.open()

            TextMetrics {
                id: widest
                font: pill.font
                text: "front end overloaded"
            }

            Rectangle {
                x: 9
                anchors.verticalCenter: parent.verticalCenter
                width: 7
                height: 7
                radius: 3.5
                color: pill.level
            }

            Popover {
                id: drawer
                parent: pill
                width: 820
                StatusDrawer {
                    Layout.fillWidth: true
                }
            }
        }
    }
}
