// A row of mutually exclusive choices that reads as one control: the mode
// selector, the RDS region. Every option is visible at once, because these
// are choices an operator makes by looking at the alternatives, and the one
// in force is filled.
//
// It does not hold the choice. `current` is bound by the caller to whatever
// the engine says is in force, and `picked` asks for a change; the fill moves
// when the answer comes back, not when the click lands.
//
// AN OVERFLOW GROUP, when `overflow` is not empty: one more segment at the
// end that opens a short list of further choices under it, for options an
// operator reaches for less often than the row's and that would widen it past
// the room it has. The caller says what the segment reads and whether one of
// its choices is in force, from models/mode_choice.h for the mode selector,
// so this file does no matching of its own. The list is a Popup and not
// modal: nothing behind it is blocked, and a click anywhere else closes it.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Rectangle {
    id: group

    property var options: []
    property string current
    property color tint: Theme.accent

    // Each entry a map of name, which `picked` carries, and label, which the
    // list shows.
    property var overflow: []
    property string overflowText
    property bool overflowChosen: false

    signal picked(string option)

    implicitWidth: strip.implicitWidth + 2
    implicitHeight: Theme.controlHeight
    radius: Theme.radius
    color: Theme.control
    border.width: 1
    border.color: Theme.border

    Row {
        id: strip

        anchors.fill: parent
        anchors.margins: 1

        Repeater {
            model: group.options

            Rectangle {
                id: segment

                required property string modelData
                required property int index

                readonly property bool chosen: group.current === modelData

                width: label.implicitWidth + 16
                height: strip.height
                radius: Theme.radius - 1
                color: chosen ? Qt.rgba(group.tint.r, group.tint.g, group.tint.b, 0.18)
                       : hover.hovered ? Theme.controlHover
                       : "transparent"

                Text {
                    id: label
                    anchors.centerIn: parent
                    text: segment.modelData
                    color: segment.chosen ? group.tint : Theme.ink
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.sizeBody
                    font.bold: segment.chosen
                }

                HoverHandler {
                    id: hover
                    cursorShape: Qt.PointingHandCursor
                }

                TapHandler {
                    onTapped: group.picked(segment.modelData)
                }
            }
        }

        Rectangle {
            id: more

            visible: group.overflow.length > 0
            width: visible ? moreLabel.implicitWidth + 16 : 0
            height: strip.height
            radius: Theme.radius - 1
            color: group.overflowChosen
                   ? Qt.rgba(group.tint.r, group.tint.g, group.tint.b, 0.18)
                   : moreHover.hovered || list.visible ? Theme.controlHover
                   : "transparent"

            Text {
                id: moreLabel
                anchors.centerIn: parent
                text: group.overflowText + " ▾"
                color: group.overflowChosen ? group.tint : Theme.inkDim
                font.family: Theme.uiFont
                font.pixelSize: Theme.sizeBody
                font.bold: group.overflowChosen
            }

            HoverHandler {
                id: moreHover
                cursorShape: Qt.PointingHandCursor
            }

            TapHandler {
                onTapped: list.visible ? list.close() : list.open()
            }

            Popup {
                id: list

                y: more.height + 2
                padding: 1
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

                // A layout so every row is as wide as the widest label, and
                // the hover fill reads as one list rather than ragged tabs.
                contentItem: ColumnLayout {
                    spacing: 0

                    Repeater {
                        model: group.overflow

                        Rectangle {
                            id: entry

                            required property var modelData

                            readonly property bool chosen: group.current === modelData.name

                            Layout.fillWidth: true
                            implicitWidth: Math.max(entryLabel.implicitWidth + 24, more.width)
                            implicitHeight: Theme.controlHeight
                            color: entryHover.hovered ? Theme.controlHover : "transparent"

                            Text {
                                id: entryLabel
                                anchors.verticalCenter: parent.verticalCenter
                                x: 12
                                text: entry.modelData.label
                                color: entry.chosen ? group.tint : Theme.ink
                                font.family: Theme.uiFont
                                font.pixelSize: Theme.sizeBody
                                font.bold: entry.chosen
                            }

                            HoverHandler {
                                id: entryHover
                                cursorShape: Qt.PointingHandCursor
                            }

                            TapHandler {
                                onTapped: {
                                    list.close()
                                    group.picked(entry.modelData.name)
                                }
                            }
                        }
                    }
                }

                background: Rectangle {
                    radius: Theme.radius
                    color: Theme.panelSolid
                    border.width: 1
                    border.color: Theme.border
                }
            }
        }
    }
}
