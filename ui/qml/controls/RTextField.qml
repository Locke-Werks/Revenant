// A text field drawn from the Theme. `mono` is for a field that holds a number
// the operator reads as one, a frequency or a rate, so its digits line up with
// the dials that show the same quantities.

import QtQuick
import QtQuick.Controls
import Revenant

TextField {
    id: control

    property bool mono: false

    implicitHeight: Theme.controlHeight
    leftPadding: 8
    rightPadding: 8
    topPadding: 0
    bottomPadding: 0
    verticalAlignment: TextInput.AlignVCenter
    font.family: mono ? Theme.monoFont : Theme.uiFont
    font.pixelSize: Theme.sizeBody
    color: enabled ? Theme.ink : Theme.inkOff
    placeholderTextColor: Theme.inkDim
    selectionColor: Theme.accentDim
    selectedTextColor: Theme.ink
    selectByMouse: true
    hoverEnabled: true

    background: Rectangle {
        implicitWidth: 100
        radius: Theme.radius
        color: control.enabled ? Theme.control : Theme.panelSolid
        border.width: 1
        border.color: control.activeFocus ? Theme.accent
                      : control.hovered && control.enabled ? Theme.inkDim
                      : Theme.border
    }
}
