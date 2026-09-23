// The key map: every key the window binds, under where it applies, drawn
// from ui/models/key_actions.h through KeyMap.keyMap(). Nothing in it is
// written here, so it cannot say a key the window does not bind.
//
// Opened by its own key and from the palette, and not modal, for the reasons
// CommandPalette.qml gives. The table in docs/ui-spectrum.md is the same list,
// printed from the same table and checked against it by ui/tests.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Popup {
    id: keyMap

    readonly property var rows: KeyMap.keyMap()

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round(Math.min(parent.height * 0.08, 64))
    width: Math.min(720, parent.width - 32)
    height: Math.min(implicitHeight, parent.height - y - 24)
    padding: 12
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: list.forceActiveFocus()

    background: Rectangle {
        radius: Theme.radius + 2
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: "keys"
                color: Theme.ink
                font.pixelSize: Theme.sizeTitle
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            Label {
                text: KeyMap.keysText("palette.open") + " searches them, "
                      + KeyMap.keysText("overlay.close") + " closes"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
            }
        }

        ListView {
            id: list

            Layout.fillWidth: true
            Layout.fillHeight: true
            implicitHeight: contentHeight
            clip: true
            model: keyMap.rows
            boundsBehavior: Flickable.StopAtBounds
            ScrollIndicator.vertical: ScrollIndicator {}

            Keys.onPressed: (event) => {
                switch (KeyMap.overlayCommand(event.key, event.modifiers)) {
                case "next":
                    list.contentY = Math.min(list.contentY + 40,
                                             Math.max(0, list.contentHeight - list.height))
                    event.accepted = true
                    break
                case "previous":
                    list.contentY = Math.max(list.contentY - 40, 0)
                    event.accepted = true
                    break
                case "close":
                    keyMap.close()
                    event.accepted = true
                    break
                }
            }

            delegate: Loader {
                id: entry

                required property var modelData
                required property int index

                width: ListView.view.width
                sourceComponent: modelData.kind === "context" ? heading : action

                Component {
                    id: heading

                    Column {
                        topPadding: entry.index === 0 ? 0 : 14
                        bottomPadding: 6
                        spacing: 2

                        Text {
                            text: entry.modelData.text
                            color: Theme.accent
                            font.pixelSize: Theme.sizeBody
                            font.bold: true
                        }

                        Text {
                            width: entry.width
                            text: entry.modelData.note
                            color: Theme.inkDim
                            wrapMode: Text.WordWrap
                            font.pixelSize: Theme.sizeSmall
                        }
                    }
                }

                Component {
                    id: action

                    RowLayout {
                        height: 24
                        spacing: 10

                        Text {
                            Layout.preferredWidth: 90
                            text: entry.modelData.group
                            elide: Text.ElideRight
                            color: Theme.inkDim
                            font.pixelSize: Theme.sizeSmall
                        }

                        Text {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: entry.modelData.text
                            elide: Text.ElideRight
                            color: Theme.ink
                            font.pixelSize: Theme.sizeBody
                        }

                        Row {
                            spacing: 4

                            Repeater {
                                model: entry.modelData.keys.split(" or ")

                                KeyChip {
                                    required property string modelData
                                    keys: modelData
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
