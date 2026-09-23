// The span: the instantaneous spectrum, the frequency ruler, and the
// waterfall, in that order and sharing one horizontal mapping.
//
// THE SCALE IS HERE BECAUSE IT LABELS THESE TWO AND NOTHING ELSE. It was the
// last row of the window's column, so whenever the receiver's pane, the RDS
// pane or the audio pane was showing it sat under those instead, several
// rows away from the columns it names. Its own comment said it was aligned
// with the two items above it; for as long as a receiver was open, the two
// items above it were the audio counters. It moved under the waterfall first,
// and then, on 2026-09-22, between the two displays as a ruler.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

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

    // Flush, so the ruler touches both of the displays it labels.
    spacing: 0

    // ------------------------------------------------------------------
    // The instantaneous spectrum
    // ------------------------------------------------------------------
    Item {
        Layout.fillWidth: true
        // A third of the span, now that the span is the window rather than
        // whatever a column of rows left of it.
        Layout.preferredHeight: Math.round(span.height * 0.34)

        SpectrumItem {
            id: spectrum
            anchors.fill: parent
            link: engineLink
            mapPins: ScaleSettings
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
        //
        // Each end carries its pin beside it, since 2026-09-22. A pin holds
        // that end at the level drawn when it went in, on both displays,
        // until it is taken out; see PinPlate.qml.
        PinPlate {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 4
            anchors.topMargin: 28
            visible: span.drawing
            text: spectrum.drawCeilingDb.toFixed(1) + " dBFS ceiling"
            pinned: ScaleSettings.ceilingPinned
            onPinRequested: ScaleSettings.pinCeiling(spectrum.drawCeilingDb)
            onUnpinRequested: ScaleSettings.unpinCeiling()
            onNudged: (steps) => ScaleSettings.nudgeCeiling(steps)
        }

        PinPlate {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 4
            visible: span.drawing
            text: spectrum.drawFloorDb.toFixed(1) + " dBFS floor"
            pinned: ScaleSettings.floorPinned
            onPinRequested: ScaleSettings.pinFloor(spectrum.drawFloorDb)
            onUnpinRequested: ScaleSettings.unpinFloor()
            onNudged: (steps) => ScaleSettings.nudgeFloor(steps)
        }

        Plate {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 4
            visible: span.drawing && spectrum.headroomDb > 0.05
            text: "floor +" + spectrum.headroomDb.toFixed(1) + " dB for the column peak"
        }
    }

    // The ruler, between the two displays it labels. See Ruler.qml.
    Ruler {
        Layout.fillWidth: true
        Layout.preferredHeight: implicitHeight
        onPicked: (hz) => span.selection.takeTune(0, hz, 0.0, 0, 0, false)
    }

    // ------------------------------------------------------------------
    // The waterfall
    // ------------------------------------------------------------------
    WaterfallItem {
        id: waterfall
        Layout.fillWidth: true
        Layout.fillHeight: true
        link: engineLink
        mapPins: ScaleSettings
        selectedDetection: span.selection.selectedDetection
        onTuneRequested: (id, centerHz, bandwidthHz, candidates, rank, exhausted) =>
                         span.selection.takeTune(id, centerHz, bandwidthHz, candidates,
                                                 rank, exhausted)
    }
}
