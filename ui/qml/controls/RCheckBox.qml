// A check box drawn from the Theme.

import QtQuick
import QtQuick.Controls
import Revenant

CheckBox {
    id: control

    implicitHeight: Theme.controlHeight
    spacing: 6
    padding: 0
    font.family: Theme.uiFont
    font.pixelSize: Theme.sizeBody
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    indicator: Rectangle {
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        implicitWidth: 14
        implicitHeight: 14
        radius: 3
        color: control.checked ? Theme.accent : Theme.control
        border.width: 1
        border.color: control.checked || control.visualFocus ? Theme.accent
                      : control.hovered ? Theme.inkDim
                      : Theme.border

        Text {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            color: Theme.panelSolid
            font.pixelSize: Theme.sizeSmall
            font.bold: true
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: control.enabled ? Theme.ink : Theme.inkOff
        verticalAlignment: Text.AlignVCenter
    }
}
