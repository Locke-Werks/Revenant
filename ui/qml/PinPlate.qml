// One end of the colour map as the spectrum draws it, and the pin on it.
//
// The plate says where the end is, in dBFS, as it always has. Beside it is
// the pin: click it to hold the end where it is drawn now, and while it is
// held the plate says "pinned" in the accent and two small buttons move it a
// decibel at a time. Unpin and the end follows the frame again. The rules,
// where a pin lands and how two pins keep apart, are render/spectrum_scale.h.

import QtQuick
import QtQuick.Controls
import Revenant

Row {
    id: pin

    // What the plate prints for the end, and which end.
    property string text
    property bool pinned: false

    // More about the end, on hover of the plate. Empty for none.
    property string detail

    signal pinRequested()
    signal unpinRequested()
    signal nudged(int steps)

    spacing: 2

    Plate {
        text: pin.pinned ? pin.text + "  pinned" : pin.text
        anchors.verticalCenter: parent.verticalCenter

        HoverHandler { id: plateHover }

        Tip {
            visible: plateHover.hovered && pin.detail.length > 0
            text: pin.detail
        }
    }

    RButton {
        implicitHeight: 18
        leftPadding: 5
        rightPadding: 5
        flat: true
        checkable: true
        checked: pin.pinned
        text: pin.pinned ? "unpin" : "pin"
        ink: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        onClicked: pin.pinned ? pin.unpinRequested() : pin.pinRequested()
    }

    RButton {
        visible: pin.pinned
        implicitHeight: 18
        leftPadding: 5
        rightPadding: 5
        flat: true
        text: "−"
        ink: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        onClicked: pin.nudged(-1)
    }

    RButton {
        visible: pin.pinned
        implicitHeight: 18
        leftPadding: 5
        rightPadding: 5
        flat: true
        text: "+"
        ink: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        onClicked: pin.nudged(1)
    }
}
