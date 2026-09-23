// A slider drawn from the Theme: a thin track, the filled part in the accent,
// a small handle. The value, the step and the snapping are the caller's, and
// several callers carry long notes about exactly those, so nothing here sets
// them.

import QtQuick
import QtQuick.Controls
import Revenant

Slider {
    id: control

    property color tint: Theme.accent

    implicitHeight: Theme.controlHeight
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    background: Item {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        implicitWidth: 120
        implicitHeight: 4
        width: control.availableWidth
        height: 4

        Rectangle {
            anchors.fill: parent
            radius: 2
            color: Theme.border
        }

        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: control.enabled ? control.tint : Theme.inkOff
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        implicitWidth: 12
        implicitHeight: 12
        radius: 6
        color: control.pressed ? control.tint : Theme.panelSolid
        border.width: 2
        border.color: !control.enabled ? Theme.inkOff
                      : control.hovered || control.pressed || control.visualFocus
                        ? control.tint : Theme.ink
    }
}
