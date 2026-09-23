// The receiver rack: one compact strip per receiver, like the channel strips
// of a mixing console. Each strip carries its receiver's colour, which is the
// colour of its marker on the span and of its band on the ruler.
//
// Up to eight, one per colour. The focused one is the receiver the rest of
// this window is about; a click on another strip focuses that one. The rules
// are models/receiver_rack.h and the wire half is ui/models/rack_link.cpp.
//
// WHAT THIS USED TO SAY: "ONE STRIP TODAY", with solo "drawn and not offered"
// because the client held one receiver per window. It holds a rack now, and
// solo and mute both act.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: rack

    // An entry with every field a strip reads, for a strip with no receiver.
    readonly property var blank: ({
        "key": 0, "slot": 0, "colour": "#000000", "label": "", "mode": "",
        "frequencyHz": 0, "lowHz": 0, "highHz": 0, "levelDbfs": -200,
        "focused": false, "pending": true, "refused": false, "refusal": "",
        "muted": false, "solo": false,
        "heard": false, "gain": 0, "gainText": ""
    })

    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        Label {
            text: "receivers"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            font.bold: true
        }

        Label {
            visible: engineLink.rackCount > 0
            text: engineLink.rackCount + " of 8"
            color: Theme.inkOff
            font.pixelSize: Theme.sizeSmall
        }

        Item { Layout.fillWidth: true }

        // The key table's "put a new receiver on the span centre", for a
        // mouse. The span's double click is the other way to add one.
        RButton {
            flat: true
            text: "+ add"
            ink: Theme.inkDim
            enabled: engineLink.sourceOpen && !engineLink.rackFull
            onClicked: engineLink.addReceiver((engineLink.spanLowHz + engineLink.spanHighHz) / 2, "")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: engineLink.rackFull ? "the rack holds eight"
                                              : "a new receiver on the span centre"
        }
    }

    // No receiver. Said where the strips would be, with the way to make one,
    // and that none are kept across a restart.
    Label {
        Layout.fillWidth: true
        visible: engineLink.rackCount === 0
        text: UiRules.rackEmptyText()
        color: Theme.inkDim
        font.pixelSize: Theme.sizeBody
        wrapMode: Text.WordWrap
    }

    // What the rack has to say that no strip carries: a receiver the engine
    // let go, an add the engine refused, a double click on a full rack.
    Label {
        Layout.fillWidth: true
        visible: text.length > 0
        text: engineLink.receiverGoneText.length > 0 ? engineLink.receiverGoneText
                                                    : engineLink.rackNote
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
    }

    // The strips, scrolled when eight do not fit the window's height.
    Flickable {
        Layout.fillWidth: true
        Layout.fillHeight: true
        contentHeight: strips.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: strips
            width: parent.width
            spacing: 6

            // By count and not by the list, so a strip is not rebuilt each
            // time a level arrives, which is several times a second and would
            // drop a gain slider out from under the pointer mid-drag.
            Repeater {
                model: engineLink.rackCount

                ReceiverStrip {
                    required property int index
                    Layout.fillWidth: true

                    // The blank covers the one turn of the event loop between
                    // a strip going and the Repeater dropping its delegate.
                    entry: engineLink.rackEntries[index] || rack.blank
                }
            }
        }
    }

    // Which click does what, where the operator reads about receivers.
    Label {
        Layout.fillWidth: true
        visible: engineLink.rackCount > 0
        text: UiRules.spanClickHint()
        color: Theme.inkOff
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
    }
}
