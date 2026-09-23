// The span: the instantaneous spectrum, the waterfall under it, and the
// frequency axis under both.
//
// THE AXIS IS HERE BECAUSE IT LABELS THESE TWO AND NOTHING ELSE. It was the
// last row of the window's column, so whenever the receiver's pane, the RDS
// pane or the audio pane was showing it sat under those instead, several
// rows away from the columns it names. Its own comment said it was aligned
// with the two items above it; for as long as a receiver was open, the two
// items above it were the audio counters. Placed here it is under the
// waterfall whatever else the window is showing.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: span

    // The click readout's state, which the two displays write and read
    // back. See TuneSelection.qml.
    required property TuneSelection selection

    // Before the first frame the items have no scale, only the defaults
    // their members were built with. Showing those would put a dBFS number
    // on screen that no measurement stands behind, which is worse than a
    // blank corner: it looks like a reading.
    readonly property bool drawing: engineLink.connected
                                    && engineLink.spectrumEnabled
                                    && engineLink.framesReceived > 0

    Layout.fillWidth: true
    Layout.fillHeight: true
    spacing: 6

    // ------------------------------------------------------------------
    // The instantaneous spectrum
    // ------------------------------------------------------------------
    Item {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.round(Window.height * 0.24)

        SpectrumItem {
            id: spectrum
            anchors.fill: parent
            link: engineLink
            selectedDetection: span.selection.selectedDetection
            onTuneRequested: (id, centerHz, bandwidthHz, candidates, rank, exhausted) =>
                             span.selection.takeTune(id, centerHz, bandwidthHz, candidates,
                                                     rank, exhausted)
        }

        // The ends the trace was drawn against, which include the
        // reduction correction. Not engineLink.floorDb: that is what the
        // engine measured, and the item draws against something slightly
        // above it.
        //
        // Each sits on a plate, because these labels are drawn over the
        // trace and the trace goes white at the top of the map. Read
        // against a saturated carrier they were illegible, which is
        // exactly where somebody looks for the ceiling.
        //
        // The ceiling stays above the floor, because a map's two ends
        // read by position as much as by the number. It is pushed clear
        // of the top edge rather than moved away from it: the detection
        // labels are painted in that strip, and a QML Rectangle drawn
        // over the item would cover one.
        Plate {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 4
            anchors.topMargin: 28
            visible: span.drawing
            text: spectrum.drawCeilingDb.toFixed(1) + " dBFS ceiling"
        }

        Plate {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 4
            visible: span.drawing
            text: spectrum.drawFloorDb.toFixed(1) + " dBFS floor"
        }

        Plate {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 4
            visible: span.drawing && spectrum.headroomDb > 0.05
            text: "floor +" + spectrum.headroomDb.toFixed(1) + " dB for the column peak"
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
        selectedDetection: span.selection.selectedDetection
        onTuneRequested: (id, centerHz, bandwidthHz, candidates, rank, exhausted) =>
                         span.selection.takeTune(id, centerHz, bandwidthHz, candidates,
                                                 rank, exhausted)
    }

    FrequencyAxis {}
}
