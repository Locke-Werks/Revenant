// The bands: a few one-click shortcuts, and every band in a grouped menu.
//
// The table is models/band_plan.h, with the regulations it was taken from and
// cases in ui/tests. A click puts the front end at the band's centre, which
// the table chooses as the part of the band worth landing on rather than the
// middle of the allocation.
//
// A band the open device cannot be put on is greyed rather than hidden, so
// the menu is the same list on every radio and says which parts of it this
// one reaches. On a source that cannot retune every row is grey, and the
// tuning row says why beside the dial.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

RowLayout {
    id: picker

    readonly property var bands: UiRules.bands()

    function reachable(index) {
        return engineLink.sourceCanRetune
               && UiRules.bandReachable(index, engineLink.sourceTuneLowHz,
                                        engineLink.sourceTuneHighHz)
    }

    function tuneTo(index) {
        engineLink.tuneSourceHz(picker.bands[index].centre)
        menu.close()
    }

    spacing: 2

    Repeater {
        model: picker.bands

        RButton {
            required property var modelData
            required property int index

            visible: modelData.favourite
            flat: true
            text: modelData.name
            enabled: picker.reachable(index)
            ink: Theme.inkDim
            onClicked: picker.tuneTo(index)

            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: (modelData.low / 1e6) + " to " + (modelData.high / 1e6) + " MHz, "
                          + modelData.mode
        }
    }

    RButton {
        id: opener

        flat: true
        text: "bands ▾"
        ink: Theme.inkDim

        // Open on any source, so the list can be read on a recording too;
        // its rows are what is greyed.
        onClicked: menu.opened ? menu.close() : menu.open()
    }

    Popup {
        id: menu

        y: opener.y + opener.height + 4
        x: opener.x
        width: 300
        height: Math.min(list.contentHeight + 2, 520)
        padding: 1

        background: Rectangle {
            radius: Theme.radius
            color: Theme.panelSolid
            border.width: 1
            border.color: Theme.border
        }

        contentItem: ListView {
            id: list

            clip: true
            model: picker.bands
            ScrollIndicator.vertical: ScrollIndicator {}

            // A group's heading rides on its first row. The model is a plain
            // list, which ListView's sections cannot read a role from.
            delegate: Column {
                id: entry

                required property var modelData
                required property int index

                readonly property bool heads: index === 0
                    || picker.bands[index - 1].group !== modelData.group

                width: ListView.view.width

                Text {
                    visible: entry.heads
                    width: parent.width
                    leftPadding: 10
                    topPadding: 8
                    bottomPadding: 2
                    text: entry.modelData.group
                    color: Theme.inkDim
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.sizeSmall
                    font.bold: true
                }

                ItemDelegate {
                    id: row

                    readonly property var modelData: entry.modelData
                    readonly property int index: entry.index

                    width: entry.width
                    height: 24
                    leftPadding: 16
                    enabled: picker.reachable(index)
                    hoverEnabled: true
                    onClicked: picker.tuneTo(index)

                    contentItem: Row {
                        spacing: 10

                        Text {
                            width: 100
                            text: row.modelData.name
                            color: row.enabled ? Theme.ink : Theme.inkOff
                            font.family: Theme.uiFont
                            font.pixelSize: Theme.sizeBody
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            width: 120
                            text: (row.modelData.low / 1e6) + "–" + (row.modelData.high / 1e6)
                            color: row.enabled ? Theme.inkDim : Theme.inkOff
                            font.family: Theme.monoFont
                            font.pixelSize: Theme.sizeSmall
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: row.modelData.mode
                            color: row.enabled ? Theme.inkDim : Theme.inkOff
                            font.family: Theme.uiFont
                            font.pixelSize: Theme.sizeSmall
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    background: Rectangle {
                        color: row.hovered && row.enabled ? Theme.controlHover : "transparent"
                    }
                }
            }
        }
    }
}
