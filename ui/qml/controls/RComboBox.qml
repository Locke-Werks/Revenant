// A drop-down drawn from the Theme, with a list that opens as a dark panel
// rather than the Basic style's white one, which was the brightest thing in
// the window every time it opened.
//
// The model, the current index and what picking does are the caller's. The
// audio device picker carries a long note about keeping currentIndex in step
// with the player, and that stays where it is.

import QtQuick
import QtQuick.Controls
import Revenant

ComboBox {
    id: control

    implicitHeight: Theme.controlHeight
    leftPadding: 8
    rightPadding: 24
    font.family: Theme.uiFont
    font.pixelSize: Theme.sizeBody
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    contentItem: Text {
        text: control.displayText
        font: control.font
        color: control.enabled ? Theme.ink : Theme.inkOff
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: control.width - width - 8
        y: (control.height - height) / 2
        text: "▾"
        color: control.enabled ? Theme.inkDim : Theme.inkOff
        font.pixelSize: Theme.sizeBody
    }

    background: Rectangle {
        implicitWidth: 120
        radius: Theme.radius
        color: control.down ? Theme.controlDown
               : control.hovered && control.enabled ? Theme.controlHover
               : Theme.control
        border.width: 1
        border.color: control.visualFocus || control.popup.visible ? Theme.accent : Theme.border
    }

    delegate: ItemDelegate {
        id: row

        required property int index
        required property var modelData

        width: ListView.view ? ListView.view.width : implicitWidth
        height: Theme.controlHeight
        leftPadding: 8
        hoverEnabled: true
        highlighted: control.highlightedIndex === index

        contentItem: Text {
            text: typeof row.modelData === "string" ? row.modelData
                  : row.modelData[control.textRole]
            font: control.font
            color: control.currentIndex === row.index ? Theme.accent : Theme.ink
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            color: row.highlighted || row.hovered ? Theme.controlHover : "transparent"
        }
    }

    popup: Popup {
        y: control.height + 2
        width: Math.max(control.width, 200)
        implicitHeight: Math.min(contentItem.implicitHeight + 2, 360)
        padding: 1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: Rectangle {
            radius: Theme.radius
            color: Theme.panelSolid
            border.width: 1
            border.color: Theme.border
        }
    }
}
