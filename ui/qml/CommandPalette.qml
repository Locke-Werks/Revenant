// The command palette: a search field over every action the window has and
// every band, opened from the keyboard and run from it.
//
// NOT MODAL, on the rule controls/Popover.qml states: nothing behind it is
// dimmed or blocked, the spectrum keeps drawing, and a click anywhere else
// closes it as Esc does. It hangs near the top of whichever window opened it
// rather than in the middle, so the span stays visible under it.
//
// What it lists and in what order is models/palette_match.h, through
// KeyMap.palette; this file draws the answer and moves a selection through it.
// Its own keys are the table's overlay keys, looked up rather than written
// here.
//
// Its text sets a size and no family, so it takes the application's font,
// which main.cpp gives a fallback. A family named on one Text has none, and
// drew this palette in a serif on the offscreen platform.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Popup {
    id: commandPalette

    // Commands.qml, which runs what is chosen and says what the window has.
    property var commands: null

    // "bands" for the band jump, empty for everything.
    property string scope: ""

    // Searched only while open, so a closed palette costs nothing when what
    // the window has changes.
    readonly property var matches: commandPalette.visible && commandPalette.commands !== null
                                   ? KeyMap.palette(field.text, commandPalette.scope,
                                                    commandPalette.commands.have,
                                                    engineLink.sourceTuneLowHz,
                                                    engineLink.sourceTuneHighHz, Theme.accent)
                                   : []

    readonly property int rowHeight: 30

    function openWith(scope, query) {
        commandPalette.scope = scope
        field.text = query
        commandPalette.open()
    }

    function runAt(index) {
        if (index < 0 || index >= commandPalette.matches.length
                || commandPalette.commands === null)
            return
        const chosen = commandPalette.matches[index]
        if (!chosen.enabled)
            return
        // Closed first, so an entry that opens an overlay of its own, the key
        // map or the band jump, opens it into a window with nothing over it.
        commandPalette.close()
        commandPalette.commands.run(chosen.handler, chosen.argument)
    }

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round(Math.min(parent.height * 0.12, 96))
    width: Math.min(640, parent.width - 32)
    height: Math.min(implicitHeight, parent.height - y - 24)
    padding: 8
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: {
        field.forceActiveFocus()
        list.currentIndex = 0
    }

    background: Rectangle {
        radius: Theme.radius + 2
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 6

        RTextField {
            id: field

            Layout.fillWidth: true
            implicitHeight: 32
            font.pixelSize: Theme.sizeTitle
            placeholderText: commandPalette.scope === "bands" ? "type a band"
                                                       : "type a command, a band or a mode"
            onTextChanged: list.currentIndex = 0

            Keys.onPressed: (event) => {
                switch (KeyMap.overlayCommand(event.key, event.modifiers)) {
                case "next":
                    list.currentIndex = Math.min(list.currentIndex + 1, list.count - 1)
                    event.accepted = true
                    break
                case "previous":
                    list.currentIndex = Math.max(list.currentIndex - 1, 0)
                    event.accepted = true
                    break
                case "run":
                    commandPalette.runAt(list.currentIndex)
                    event.accepted = true
                    break
                case "close":
                    commandPalette.close()
                    event.accepted = true
                    break
                }
            }
        }

        ListView {
            id: list

            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(count, 12) * commandPalette.rowHeight
            visible: count > 0
            clip: true
            model: commandPalette.matches
            boundsBehavior: Flickable.StopAtBounds
            ScrollIndicator.vertical: ScrollIndicator {}

            // Keeps the chosen row in view as the keys walk past the bottom,
            // with no scroll animation: the row moving is the keypress's
            // effect, and a slide would only delay it.
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

            delegate: ItemDelegate {
                id: row

                required property var modelData
                required property int index

                width: ListView.view.width
                height: commandPalette.rowHeight
                leftPadding: 12
                rightPadding: 8
                hoverEnabled: true
                onClicked: commandPalette.runAt(row.index)

                contentItem: RowLayout {
                    spacing: 10

                    Text {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: row.modelData.label
                        textFormat: Text.StyledText
                        clip: true
                        color: row.modelData.enabled ? Theme.ink : Theme.inkDim
                        font.pixelSize: Theme.sizeBody
                        verticalAlignment: Text.AlignVCenter
                    }

                    Text {
                        Layout.preferredWidth: 150
                        text: row.modelData.group
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignRight
                        color: row.modelData.enabled ? Theme.inkDim : Theme.inkOff
                        font.pixelSize: Theme.sizeSmall
                    }

                    // The keys, or why it cannot run. One column either way, at
                    // a fixed width so the labels do not move as rows change.
                    Item {
                        Layout.preferredWidth: 190
                        Layout.fillHeight: true

                        Row {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 4
                            visible: row.modelData.enabled

                            Repeater {
                                model: row.modelData.keys.length > 0
                                       ? row.modelData.keys.split(" or ") : []

                                KeyChip {
                                    required property string modelData
                                    keys: modelData
                                }
                            }
                        }

                        Text {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !row.modelData.enabled
                            text: row.modelData.reason
                            color: Theme.inkDim
                            font.italic: true
                            font.pixelSize: Theme.sizeSmall
                        }
                    }
                }

                background: Rectangle {
                    radius: Theme.radius
                    color: row.ListView.isCurrentItem ? Theme.controlHover
                           : row.hovered ? Theme.control : "transparent"

                    // The accent on the chosen row's edge: it is what Return runs.
                    Rectangle {
                        visible: row.ListView.isCurrentItem
                        width: 2
                        height: parent.height - 8
                        anchors.verticalCenter: parent.verticalCenter
                        x: 3
                        radius: 1
                        color: row.modelData.enabled ? Theme.accent : Theme.inkOff
                    }
                }
            }
        }

        Text {
            visible: list.count === 0
            Layout.fillWidth: true
            Layout.preferredHeight: commandPalette.rowHeight
            verticalAlignment: Text.AlignVCenter
            leftPadding: 12
            text: "nothing matches"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        // How to drive it, from the table rather than written out.
        Text {
            Layout.fillWidth: true
            leftPadding: 4
            text: KeyMap.keysText("overlay.run") + " runs, "
                  + KeyMap.keysText("overlay.close") + " closes, "
                  + KeyMap.keysText("keymap.open") + " shows every key"
            color: Theme.inkDim
            elide: Text.ElideRight
            font.pixelSize: Theme.sizeSmall
        }
    }
}
