// The window. Layout and binding only.
//
// Every number shown here is read from a property on EngineLink or on one of
// the two render items. Nothing is computed in this file: the reduction
// correction in particular belongs to the item that applied it, because it
// depends on that item's width, and a copy of the arithmetic here would be a
// second answer to the same question.
//
// The one call that looks like an exception is not one. The axis asks
// EngineLink for the frequency at a fraction of the span, and that is a
// method rather than a property because only a display knows where its ticks
// are. The arithmetic behind it is still in one place, on the far side of
// that call, working from the rationals the wire carries.

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
           ? "Revenant  ·  " + engineLink.endpoint
           : "Revenant  ·  waiting for " + engineLink.endpoint

    readonly property color inkDim: "#6f7b8c"
    readonly property color ink: "#c6d0dd"
    readonly property color inkWarn: "#d6a24a"
    readonly property color inkBad: "#d6624a"

    // Before the first frame the items have no scale, only the defaults
    // their members were built with. Showing those would put a dBFS number
    // on screen that no measurement stands behind, which is worse than a
    // blank corner: it looks like a reading.
    readonly property bool drawing: engineLink.connected
                                    && engineLink.spectrumEnabled
                                    && engineLink.framesReceived > 0

    // Absolute megahertz at a fraction of the drawn span, as a tick label.
    //
    // engineLink.connected and engineLink.bins are read here so that a
    // binding calling this follows the link: frequencyAtFraction is a
    // method, and QML captures the properties a binding touches while it
    // evaluates, which cannot see inside a C++ call. Without one of them in
    // the expression the axis would keep the previous engine's numbers
    // after a reconnect, which is the one failure an axis must not have.
    function tickMhz(fraction) {
        if (!engineLink.connected || engineLink.bins <= 0)
            return ""
        return (engineLink.frequencyAtFraction(fraction) / 1.0e6).toFixed(4)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        // ------------------------------------------------------------------
        // What the engine is
        // ------------------------------------------------------------------
        // Every label in this row sets Layout.minimumWidth and elides.
        // Without that a RowLayout's minimum width is the sum of what its
        // children want, the ColumnLayout inherits it, and a window narrower
        // than that gets a layout laid out at its minimum instead: the
        // waterfall is then drawn wider than the window and its right-hand
        // end is off-screen, while the axis underneath still claims the
        // whole span. Part of the band silently missing while the labels say
        // otherwise is the one failure a spectrum display must not have, and
        // it turned up at 640 pixels wide.
        RowLayout {
            Layout.fillWidth: true
            spacing: 18

            Label {
                Layout.minimumWidth: 0
                text: engineLink.connected
                      ? engineLink.deviceName + "  (" + engineLink.deviceVendor + ")"
                      : "waiting for an engine at " + engineLink.endpoint
                color: engineLink.connected ? window.ink : window.inkWarn
                font.pixelSize: 13
                font.bold: true
                elide: Text.ElideRight
            }

            Label {
                Layout.minimumWidth: 0
                visible: engineLink.connected
                text: engineLink.sourceRate + " S/s source  ·  "
                      + engineLink.channelRate + " S/s channel  ·  "
                      + engineLink.gridChannels + " channels  ·  "
                      + engineLink.bins + " bins"
                color: window.inkDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            Item { Layout.fillWidth: true }

            // Rows a second the display actually drew. Not the rate frames
            // arrive at: on a window this size those differ by a third, and
            // the arrival rate would say the picture was moving faster than
            // it is.
            Label {
                Layout.minimumWidth: 0
                visible: engineLink.connected
                text: engineLink.frameRate.toFixed(1) + " rows/s"
                color: window.inkDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            // The two losses named apart, because they have different
            // fixes and the sum on its own points at neither. "not drawn" is
            // this client replacing a frame in the hand-off slot before the
            // GUI thread came for it, which means the GUI thread is the
            // limit; "engine dropped" means the engine had a frame at the
            // rate asked for and threw it away because this client had not
            // answered for the previous one.
            Label {
                Layout.minimumWidth: 0
                visible: engineLink.connected
                text: engineLink.framesReceived + " frames  ·  "
                      + engineLink.framesDroppedByEngine + " engine dropped  ·  "
                      + engineLink.framesDroppedByUi + " not drawn"
                color: engineLink.framesDropped > 0 ? window.inkWarn : window.inkDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        // ------------------------------------------------------------------
        // Why there is no engine
        // ------------------------------------------------------------------
        // The supervisor keeps trying for as long as the window is open, so
        // this is a state and not a failure: start the engine second, or
        // stop one and start another, and this row is what is on screen in
        // between. It carries the last reason rather than a spinner, because
        // "connection refused" and "the connection to the engine is gone"
        // are different situations and only one of them means an engine was
        // ever there.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: !engineLink.connected && engineLink.errorText.length > 0

            Label {
                Layout.alignment: Qt.AlignTop
                text: "retrying:"
                color: window.inkBad
                font.pixelSize: 12
                font.bold: true
            }

            Label {
                Layout.fillWidth: true
                text: engineLink.errorText
                color: window.ink
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
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
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: engineLink.connected && engineLink.clamped

            Label {
                Layout.alignment: Qt.AlignTop
                text: "clamped:"
                color: window.inkWarn
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
                maximumLineCount: 2
                elide: Text.ElideRight
            }
        }

        // ------------------------------------------------------------------
        // The instantaneous spectrum
        // ------------------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(window.height * 0.24)

            SpectrumItem {
                id: spectrum
                anchors.fill: parent
                link: engineLink
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
            Plate {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 4
                visible: window.drawing
                text: spectrum.drawCeilingDb.toFixed(1) + " dBFS"
            }

            Plate {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 4
                visible: window.drawing
                text: spectrum.drawFloorDb.toFixed(1) + " dBFS"
            }

            Plate {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 4
                visible: window.drawing && spectrum.headroomDb > 0.05
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
        }

        // ------------------------------------------------------------------
        // The frequency axis, in absolute hertz
        // ------------------------------------------------------------------
        // Aligned with the two items above rather than merely near them:
        // all three fill this same width, so a fraction of this item is the
        // same fraction of the span they drew, and a tick sits over the
        // column it names.
        Item {
            id: axis

            Layout.fillWidth: true
            Layout.preferredHeight: 22
            visible: engineLink.connected && engineLink.spectrumEnabled

            // Five where there is room, which is the count
            // tools/cli/main.cpp draws, and fewer where there is not. A
            // label is seven digits and a point at 11 pixels, and the last
            // one carries " MHz" as well, so 110 apart leaves clear air
            // between them. Two labels that touch are worse than three that
            // do not: a frequency axis is read by matching a tick to a
            // column, and a smudge is not a tick.
            readonly property int tickCount:
                Math.max(2, Math.min(5, Math.floor(axis.width / 110)))

            function fractionAt(index) {
                return axis.tickCount > 1 ? index / (axis.tickCount - 1) : 0.0
            }

            Repeater {
                model: axis.tickCount

                Rectangle {
                    required property int index

                    width: 1
                    height: 4
                    y: 0
                    // The last tick belongs at the right-hand edge of the
                    // last column, which is one pixel outside the item, so
                    // it is pulled back in rather than drawn off the end.
                    x: Math.min(Math.round(axis.fractionAt(index) * axis.width),
                                axis.width - 1)
                    color: window.inkDim
                }
            }

            Repeater {
                model: axis.tickCount

                Label {
                    required property int index

                    readonly property real fraction: axis.fractionAt(index)

                    // The unit rides on the last label, the way the CLI's
                    // axis line ends in one, rather than taking a label
                    // position of its own.
                    text: window.tickMhz(fraction)
                          + (index === axis.tickCount - 1 ? " MHz" : "")
                    color: window.inkDim
                    font.pixelSize: 11
                    y: 6
                    // Centred on the tick, then pulled back inside the item
                    // rather than clipped, so the two end labels stay whole.
                    x: Math.max(0, Math.min(axis.width - width,
                                            fraction * axis.width - width / 2))
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: engineLink.connected && !engineLink.spectrumEnabled
            text: "This engine was built with no spectrum stage, so there are no frames to draw."
            color: window.inkWarn
            font.pixelSize: 12
        }
    }

    // A label with enough background behind it to be read over the trace.
    component Plate: Rectangle {
        property alias text: plateText.text

        implicitWidth: plateText.implicitWidth + 8
        implicitHeight: plateText.implicitHeight + 4
        color: "#b3060810"
        radius: 2

        Label {
            id: plateText
            anchors.centerIn: parent
            color: window.inkDim
            font.pixelSize: 11
        }
    }
}
