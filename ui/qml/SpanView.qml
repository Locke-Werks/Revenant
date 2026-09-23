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

    // The ends as drawn, which is where the pin keys pin them, the same as
    // the pin buttons beside them.
    readonly property double drawFloorDb: spectrum.drawFloorDb
    readonly property double drawCeilingDb: spectrum.drawCeilingDb

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
            onTuneRequested: (id, centerHz, bandwidthHz, candidates, rank, exhausted, pointerHz) =>
                             span.selection.takeTune(id, centerHz, bandwidthHz, candidates,
                                                     rank, exhausted, pointerHz)
            onAddRequested: (id, centerHz, bandwidthHz, pointerHz) =>
                            span.selection.takeAdd(id, centerHz, bandwidthHz, pointerHz)
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

        // THE TRACK UNDER THE POINTER, in full, on either display.
        //
        // A span label says the frequency and the SNR and nothing else since
        // 2026-09-22. The three numbers that used to crowd it, or were never
        // on it, are here, each named for what it measures: how long the
        // track has held (the stopwatch the detections bar filters on, which
        // the label used to print bare as a confidence), how far it stood above
        // the threshold, and how much of it is in three bins. Only while the
        // pointer is on a box, so the card is never over a picture nobody asked
        // about.
        Rectangle {
            id: hoverCard

            readonly property var trackId: spectrum.hoveredDetection !== 0
                                      ? spectrum.hoveredDetection
                                      : waterfall.hoveredDetection
            readonly property double held: trackId !== 0 ? engineLink.detectionConfidence(trackId) : -1
            readonly property double margin: trackId !== 0 ? engineLink.detectionMargin(trackId) : -1
            readonly property double concentration:
                trackId !== 0 ? engineLink.detectionConcentration(trackId) : -1

            // What the engine says the signal is, in full: the bracket has
            // room for the name only. models/label_tune.h writes the line.
            readonly property string label: trackId !== 0 ? engineLink.detectionLabelText(trackId) : ""

            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: 6
            anchors.topMargin: 28
            visible: trackId !== 0 && held >= 0
            width: hoverText.implicitWidth + 16
            height: hoverText.implicitHeight + 10
            radius: Theme.radius
            color: Theme.panel
            border.width: 1
            border.color: Theme.border

            Text {
                id: hoverText
                anchors.centerIn: parent
                color: Theme.ink
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
                text: (hoverCard.label !== "" ? hoverCard.label + "\n" : "")
                      + "track " + hoverCard.trackId
                      + "  ·  held for " + hoverCard.held.toFixed(2)
                      + (hoverCard.margin >= 0 ? "  ·  margin " + hoverCard.margin.toFixed(2) : "")
                      + (hoverCard.concentration >= 0
                         ? "  ·  " + hoverCard.concentration.toFixed(2) + " of it in 3 bins"
                         : "")
            }
        }
    }

    // The ruler, between the two displays it labels. See Ruler.qml.
    Ruler {
        Layout.fillWidth: true
        Layout.preferredHeight: implicitHeight
        onPicked: (hz) => span.selection.takeTune(0, hz, 0.0, 0, 0, false, hz)
        onAdded: (hz) => span.selection.takeAdd(0, hz, 0.0, hz)
    }

    // ------------------------------------------------------------------
    // The waterfall
    // ------------------------------------------------------------------
    WaterfallItem {
        id: waterfall
        Layout.fillWidth: true
        Layout.fillHeight: true

        // WHERE THE HISTORY ENDS, while it is still filling. Rows arrive at
        // the top and push the history down, so for the first minute after a
        // start, a reconnect or a resize the bottom of the waterfall has never
        // been written and is the background. Seen on 2026-09-22 as the
        // waterfall stopping three quarters of the way down; it had not
        // stopped, it had not got there yet. A line and a note at the edge
        // say which.
        Rectangle {
            visible: waterfall.historyFraction > 0 && waterfall.historyFraction < 1
            y: Math.round(waterfall.height * waterfall.historyFraction)
            width: parent.width
            height: 1
            color: Theme.border

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 6
                text: "history fills in from the top as frames arrive"
                color: Theme.inkOff
                font.family: Theme.uiFont
                font.pixelSize: Theme.sizeSmall
            }
        }
        link: engineLink
        mapPins: ScaleSettings
        selectedDetection: span.selection.selectedDetection
        onTuneRequested: (id, centerHz, bandwidthHz, candidates, rank, exhausted, pointerHz) =>
                         span.selection.takeTune(id, centerHz, bandwidthHz, candidates,
                                                 rank, exhausted, pointerHz)
        onAddRequested: (id, centerHz, bandwidthHz, pointerHz) =>
                        span.selection.takeAdd(id, centerHz, bandwidthHz, pointerHz)
    }
}
