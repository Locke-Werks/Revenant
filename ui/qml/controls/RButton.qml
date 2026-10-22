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

    // An icon from controls/icons.js. With one, the button is the icon alone
    // and text moves to the tooltip and the accessible name, unless label is
    // set: then label is drawn on the button beside it. The rule, from the
    // owner on 2026-10-22: an icon that says what the button does stands
    // alone, and one that does not gets its words on the button.
    property string iconName: ""
    property string label: ""

    // The icon after the label rather than before it, for a menu's chevron.
    property bool iconAfter: false

    // What the hover says. Defaults to text for an icon alone, since that is
    // the only place its name is then written.
    property string tip: iconOnly ? text : ""

    readonly property bool iconOnly: iconName.length > 0 && label.length === 0
    readonly property color inkNow: !enabled ? Theme.inkOff
                                    : checked ? tint
                                    : ink

    implicitHeight: Theme.controlHeight
    leftPadding: iconOnly ? 5 : flat ? 6 : 10
    rightPadding: iconOnly ? 5 : flat ? 6 : 10
    topPadding: 0
    bottomPadding: 0
    font.family: Theme.uiFont
    font.pixelSize: Theme.sizeBody
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Accessible.name: text.length > 0 ? text : label

    contentItem: Item {
        // Measured from the parts rather than from the row, whose width
        // follows the label's elided width, which follows the button's: a
        // loop that settled on a button too narrow for its own label.
        implicitWidth: (control.iconName.length > 0 ? Theme.iconSize : 0)
                       + (control.iconName.length > 0 && !control.iconOnly ? row.spacing : 0)
                       + (control.iconOnly ? 0 : words.implicitWidth)
        implicitHeight: Math.max(Theme.iconSize, words.implicitHeight)

        Row {
            id: row
            anchors.verticalCenter: parent.verticalCenter
            x: control.alignment === Text.AlignLeft ? 0 : Math.max(0, (parent.width - width) / 2)
            spacing: 5
            layoutDirection: control.iconAfter ? Qt.RightToLeft : Qt.LeftToRight

            Icon {
                visible: control.iconName.length > 0
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.iconSize
                height: Theme.iconSize
                name: control.iconName
                color: control.inkNow
            }

            Text {
                id: words
                visible: !control.iconOnly
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, Math.max(0, control.availableWidth
                       - (control.iconName.length > 0 ? Theme.iconSize + 5 : 0)))
                text: control.iconName.length > 0 ? control.label : control.text
                font: control.font
                color: control.inkNow
                horizontalAlignment: control.alignment
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }
    }

    Tip {
        text: control.tip
        visible: control.hovered && control.tip.length > 0
    }

    background: Rectangle {
        implicitWidth: Theme.controlHeight
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
