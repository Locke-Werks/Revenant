// Decoding, and the log of what was decoded.
//
// OFFERED ONLY WHERE SOMETHING READS THE RECEIVER. The menu is the engine's
// own list cut to the decoders that read this receiver's mode, with auto
// first where it would attach anything, so the section is absent on am, dsb
// and wfm rather than a menu of refusals. WFM keeps its RDS section above.
// models/decoded_log.h has the rule and ui/tests/test_decoded_log.cpp its
// cases.
//
// A REFUSAL IS A CHIP. The engine refuses a pairing in a sentence naming the
// modes the decoder needs, and that sentence is the chip's tooltip, verbatim.
// An encrypted call is not a refusal and not an error: the line carries a dim
// chip saying so, with the talkgroup, and the decoder carries on.
//
// THE LOG FOLLOWS THE NEWEST LINE until the operator scrolls up to read, and
// then holds still until they come back down or ask for the newest. A click
// on a line opens its fields; copy and clear act on a line or on the whole.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: pane

    readonly property var log: engineLink.decodedLog
    readonly property bool offered: engineLink.receiverId > 0
                                    && engineLink.decodeChoices.length > 0

    Layout.fillWidth: true
    spacing: 4
    visible: offered

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 8

        RButton {
            flat: true
            checkable: true
            checked: engineLink.decodeWanted
            text: "decode"
            tint: Theme.receiverColours[engineLink.focusedSlot]
            ink: Theme.inkDim
            font.bold: true
            onClicked: engineLink.decodeWanted = !engineLink.decodeWanted
        }

        RComboBox {
            id: choice

            Layout.preferredWidth: 110
            model: engineLink.decodeChoices
            currentIndex: engineLink.decodeChoices.indexOf(engineLink.decodeChoice)
            onActivated: (index) => engineLink.decodeChoice = engineLink.decodeChoices[index]

            HoverHandler { id: choiceHover }

            // The engine's own sentence about the decoder in force, on hover,
            // drawn as StatusChip draws its detail. Auto has none of its own;
            // what it attached is named beside the menu.
            ToolTip {
                id: choiceTip

                readonly property string sentence:
                    engineLink.decoderDescription(engineLink.decodeChoice)

                visible: choiceHover.hovered && !choice.popup.visible && sentence.length > 0
                delay: 400
                y: choice.height + 4
                width: Math.min(tipText.implicitWidth + 20, 460)
                padding: 10

                contentItem: Text {
                    id: tipText
                    text: choiceTip.sentence
                    color: Theme.ink
                    wrapMode: Text.WordWrap
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.sizeBody
                }

                background: Rectangle {
                    radius: Theme.radius
                    color: Theme.panelSolid
                    border.width: 1
                    border.color: Theme.border
                }
            }
        }

        // What is attached right now, which for auto is the answer to "which
        // ones". Only while the switch is on.
        Label {
            visible: engineLink.decodeWanted && engineLink.decodeAttached.length > 0
            text: engineLink.decodeAttached
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            elide: Text.ElideRight
            Layout.maximumWidth: 220
        }

        StatusChip {
            visible: engineLink.decodeLabel.length > 0
            label: engineLink.decodeLabel
            detail: engineLink.decodeDetail
            ink: Theme.inkWarn
        }

        StatusChip {
            visible: pane.log.droppedLabel.length > 0
            label: pane.log.droppedLabel
            detail: pane.log.droppedDetail
            ink: Theme.inkDim
        }

        Item { Layout.fillWidth: true }

        RButton {
            visible: !logView.follow
            flat: true
            text: "newest"
            ink: Theme.accent
            onClicked: logView.toTail()
        }

        Readout {
            widest: "0000 lines"
            text: pane.log.count + (pane.log.count === 1 ? " line" : " lines")
            font.pixelSize: Theme.sizeSmall
        }

        RButton {
            flat: true
            text: "copy all"
            ink: Theme.inkDim
            enabled: pane.log.count > 0
            onClicked: pane.log.copyAll()
        }

        RButton {
            flat: true
            text: "clear"
            ink: Theme.inkDim
            enabled: pane.log.count > 0 || pane.log.droppedLabel.length > 0
            onClicked: pane.log.clear()
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredHeight: 176
        radius: Theme.radius
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border

        // Nothing yet, said where the lines will be.
        Label {
            anchors.centerIn: parent
            visible: pane.log.count === 0
            text: engineLink.decodeWanted ? "Nothing decoded yet."
                                          : "Decoding is off. Turn it on to fill the log."
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        ListView {
            id: logView

            // Following the newest line. Set false by the operator scrolling
            // away from it and true again by scrolling back or by "newest".
            property bool follow: true
            property bool placing: false

            function toTail() {
                placing = true;
                positionViewAtEnd();
                placing = false;
                follow = true;
            }

            anchors.fill: parent
            anchors.margins: 4
            clip: true
            model: pane.log
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            onCountChanged: {
                if (count === 0) {
                    follow = true;
                } else if (follow) {
                    Qt.callLater(toTail);
                }
            }
            onContentYChanged: {
                if (!placing) {
                    follow = pane.log.atTail(contentY - originY, height, contentHeight);
                }
            }

            TextMetrics {
                id: timeWidth
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
                text: "00:00:00.0"
            }

            TextMetrics {
                id: decoderWidth
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
                text: "sitor_b"
            }

            TextMetrics {
                id: keyWidth
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
                text: "golay_worst_correction"
            }

            delegate: Item {
                id: entry

                required property int index
                required property string time
                required property string decoder
                required property string text
                required property string chip
                required property bool expanded
                required property var fields

                width: ListView.view.width
                height: line.height + (entry.expanded ? detail.height + 4 : 0)

                Rectangle {
                    anchors.fill: parent
                    color: hover.hovered ? Theme.controlHover : "transparent"
                    radius: 2
                }

                HoverHandler { id: hover }

                // The click that opens a line. Under the copy button, which
                // is declared after it and so takes its own clicks first.
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: pane.log.toggleExpanded(entry.index)
                }

                RowLayout {
                    id: line

                    x: 4
                    width: parent.width - 8
                    height: 20
                    spacing: 10

                    Text {
                        Layout.preferredWidth: timeWidth.advanceWidth
                        text: entry.time
                        color: Theme.inkDim
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }

                    Text {
                        Layout.preferredWidth: decoderWidth.advanceWidth
                        text: entry.decoder
                        color: Theme.receiverColours[engineLink.focusedSlot]
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                        elide: Text.ElideRight
                    }

                    // An encrypted call, said quietly. Never an error.
                    StatusChip {
                        visible: entry.chip.length > 0
                        label: entry.chip
                        ink: Theme.inkDim
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: entry.text
                        color: entry.chip.length > 0 ? Theme.inkDim : Theme.ink
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                        elide: Text.ElideRight
                    }

                    RButton {
                        visible: hover.hovered
                        flat: true
                        implicitHeight: 18
                        text: "copy"
                        ink: Theme.inkDim
                        font.pixelSize: Theme.sizeSmall
                        onClicked: pane.log.copyLine(entry.index)
                    }
                }

                // The whole line wrapped, then every field, in two columns.
                Column {
                    id: detail

                    visible: entry.expanded
                    x: 4 + timeWidth.advanceWidth + 10
                    y: line.height
                    width: parent.width - x - 8
                    spacing: 1

                    Text {
                        width: parent.width
                        text: entry.text
                        color: Theme.ink
                        wrapMode: Text.WrapAnywhere
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }

                    Repeater {
                        model: entry.expanded ? entry.fields : []

                        delegate: Row {
                            required property var modelData

                            spacing: 10

                            Text {
                                width: keyWidth.advanceWidth
                                text: modelData.key
                                color: Theme.inkDim
                                font.family: Theme.monoFont
                                font.pixelSize: Theme.sizeSmall
                                elide: Text.ElideRight
                            }

                            Text {
                                width: detail.width - keyWidth.advanceWidth - 10
                                text: modelData.value
                                color: Theme.ink
                                wrapMode: Text.WrapAnywhere
                                font.family: Theme.monoFont
                                font.pixelSize: Theme.sizeSmall
                            }
                        }
                    }
                }
            }
        }
    }
}
