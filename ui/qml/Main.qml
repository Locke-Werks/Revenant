// The window. Layout and binding only.
//
// Every number shown here is read from a property on EngineLink or on one of
// the two render items. Nothing is computed in this file: the reduction
// correction in particular belongs to the item that applied it, because it
// depends on that item's width, and a copy of the arithmetic here would be a
// second answer to the same question.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ApplicationWindow {
    id: window

    width: 1280
    height: 800
    visible: true
    color: "#06080e"
    title: engineLink.connected
           ? "Revenant  —  " + engineLink.endpoint
           : "Revenant  —  not connected"

    readonly property color inkDim: "#6f7b8c"
    readonly property color ink: "#c6d0dd"

    // Before the first frame the items have no scale, only the defaults
    // their members were built with. Showing those would put a dBFS number
    // on screen that no measurement stands behind, which is worse than a
    // blank corner: it looks like a reading.
    readonly property bool drawing: engineLink.connected
                                    && engineLink.spectrumEnabled
                                    && engineLink.framesReceived > 0

    function mhz(hz) {
        return (hz / 1.0e6).toFixed(4)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        // ------------------------------------------------------------------
        // What the engine is
        // ------------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 18

            Label {
                text: engineLink.connected
                      ? engineLink.deviceName + "  (" + engineLink.deviceVendor + ")"
                      : "no engine at " + engineLink.endpoint
                color: engineLink.connected ? window.ink : "#d6624a"
                font.pixelSize: 13
                font.bold: true
            }

            Label {
                visible: engineLink.connected
                text: engineLink.sourceRate + " S/s source  ·  "
                      + engineLink.channelRate + " S/s channel  ·  "
                      + engineLink.gridChannels + " channels  ·  "
                      + engineLink.bins + " bins"
                color: window.inkDim
                font.pixelSize: 12
            }

            Item { Layout.fillWidth: true }

            Label {
                visible: !engineLink.connected && engineLink.errorText.length > 0
                text: engineLink.errorText
                color: "#d6624a"
                font.pixelSize: 12
                elide: Text.ElideRight
                Layout.maximumWidth: 520
            }

            Label {
                visible: engineLink.connected
                text: engineLink.framesReceived + " frames  ·  "
                      + engineLink.framesDropped + " dropped"
                color: engineLink.framesDropped > 0 ? "#d6a24a" : window.inkDim
                font.pixelSize: 12
            }
        }

        // ------------------------------------------------------------------
        // What the engine built, when it is not what was asked for
        // ------------------------------------------------------------------
        // A clamp is the ordinary case and not a fault: a ring rounded down
        // to a power of two, a grid that took half the channels asked for, a
        // dispatch capped by the channel ring. Frames keep arriving and they
        // look correct, which is why this has to be on screen at all.
        // core/engine/engine.cpp says what happens otherwise, where it packs
        // the note into the ring's field: the operator finds out when a
        // frequency lands in the wrong channel.
        //
        // So a line of text and not a dialog, and the sentence rather than
        // the flag. Which limit bound, what was asked for and what was built
        // are all in the sentence; clamped only decides whether the row is
        // here. "clamped:" is the word tools/cli/main.cpp prints for the same
        // field, so the two clients describe one engine the same way.
        //
        // The row takes as many lines as the sentence needs and then keeps
        // that height: clampReason is fixed at connect like the rest of
        // EngineInfo, so this cannot push the waterfall around mid-run.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: engineLink.connected && engineLink.clamped

            Label {
                Layout.alignment: Qt.AlignTop
                text: "clamped:"
                color: "#d6a24a"
                font.pixelSize: 12
                font.bold: true
            }

            Label {
                Layout.fillWidth: true
                // Every producer of the flag writes a reason with it, so the
                // fallback is unreachable today. It is here because a bare
                // "clamped:" with nothing after it would read as this row
                // failing rather than as the engine saying nothing.
                text: engineLink.clampReason.length > 0
                      ? engineLink.clampReason
                      : "the engine built something other than what was asked for and gave no reason"
                color: window.ink
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        // ------------------------------------------------------------------
        // The instantaneous spectrum
        // ------------------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(parent.height * 0.28)

            SpectrumItem {
                id: spectrum
                anchors.fill: parent
                link: engineLink
            }

            // The ends the trace was drawn against, which include the
            // reduction correction. Not engineLink.floorDb: that is what the
            // engine measured, and the item draws against something slightly
            // above it.
            Label {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 4
                visible: window.drawing
                text: spectrum.drawCeilingDb.toFixed(1) + " dBFS"
                color: window.inkDim
                font.pixelSize: 11
            }

            Label {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 4
                visible: window.drawing
                text: spectrum.drawFloorDb.toFixed(1) + " dBFS"
                color: window.inkDim
                font.pixelSize: 11
            }

            Label {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 4
                visible: window.drawing && spectrum.headroomDb > 0.05
                text: "peak-reduction floor +" + spectrum.headroomDb.toFixed(1) + " dB"
                color: window.inkDim
                font.pixelSize: 11
            }
        }

        // ------------------------------------------------------------------
        // The waterfall
        // ------------------------------------------------------------------
        WaterfallItem {
            id: waterfall
            Layout.fillWidth: true
            Layout.fillHeight: true
            link: engineLink
        }

        // ------------------------------------------------------------------
        // The frequency axis, three ticks, absolute hertz
        // ------------------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 18
            visible: engineLink.spectrumEnabled

            Label {
                anchors.left: parent.left
                text: window.mhz(engineLink.spanLowHz) + " MHz"
                color: window.inkDim
                font.pixelSize: 11
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: window.mhz((engineLink.spanLowHz + engineLink.spanHighHz) / 2)
                color: window.inkDim
                font.pixelSize: 11
            }

            Label {
                anchors.right: parent.right
                text: window.mhz(engineLink.spanHighHz) + " MHz"
                color: window.inkDim
                font.pixelSize: 11
            }
        }

        Label {
            Layout.fillWidth: true
            visible: engineLink.connected && !engineLink.spectrumEnabled
            text: "This engine was built with no spectrum stage, so there are no frames to draw."
            color: "#d6a24a"
            font.pixelSize: 12
        }
    }
}
