// Trunk following: the engine's P25 follower switched on the focused
// control channel receiver, the calls it saw with their talkgroup aliases,
// and the lists that decide which it follows.
//
// THE FOLLOWER IS THE ENGINE'S. This only switches it, edits the lists and
// shows the log; core/engine/trunk_follower.h has the rules. An encrypted call
// is a dim chip and is never followed or played. A call with no receiver says
// why in its last column: denied, every follower busy, outside the span.
//
// The alias table is this client's, kept in settings, and lines that do not
// read as "talkgroup = label" are ignored.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: pane

    readonly property bool onControl: engineLink.receiverDemod === "p25p1"
    property bool editing: false

    Layout.fillWidth: true
    spacing: 4
    visible: onControl || engineLink.trunkEnabled || engineLink.trunkCalls.length > 0

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 8

        Label {
            text: "trunking"
            color: Theme.inkDim
            font.bold: true
            font.pixelSize: Theme.sizeBody
        }

        RButton {
            flat: true
            checkable: true
            checked: engineLink.trunkEnabled
            enabled: engineLink.trunkEnabled || pane.onControl
            text: "follow"
            iconName: "crosshair"
            label: "follow"
            onClicked: engineLink.setTrunkFollow(!engineLink.trunkEnabled)
        }

        Text {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: engineLink.trunkSummary
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            elide: Text.ElideRight
        }

        RButton {
            flat: true
            checkable: true
            checked: pane.editing
            text: "lists"
            iconName: "list"
            label: "lists"
            onClicked: pane.editing = !pane.editing
        }
    }

    GridLayout {
        visible: pane.editing
        Layout.fillWidth: true
        columns: 2
        columnSpacing: 8
        rowSpacing: 4

        Label { text: "followers"; color: Theme.inkDim; font.pixelSize: Theme.sizeSmall }
        RTextField {
            // The digits plus the padding, so two of them do not clip when
            // the font rounds up at 150% display scaling.
            Layout.preferredWidth: Math.max(60, contentWidth + leftPadding + rightPadding + 4)
            mono: true
            text: engineLink.trunkMaxFollowers
            validator: IntValidator { bottom: 1; top: 16 }
            onEditingFinished: engineLink.trunkMaxFollowers = Number(text)
        }

        Label { text: "allow"; color: Theme.inkDim; font.pixelSize: Theme.sizeSmall }
        RTextField {
            Layout.fillWidth: true
            mono: true
            placeholderText: "every talkgroup; or 100, 200, 3501"
            text: engineLink.trunkAllow
            onEditingFinished: engineLink.trunkAllow = text
        }

        Label { text: "deny"; color: Theme.inkDim; font.pixelSize: Theme.sizeSmall }
        RTextField {
            Layout.fillWidth: true
            mono: true
            placeholderText: "none"
            text: engineLink.trunkDeny
            onEditingFinished: engineLink.trunkDeny = text
        }

        Label { text: "priority"; color: Theme.inkDim; font.pixelSize: Theme.sizeSmall }
        RTextField {
            Layout.fillWidth: true
            mono: true
            placeholderText: "talkgroup=priority, higher takes a busy receiver"
            text: engineLink.trunkPriority
            onEditingFinished: engineLink.trunkPriority = text
        }

        Label {
            Layout.alignment: Qt.AlignTop
            text: "aliases"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 80
            TextArea {
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
                color: Theme.ink
                placeholderText: "100 = Fire Dispatch, one per line"
                placeholderTextColor: Theme.inkDim
                text: engineLink.trunkAliases
                background: Rectangle {
                    color: Theme.control
                    border.width: 1
                    border.color: Theme.border
                    radius: Theme.radius
                }
                onEditingFinished: engineLink.trunkAliases = text
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.minimumHeight: 60
        Layout.preferredHeight: 140
        radius: Theme.radius
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border

        Label {
            anchors.centerIn: parent
            visible: engineLink.trunkCalls.length === 0
            text: pane.onControl ? "Follow this control channel to log its calls." : "No calls yet."
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        ListView {
            anchors.fill: parent
            anchors.margins: 4
            clip: true
            model: engineLink.trunkCalls
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: RScrollBar {}

            delegate: Rectangle {
                id: entry

                required property var modelData

                width: ListView.view.width
                // At least a line of the small mono text, which is taller
                // than 16 px at 150% display scaling.
                height: Math.max(20, callRow.implicitHeight + 4)
                radius: 2
                color: entry.modelData.hearing
                       ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.14)
                       : "transparent"

                RowLayout {
                    id: callRow
                    x: 4
                    width: parent.width - 8
                    height: parent.height
                    spacing: 10

                    Text {
                        text: entry.modelData.start
                        color: entry.modelData.active ? Theme.ink : Theme.inkDim
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }

                    Text {
                        Layout.preferredWidth: 140
                        text: entry.modelData.label
                        color: entry.modelData.encrypted ? Theme.inkDim : Theme.ink
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.preferredWidth: Math.max(70, implicitWidth)
                        text: entry.modelData.source
                        color: Theme.inkDim
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }

                    Text {
                        text: entry.modelData.frequency
                        color: Theme.inkDim
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }

                    StatusChip {
                        visible: entry.modelData.emergency
                        label: "emergency"
                        ink: Theme.inkBad
                    }

                    StatusChip {
                        visible: entry.modelData.encrypted
                        label: "encrypted"
                        ink: Theme.inkDim
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: entry.modelData.seconds + " s  " + entry.modelData.note
                        color: Theme.inkDim
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}
