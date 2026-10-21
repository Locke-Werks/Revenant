// Band-plan and live labels over the span spectrum, stacked up from its
// bottom edge with a leader tick down to each label's exact frequency.
//
// The bottom and not the top because the detection plates are painted across
// the top strip by render/spectrum_item.cpp, and a QML plate over one would
// cover it. models/live_labels.h says which labels exist and
// render/label_layout.h where they go; this only draws them.
//
// A click tunes a receiver there in the label's mode.

import QtQuick
import QtQuick.Controls
import Revenant

Item {
    id: root

    property bool shown: SpanLabelSettings.showLabels

    readonly property int rowHeight: 16
    readonly property int tickHeight: 6

    SpanLabelModel {
        id: labels
        link: engineLink
        viewWidth: root.width
        enabled: root.shown
        categories: SpanLabelSettings.categories
        region: BandPlanSettings.region
        font.family: Theme.monoFont
        font.pixelSize: Theme.sizeSmall
        padding: 4
    }

    Repeater {
        model: labels

        delegate: Item {
            id: entry
            required property int index
            required property string text
            required property double x0
            required property double plateWidth
            required property int row
            required property double anchorX
            required property double labelOpacity
            required property bool live
            required property double hz
            required property string mode

            anchors.fill: parent
            opacity: entry.labelOpacity

            // The tick runs from the plate's foot to the bottom edge, so the
            // frequency it marks is exact even when the plate was slid.
            Rectangle {
                x: Math.round(entry.anchorX)
                width: 1
                y: plate.y + plate.height
                height: root.height - y
                color: entry.live ? Theme.inkTune : Theme.inkDim
            }

            Rectangle {
                id: plate
                x: entry.x0
                width: entry.plateWidth
                height: root.rowHeight - 2
                y: root.height - root.tickHeight - (entry.row + 1) * root.rowHeight
                radius: 2
                color: Theme.panelSolid
                border.color: entry.live ? Theme.inkTune : Theme.border
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: entry.text
                    color: entry.live ? Theme.ink : Theme.inkDim
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }

                MouseArea {
                    id: hit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: labels.activate(entry.index)
                }

                ToolTip {
                    visible: hit.containsMouse
                    delay: 400
                    text: entry.text + "  " + (entry.hz / 1e6).toFixed(4) + " MHz"
                          + (entry.mode !== "" ? "  " + entry.mode : "")
                          + (entry.live ? "  (heard now)" : "  (band plan)")
                }
            }
        }
    }
}
