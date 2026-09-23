// A word or two about something that went wrong or needs a look, with the
// whole sentence one hover away.
//
// WHY CHIPS AND NOT ROWS. The engine and this client explain a refusal in a
// sentence written to be acted on, and those sentences are deliberate: which
// limit bound, what was asked for, what the channel carries. They used to be
// printed in full, at full width, under whatever they were about, and on a
// busy receiver half the window was paragraphs. The owner's rule from
// 2026-09-22: the chip says what is wrong, the detail says why. So the words
// are unchanged and verbatim, in the tooltip, and what sits in the layout is
// the name of the problem in the colour of how bad it is.

import QtQuick
import QtQuick.Controls
import Revenant

Rectangle {
    id: chip

    // The name of the problem, shown.
    property string label

    // The full sentence, verbatim, on hover. Empty for a chip that needs none.
    property string detail

    // How loudly: Theme.inkWarn for something the operator did not get,
    // Theme.inkBad for a picture that is not the radio's, Theme.inkDim for a
    // note.
    property color ink: Theme.inkWarn

    implicitWidth: text.implicitWidth + 16
    implicitHeight: 20
    radius: 10
    color: Qt.rgba(chip.ink.r, chip.ink.g, chip.ink.b, 0.12)
    border.width: 1
    border.color: Qt.rgba(chip.ink.r, chip.ink.g, chip.ink.b, 0.45)

    Text {
        id: text
        anchors.centerIn: parent
        text: chip.label
        color: chip.ink
        font.family: Theme.uiFont
        font.pixelSize: Theme.sizeSmall
    }

    HoverHandler {
        id: hover
        cursorShape: chip.detail.length > 0 ? Qt.WhatsThisCursor : Qt.ArrowCursor
    }

    // Its own tooltip rather than the attached one, so the sentence wraps at
    // a readable width and is drawn on the dark panel rather than the Basic
    // style's light one.
    ToolTip {
        id: tip

        visible: hover.hovered && chip.detail.length > 0
        delay: 250
        y: chip.height + 4
        width: Math.min(sentence.implicitWidth + 20, 460)
        padding: 10

        contentItem: Text {
            id: sentence
            text: chip.detail
            color: Theme.ink
            wrapMode: Text.WordWrap
            font.family: Theme.uiFont
            font.pixelSize: Theme.sizeBody
        }

        background: Rectangle {
            radius: Theme.radius
            color: Theme.panelSolid
            border.width: 1
            border.color: Qt.rgba(chip.ink.r, chip.ink.g, chip.ink.b, 0.45)
        }
    }
}
