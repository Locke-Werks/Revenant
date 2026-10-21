// The receiver waterfall's before/after split, as one chip for its corner:
// "split before | after notch", filled while the split is on.
//
// Like RSegmented it does not hold the state. `checked` is bound by the
// caller to the waterfall's `split`, and `toggled` asks for the other one.
// `live` is the waterfall's `splitLive`: with the split on and no
// before-notch rows coming, the left-hand word dims and reads "no data", the
// same thing the waterfall's own caption says, so the chip never claims a
// comparison that is not being drawn.

import QtQuick
import Revenant

Rectangle {
    id: chip

    property bool checked: false
    property bool live: false
    property color tint: Theme.accent

    signal toggled(bool on)

    implicitWidth: row.implicitWidth + 16
    implicitHeight: Theme.controlHeight
    radius: Theme.radius
    color: chip.checked ? Qt.rgba(chip.tint.r, chip.tint.g, chip.tint.b, 0.18)
           : hover.hovered ? Theme.controlHover
           : Theme.control
    border.width: 1
    border.color: chip.checked ? chip.tint : Theme.border

    Row {
        id: row

        anchors.centerIn: parent
        spacing: 6

        Text {
            text: "split"
            color: chip.checked ? chip.tint : Theme.inkDim
            font.family: Theme.uiFont
            font.pixelSize: Theme.sizeBody
            font.bold: chip.checked
        }

        Text {
            text: chip.checked && !chip.live ? "no data" : "before"
            color: chip.checked && chip.live ? Theme.ink
                   : chip.checked ? Theme.inkWarn
                   : Theme.inkDim
            font.family: Theme.uiFont
            font.pixelSize: Theme.sizeBody
        }

        Text {
            text: "|"
            color: Theme.inkDim
            font.family: Theme.uiFont
            font.pixelSize: Theme.sizeBody
        }

        Text {
            text: "after notch"
            color: chip.checked ? Theme.ink : Theme.inkDim
            font.family: Theme.uiFont
            font.pixelSize: Theme.sizeBody
        }
    }

    HoverHandler {
        id: hover
        cursorShape: Qt.PointingHandCursor
    }

    TapHandler {
        onTapped: chip.toggled(!chip.checked)
    }
}
