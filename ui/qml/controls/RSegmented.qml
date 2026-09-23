// A row of mutually exclusive choices that reads as one control: the mode
// selector, the RDS region. Every option is visible at once, because these
// are choices an operator makes by looking at the alternatives, and the one
// in force is filled.
//
// It does not hold the choice. `current` is bound by the caller to whatever
// the engine says is in force, and `picked` asks for a change; the fill moves
// when the answer comes back, not when the click lands.

import QtQuick
import QtQuick.Controls
import Revenant

Rectangle {
    id: group

    property var options: []
    property string current
    property color tint: Theme.accent

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
    }
}
