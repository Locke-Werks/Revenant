// The engine's rolling capture, browsed: what it caught over a window, on the
// span on screen or a typed band, newest first, and a way to hear one again.
//
// THE DEFAULTS ARE THE QUESTION. "What did I just miss": the last hour, this
// span, any length. The window, band and minimum length are there for the
// operator who wants a different one, and each change searches again. The
// strip over the list is the same window's occupancy, oldest on the left,
// so a burst can be seen before it is found in the rows.
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

    // Labels from models/capture_browser.h's kCaptureWindows and
    // kCaptureMinDurations; EngineLink turns them into nanoseconds.
    property string windowLabel: "1 h"
    property string minDuration: "any"

    // The decoders whose identifiers the engine writes into the capture.
    // "any" is no filter; a named one keeps only that decoder's hits.
    readonly property var decoders:
        ["any", "p25p1", "dmr", "ais", "ax25", "m17", "pocsag", "dstar", "tetra"]
    property string decoder: "any"

    // Called when the popover opens, so the list is never older than the
    // last time the operator looked.
    function opened() {
        panel.search()
    }

    function search() {
        engineLink.searchCapture(panel.windowLabel, bandField.text, panel.minDuration,
                                 panel.decoder, decodedField.text)
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
                  : engineLink.captureHits.length
                    + (engineLink.captureHits.length === 1 ? " hit" : " hits")
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        RButton {
            flat: true
            text: "refresh"
            ink: Theme.inkDim
            enabled: !engineLink.captureSearchBusy
            onClicked: panel.search()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        RSegmented {
            options: ["15 min", "1 h", "6 h", "24 h"]
            current: panel.windowLabel
            onPicked: (option) => {
                panel.windowLabel = option
                panel.search()
            }
        }

        // Empty is the span being captured, which is what the placeholder
        // says; a typed band is read on return.
        RTextField {
            id: bandField
            Layout.preferredWidth: 220
            mono: true
            placeholderText: "this span, or 162.4M:162.6M"
            onAccepted: panel.search()
        }

        Label {
            text: "at least"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        RSegmented {
            options: ["any", "1 s", "5 s", "30 s"]
            current: panel.minDuration
            onPicked: (option) => {
                panel.minDuration = option
                panel.search()
            }
        }
    }

    // Who was heard rather than what: a decoder, and identifiers it decoded.
    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            text: "decoded by"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        RComboBox {
            id: decoderChoice

            Layout.preferredWidth: 110
            model: panel.decoders
            currentIndex: panel.decoders.indexOf(panel.decoder)
            onActivated: (index) => {
                panel.decoder = panel.decoders[index]
                panel.search()
            }
        }

        // Read on return, like the band. Each term is key=value and every
        // one must match, so the Tip says the syntax rather than the
        // placeholder carrying all of it.
        RTextField {
            id: decodedField

            Layout.fillWidth: true
            Layout.maximumWidth: 320
            mono: true
            placeholderText: "anyone, or mmsi=366123456"
            onAccepted: panel.search()

            HoverHandler { id: decodedHover }

            Tip {
                visible: decodedHover.hovered && !decodedField.activeFocus
                delay: 400
                text: "key=value terms separated by spaces, all of which must match, "
                      + "for example mmsi=366123456 talkgroup=101. "
                      + "Keys are the decoder's own: talkgroup, source, mmsi, callsign."
            }
        }
    }

    // Occupancy across the window, oldest left: brighter where more of the
    // column held a signal, and a dimmer shade where nothing was recorded at
    // all, because a gap in the capture is not the same news as a quiet band.
    Row {
        id: strip

        visible: engineLink.captureOccupancy.length > 0
        Layout.preferredWidth: 640
        Layout.preferredHeight: 10

        Repeater {
            model: engineLink.captureOccupancy

            Rectangle {
                required property var modelData

                width: strip.width / Math.max(1, engineLink.captureOccupancy.length)
                height: strip.height
                color: modelData.captured < 0.5 ? Theme.control
                       : Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b,
                                 0.12 + 0.88 * Math.min(1, modelData.occupied))
            }
        }
    }

    // While a replay is open, said here as well as on the recording strip,
    // because this is where the operator just was when it changed.
    RowLayout {
        visible: panel.replaying
        Layout.fillWidth: true
        spacing: 10

        // A replay plays its hit and stops. Said here, where the operator is,
        // or the waterfall just freezes on its last row.
        Label {
            text: engineLink.engineRunning ? "replaying" : "replay ended"
            color: engineLink.engineRunning ? Theme.accent : Theme.inkDim
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
            visible: !engineLink.engineRunning
            text: "replay again"
            onClicked: engineLink.openSource(String(engineLink.openedSource.uri || ""))
        }

        RButton {
            text: "back to the radio"
            iconName: "x"
            label: "back to radio"
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
        text: "nothing caught in this window"
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

                // Each column grows to its text: the fixed widths were measured
                // at 100% and cut the figures off at 150% scaling.
                Label {
                    Layout.preferredWidth: Math.max(64, implicitWidth)
                    text: row.modelData.time
                    color: Theme.ink
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }
                Label {
                    Layout.preferredWidth: Math.max(48, implicitWidth)
                    text: row.modelData.duration
                    horizontalAlignment: Text.AlignRight
                    color: Theme.inkDim
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }
                Label {
                    Layout.preferredWidth: Math.max(200, implicitWidth)
                    text: row.modelData.frequency
                    color: Theme.ink
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }
                Label {
                    Layout.preferredWidth: row.modelData.decoded !== "" ? 56 : -1
                    Layout.fillWidth: row.modelData.decoded === ""
                    text: row.modelData.label
                    elide: Text.ElideRight
                    color: row.modelData.label === "signal" ? Theme.inkDim : Theme.ink
                    font.pixelSize: Theme.sizeSmall
                }
                // A decoder's hit says who it was, the reason it was found.
                Label {
                    visible: row.modelData.decoded !== ""
                    Layout.fillWidth: true
                    text: row.modelData.decoded
                    elide: Text.ElideRight
                    color: Theme.ink
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }
                Label {
                    Layout.preferredWidth: Math.max(40, implicitWidth)
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
