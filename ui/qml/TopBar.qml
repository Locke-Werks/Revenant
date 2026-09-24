// The one strip of chrome above the span: tuning on the left, and on the
// right the handful of things an operator reaches for less often, each of
// which opens a panel over the span rather than a row under the bar.
//
// WHY THE REST OF THE OLD COLUMN IS NOT HERE. The window used to stack
// twenty-odd rows above the spectrum, each drawn whenever its condition held,
// and the spectrum got whatever height was left. The owner's words for it on
// 2026-09-22 were cluttered and confusing, against a design brief of
// "intuitive but not simple, powerful but not overwhelming". So the rows are
// now one gesture away instead of always present: the radio and the frequency
// manager in panels, the diagnostics behind the status pill, and each fault
// once, either named by the pill or as a chip beside it. The gain and the
// detector's settings are in the control row under this bar, ControlRow.qml.
//
// WHAT THE END OF THAT SENTENCE USED TO SAY: "the radio, the detection
// thresholds and the frequency manager in panels, the diagnostics behind the
// status pill, and faults in a banner that exists only while they do." The
// banner showed each fault as a chip under the bar while the pill named the
// first of them beside the receivers button, so the owner saw "receiver let
// go" twice; on 2026-09-23 the banner's row became the control row and the
// chips came up here.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Rectangle {
    id: bar

    // The radio's dial and the digit the tuning keys step, for Commands.qml.
    readonly property alias tuneDial: tuneBar.dial
    property int tuneDigit: -1

    // Opens one of the bar's panels by name, or closes it if it is open, the
    // way its button does. The keys reach the panels through this.
    function togglePanel(name) {
        const panel = name === "radio" ? radioPanel
                    : name === "memories" ? memoriesPanel
                    : null
        if (panel === null)
            return
        if (panel.opened)
            panel.close()
        else
            panel.open()
    }

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

    // The sentence behind a notice, by the label models/status_summary.h
    // gives it: the engine's or this client's own words, verbatim, which the
    // pill carries on hover when it names the notice and a chip carries when
    // it does not. Empty for a headline with no sentence of its own.
    function noticeDetail(label) {
        switch (label) {
        case "no engine":
            return engineLink.errorText.length > 0
                   ? engineLink.errorText
                   : "waiting for an engine at " + engineLink.endpoint
        case "front end overloaded":
            return engineLink.frontEndText
        case "source behind":
            return engineLink.pacingText
        case "detector refused":
            return engineLink.detectionFault
        case "radio refused":
            return engineLink.sourceFault
        case "tune refused":
            return engineLink.tuneFault
        case "receiver let go":
            return engineLink.receiverGoneText
        }
        return ""
    }

    // A refusal of something the operator asked for is a warning; the rest
    // mean the picture is not the radio's. The same inks the strip used.
    function noticeInk(label) {
        return label === "tune refused" || label === "receiver let go"
               ? Theme.inkWarn : Theme.inkBad
    }

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
            id: tuneBar
            Layout.minimumWidth: 0
            keyDigit: bar.tuneDigit
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

        // The frequency manager. Shown with no engine too: the memories are
        // a file on this machine, and importing, editing and exporting them
        // need no radio.
        RButton {
            id: memoriesButton
            flat: true
            text: "memories"
            ink: Theme.inkDim
            onClicked: memoriesPanel.opened ? memoriesPanel.close() : memoriesPanel.open()

            // Each of the bar's buttons names its key on hover, so the keys
            // are found where the mouse already is.
            Tip {
                visible: parent.hovered && !memoriesPanel.opened
                text: "the frequency manager  " + KeyMap.keysText("panel.memories")
            }

            // Wider than the bar's other panels, so it is kept inside the
            // window's left edge as well as opening leftwards from the button:
            // the bar starts at the window's edge, so the button's x in the
            // row plus the row's margin is its distance from that edge.
            Popover {
                id: memoriesPanel
                parent: memoriesButton
                x: Math.max(8 - memoriesButton.x - 12, memoriesButton.width - width)
                onOpened: memoriesView.opened()

                FrequencyManagerPanel {
                    id: memoriesView
                    Layout.preferredWidth: Math.min(900, bar.width - 40)
                    popover: memoriesPanel
                }
            }
        }

        RButton {
            id: radioButton
            flat: true
            text: engineLink.sourceOpen ? "radio" : "radio: none open"
            ink: engineLink.sourceOpen ? Theme.inkDim : Theme.inkWarn
            visible: engineLink.connected
            onClicked: radioPanel.opened ? radioPanel.close() : radioPanel.open()

            Tip {
                visible: parent.hovered && !radioPanel.opened
                text: "choose a radio or open a recording  " + KeyMap.keysText("panel.radio")
            }

            Popover {
                id: radioPanel
                parent: radioButton
                SourcePicker {}
            }
        }

        // The receivers, docked under the span or in their own window.
        // Checked while they are shown, wherever that is, and a click hides
        // them or brings them back.
        RButton {
            flat: true
            checkable: true
            checked: receiverPlacement.shown
            text: "receivers"
            ink: Theme.inkDim
            tint: Theme.receiverColours[engineLink.focusedSlot]
            onClicked: receiverPlacement.shown = !receiverPlacement.shown

            Tip {
                visible: parent.hovered
                text: (receiverPlacement.shown ? "hide the receivers  " : "show the receivers  ")
                      + KeyMap.keysText("window.receivers")
            }
        }

        // Every notice the pill does not name, each a chip with its sentence
        // verbatim on hover; see controls/StatusChip.qml. The pill names the
        // first and these are the rest, so no notice is on screen twice.
        Repeater {
            model: bar.status.chips

            StatusChip {
                required property string modelData

                label: modelData
                detail: bar.noticeDetail(modelData)
                ink: bar.noticeInk(modelData)
            }
        }

        // The one removal an add answers. A retune that moved a receiver to a
        // place in its channel needing a different filter was refused only
        // because the engine builds a new filter for a new receiver, so this
        // puts one back at the frequency it was on, in the same mode. Offered
        // for nothing else, because anything else would be refused again; see
        // receiver_can_come_back in models/receiver_gone.h. A chip that acts,
        // not a dialog: the sentence beside it is the question. Beside the
        // "receiver let go" notice, which is the pill or the chip before it.
        StatusChip {
            visible: engineLink.receiverGoneText.length > 0 && engineLink.receiverComebackHz > 0
            label: "add it back"
            detail: "Adds a receiver again on the frequency the retune let go, in the same mode. "
                    + "The engine builds the filter its new place in the channel needs."
            ink: Theme.inkWarn

            Accessible.role: Accessible.Button
            Accessible.name: "add the receiver back"

            TapHandler {
                onTapped: engineLink.addGoneReceiverBack()
            }
        }

        // The status pill: a dot and a word or two, and everything behind it
        // in the drawer. models/status_summary.h decides what it says. When it
        // names a notice it is that notice's chip too: the sentence is on its
        // hover and no chip repeats it.
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

            Tip {
                visible: pill.hovered && !drawer.opened
                delay: 250
                text: {
                    const sentence = bar.noticeDetail(bar.status.headline)
                    return sentence.length > 0 ? sentence : "the engine's status, and the drawer"
                }
            }

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
