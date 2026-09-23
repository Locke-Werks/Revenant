// A tooltip drawn from the Theme, for a control whose hover explains it.
//
// The Basic style's own tooltip is a light box, which was the brightest thing
// in the window wherever it opened, over a dark span the operator was reading.
// StatusChip and the decoder menu had each drawn their own dark one; this is
// that one, shared, and every tooltip in the window is this or StatusChip's.
//
// Declared inside the control it explains, which is its parent, with
// visible bound to that control's hover.

import QtQuick
import QtQuick.Controls
import Revenant

ToolTip {
    id: tip

    delay: 500
    padding: 8

    // Under the control, as StatusChip's is, rather than the Basic style's
    // above it, which over the top bar is off the top of the window.
    y: parent ? parent.height + 4 : 0

    // Wrapped at a readable width rather than across the window.
    width: Math.min(body.implicitWidth + leftPadding + rightPadding, 460)

    contentItem: Text {
        id: body
        text: tip.text
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
