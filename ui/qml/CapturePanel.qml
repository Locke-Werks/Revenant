// The engine's rolling capture, browsed: what it caught in the last hour over
// the span on screen, newest first, and a way to hear one again.
//
// A LIST AND NOTHING ELSE. No filters, no timeline, no occupancy chart: the
// question it answers is "what did I just miss", and the defaults in
// models/capture_browser.h are that question. More controls wait for an
// operator who needs them.
//
// OPENING A ROW IS THE PICKER'S PATH. Its uri goes through engineLink.
// openSource, which closes the radio and opens the replay, so the recording
// strip under the bar names it the way it names any recording, and the way
// back is the radio picker this panel hands over to rather than a second
// switch of its own.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: panel

    // Set by TopBar: closes this panel and opens the radio picker.
    property var backToRadio: null

    readonly property int rowHeight: 26
    readonly property bool replaying:
        String(engineLink.openedSource.uri || "").startsWith("capture:")

    // Called when the popover opens, so the list is never older than the
    // last time the operator looked.
    function opened() {
        engineLink.searchCapture()
    }

    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        spacing: 10

        Label {
            text: "capture"
            color: Theme.ink
            font.pixelSize: Theme.sizeTitle
            font.bold: true
        }

        Label {
            Layout.fillWidth: true
            text: engineLink.captureSearchBusy ? "searching"
                  : "last hour, this span, " + engineLink.captureHits.length
                    + (engineLink.captureHits.length === 1 ? " hit" : " hits")
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        RButton {
            flat: true
            text: "refresh"
            ink: Theme.inkDim
            enabled: !engineLink.captureSearchBusy
            onClicked: engineLink.searchCapture()
        }
    }

    // While a replay is open, said here as well as on the recording strip,
    // because this is where the operator just was when it changed.
    RowLayout {
        visible: panel.replaying
        Layout.fillWidth: true
        spacing: 10

        Label {
            text: "replaying"
            color: Theme.accent
            font.pixelSize: Theme.sizeSmall
            font.bold: true
        }

        Label {
            Layout.fillWidth: true
            text: engineLink.openedSource.displayName || ""
            elide: Text.ElideMiddle
            color: Theme.ink
            font.pixelSize: Theme.sizeSmall
        }

        RButton {
            text: "back to the radio"
            onClicked: if (panel.backToRadio) panel.backToRadio()
        }
    }

    Label {
        visible: engineLink.captureSearchFault !== ""
        Layout.fillWidth: true
        Layout.maximumWidth: 640
        text: engineLink.captureSearchFault
        wrapMode: Text.Wrap
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeSmall
    }

    Label {
        visible: !engineLink.captureSearchBusy && engineLink.captureSearchFault === ""
                 && engineLink.captureHits.length === 0
        text: "nothing caught in the last hour on this span"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
    }

    ListView {
        id: list

        visible: engineLink.captureHits.length > 0
        Layout.preferredWidth: 640
        Layout.preferredHeight: panel.rowHeight * Math.min(12, Math.max(1, count))
        clip: true
        model: engineLink.captureHits
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: RScrollBar {}

        delegate: Rectangle {
            id: row

            required property var modelData

            width: ListView.view.width
            height: panel.rowHeight
            radius: Theme.radius
            color: rowHover.hovered ? Theme.control : "transparent"

            HoverHandler { id: rowHover }

            TapHandler {
                onDoubleTapped: engineLink.openSource(row.modelData.uri)
            }

            Tip {
                visible: rowHover.hovered
                text: "double-click to replay"
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 12

                Label {
                    Layout.preferredWidth: 64
                    text: row.modelData.time
                    color: Theme.ink
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }
                Label {
                    Layout.preferredWidth: 48
                    text: row.modelData.duration
                    horizontalAlignment: Text.AlignRight
                    color: Theme.inkDim
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }
                Label {
                    Layout.preferredWidth: 200
                    text: row.modelData.frequency
                    color: Theme.ink
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }
                Label {
                    Layout.fillWidth: true
                    text: row.modelData.label
                    elide: Text.ElideRight
                    color: row.modelData.label === "signal" ? Theme.inkDim : Theme.ink
                    font.pixelSize: Theme.sizeSmall
                }
                Label {
                    Layout.preferredWidth: 40
                    text: row.modelData.confidence
                    horizontalAlignment: Text.AlignRight
                    color: Theme.inkDim
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }
            }
        }
    }
}
