// One key sequence, drawn as a keycap: the monospace family in a small
// outlined box, so a sequence reads as something to press rather than as a
// word in the sentence beside it.
//
// The text is the table's, from ui/models/key_actions.h, and never typed here.

import QtQuick
import Revenant

Rectangle {
    id: chip

    property string keys

    implicitWidth: label.implicitWidth + 12
    implicitHeight: 18
    radius: 3
    color: Theme.control
    border.width: 1
    border.color: Theme.border

    Text {
        id: label
        anchors.centerIn: parent
        text: chip.keys
        color: Theme.ink
        font.family: Theme.monoFont
        font.pixelSize: Theme.sizeSmall
    }
}
