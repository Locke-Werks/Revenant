// A button drawn from the Theme.
//
// Two weights. The default is a quiet filled button for an action; Button's
// own `flat` drops the fill until the pointer is over it, which is what a row of
// shortcuts or a toggle inside a strip wants so that the row does not read
// as a wall of boxes. `checked` is the accent in both, because a toggle that
// is on is the thing the accent exists to mark.
//
// `tint` replaces the accent for a control that belongs to something with a
// colour of its own, which is a receiver.

import QtQuick
import QtQuick.Controls
import Revenant

Button {
    id: control

    property color tint: Theme.accent

    // Where the label sits. Centred for a button; the status pill reads as a
    // line of text with a dot before it, so it sets this to the left.
    property int alignment: Text.AlignHCenter

    // Ink for a button that is neither checked nor disabled, for the few
    // places a plain action has to say something about its state.
    property color ink: Theme.ink

    implicitHeight: Theme.controlHeight
    leftPadding: flat ? 6 : 10
    rightPadding: flat ? 6 : 10
    topPadding: 0
    bottomPadding: 0
    font.family: Theme.uiFont
    font.pixelSize: Theme.sizeBody
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.inkOff
               : control.checked ? control.tint
               : control.ink
        horizontalAlignment: control.alignment
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 24
        radius: Theme.radius
        color: control.down ? Theme.controlDown
               : control.hovered && control.enabled ? Theme.controlHover
               : control.flat ? "transparent"
               : Theme.control
        border.width: 1
        border.color: control.visualFocus ? control.tint
                      : control.checked ? Qt.darker(control.tint, 1.8)
                      : control.flat ? "transparent"
                      : Theme.border
    }
}
