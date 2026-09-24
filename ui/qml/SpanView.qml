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
            scaleLabelHeight: scaleMetrics.height
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
            id: ceilingPin
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

            // How far above the engine's floor this one is drawn, and why, on
            // hover. It was a plate of its own in the corner opposite,
            // "floor +16.8 dB for the column peak", always on screen and read
            // by nobody who did not already know what it meant.
            detail: spectrum.headroomDb > 0.05
                    ? "Drawn " + spectrum.headroomDb.toFixed(1) + " dB above the engine's "
                      + "floor, its fifth percentile. Each column of the display shows the "
                      + "loudest of the bins under it, and on an empty band the loudest of "
                      + "those sits that far above the fifth percentile of one, so the "
                      + "floor is raised to match."
                    : ""
        }

        // THE LEVEL SCALE, up the right edge, since 2026-09-23. The ticks and
        // their spacing are models/level_scale.h, planned by the item from
        // the ends it draws against, so the scale follows the auto-scale and
        // the pins alike; the gridlines and edge marks are drawn by the item
        // under the trace, and only the numbers are here. Outlined in the
        // background colour rather than set on plates, so they read over the
        // trace without hiding a strip of it.
        FontMetrics {
            id: scaleMetrics
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeSmall
        }

        // How far in from the right edge the scale's numbers reach, which the
        // peak's plate keeps out of: four characters, "-120", and the ticks.
        readonly property double scaleGutter: scaleMetrics.averageCharacterWidth * 4 + 12

        Repeater {
            model: span.drawing ? spectrum.scaleLabelCount : 0

            Text {
                required property int index
                readonly property var entry: spectrum.scaleLabels[index] || ({})

                // A number under the noise floor's plate would be read as
                // part of it.
                visible: !noisePlate.covers(y, height)
                x: parent.width - 9 - implicitWidth
                y: (entry.y || 0) - height / 2
                height: scaleMetrics.height
                verticalAlignment: Text.AlignVCenter
                text: entry.text || ""
                color: Theme.inkDim
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
                style: Text.Outline
                styleColor: Theme.background
            }
        }

        // THE NOISE FLOOR, as this window measures it. The engine publishes
        // no floor of its own; the detector keeps one and it does not cross
        // the wire. models/span_markers.h has why the estimate is a low
        // percentile of the trace and not the auto-scale floor on the plate
        // at the left. The line is the item's, dashed under the trace; the
        // plate goes under the noise so it covers the fill and not the trace.
        Plate {
            id: noisePlate

            readonly property var at: spectrum.noisePlate(spectrum.noiseY, spectrum.noiseLowY,
                                                          implicitWidth, implicitHeight,
                                                          parent.width, parent.height)

            function covers(top, tall) {
                return visible && top < y + height && y < top + tall
            }

            // Under the bottom of the scale, which is where the auto-scale
            // puts the noise on a flat band: its floor is lifted to where a
            // noise-only column lands, so half the noise is drawn at the
            // bottom edge. The line is not drawn there, and the arrow says
            // which way the floor is.
            readonly property bool below: spectrum.noiseY >= parent.height

            visible: span.drawing && spectrum.noiseValid
            x: at.x
            y: at.y
            text: (below ? "▼ " : "") + "noise " + spectrum.noiseDb.toFixed(1) + " dBFS est."

            HoverHandler { id: noiseHover }

            Tip {
                visible: noiseHover.hovered
                text: "The noise floor as this window estimates it from the trace: a quarter "
                      + "of the trace's columns sit below the dashed line. The engine publishes "
                      + "no noise floor; its detector keeps one and it stays inside the engine. "
                      + "This is not the floor plate at the left, which is where the colour "
                      + "map starts rather than where the noise is."
            }
        }

        // THE STRONGEST SIGNAL across the whole span the source delivers,
        // held on one signal until another is 3 dB louder and eased over a
        // quarter second, so it can be read; models/span_markers.h. A tick
        // points down at it and the plate sits over the tick, where the pane
        // is empty because nothing else is as tall. Above the top of the
        // scale it cannot be pointed at, and the plate goes beside it,
        // under the detection labels, with an arrow saying which way it is.
        Item {
            id: peakMarker

            readonly property bool above: spectrum.peakY < 0
            readonly property var at: spectrum.peakPlate(spectrum.peakX, spectrum.peakY,
                                                         peakPlate.width, peakPlate.height,
                                                         parent.scaleGutter, width, height,
                                                         Qt.rect(ceilingPin.x, ceilingPin.y,
                                                                 ceilingPin.width,
                                                                 ceilingPin.height))

            anchors.fill: parent
            visible: span.drawing && spectrum.peakValid

            Rectangle {
                visible: peakMarker.at.tick
                x: Math.round(peakMarker.at.tickX)
                y: peakMarker.at.tickTop
                width: 1
                height: peakMarker.at.tickBottom - peakMarker.at.tickTop
                color: Theme.ink
            }

            Rectangle {
                id: peakPlate

                x: peakMarker.at.x
                y: peakMarker.at.y
                width: peakText.implicitWidth + 8
                height: peakText.implicitHeight + 4
                radius: 2
                color: Theme.plate

                Text {
                    id: peakText
                    anchors.centerIn: parent
                    color: Theme.ink
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                    text: (peakMarker.above ? "▲ " : "") + "peak "
                          + spectrum.peakDb.toFixed(1) + " dBFS  "
                          + (spectrum.peakHz / 1.0e6).toFixed(4) + " MHz"
                }

                HoverHandler { id: peakHover }

                Tip {
                    visible: peakHover.hovered
                    text: "The strongest signal across the whole span the source delivers, "
                          + "at its loudest bin. The marker stays on one signal until another "
                          + "is 3 dB louder, and its figures are eased over a quarter second."
                          + (peakMarker.above ? " It is above the top of the scale." : "")
                }
            }
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
                text: "filling from the top"
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
