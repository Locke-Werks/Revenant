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
//
// THE SELECTION LIVES HERE AND NOT IN EITHER ITEM
//
// Two displays show the same detections and there is one chosen track, not
// one per display. So a click emits tuneRequested, this file decides what
// that meant, and both items read selectedDetection back as a binding. An
// item that set its own would break the binding the other one is reading,
// and the two would disagree about which box is chosen.

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

    // The detection colour, and it is the one hue render/spectrum_scale.cpp
    // never produces. Kept the same on both sides so a box, its label and
    // this readout are visibly one thing.
    readonly property color inkTune: "#ff58c8"

    // Before the first frame the items have no scale, only the defaults
    // their members were built with. Showing those would put a dBFS number
    // on screen that no measurement stands behind, which is worse than a
    // blank corner: it looks like a reading.
    readonly property bool drawing: engineLink.connected
                                    && engineLink.spectrumEnabled
                                    && engineLink.framesReceived > 0

    // What the last click resolved to. tunedId is zero for a click that
    // landed on bare spectrum, which is a frequency and not a track: track
    // ids are issued from one, per core/detect/detector.h.
    property real tunedHz: 0
    property real tunedBandwidthHz: 0
    property var tunedId: 0
    property var selectedDetection: 0

    // How many boxes the last click was inside, how far into the cycle this
    // answer was, and whether the next click in the same place advances or
    // starts over. Carried on the signal rather than read back off a
    // property, because they belong to that click: a poll a frame later
    // changes the list and would change these under a reading still on
    // screen. See ClickResult in render/spectrum_item.h.
    property int tunedCandidates: 0
    property int tunedRank: 0
    property bool tunedExhausted: false

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

    function bandwidthText(hz) {
        if (hz >= 1.0e6)
            return (hz / 1.0e6).toFixed(3) + " MHz"
        if (hz >= 1000)
            return (hz / 1000).toFixed(2) + " kHz"
        return Math.round(hz) + " Hz"
    }

    // One click, whichever display it came from.
    function takeTune(id, centerHz, bandwidthHz, candidates, rank, exhausted) {
        window.selectedDetection = id
        window.tunedId = id
        window.tunedHz = centerHz
        window.tunedBandwidthHz = bandwidthHz
        window.tunedCandidates = candidates
        window.tunedRank = rank
        window.tunedExhausted = exhausted
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

            // WHETHER THE ENGINE IS RUNNING, WHICH IS NOT WHETHER IT IS
            // REACHABLE
            //
            // An engine answers RPC calls from the moment it binds its port,
            // which core/engine/engine.cpp does before run() and leaves true
            // after the source ends. So an engine that is up with its graph
            // stopped is connected, has geometry, and draws 0.0 rows/s, and
            // without this it presents as a healthy engine on a dead band.
            // Those two need different actions and the row has to say which
            // it is.
            //
            // Paired with the rate rather than given a row of its own,
            // because the pair is the diagnosis: running with 0.0 rows/s is
            // a source that has stopped delivering, stopped with 0.0 is an
            // engine waiting to be started, and both used to look the same.
            Label {
                Layout.minimumWidth: 0
                visible: engineLink.connected
                text: engineLink.engineRunning
                      ? engineLink.frameRate.toFixed(1) + " rows/s"
                      : "engine stopped  ·  " + engineLink.frameRate.toFixed(1) + " rows/s"
                color: engineLink.engineRunning ? window.inkDim : window.inkWarn
                font.pixelSize: 12
                font.bold: !engineLink.engineRunning
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
        // Why the detector's answer has stopped changing
        // ------------------------------------------------------------------
        // The engine is there and refusing, rather than gone. EngineLink
        // tells those apart by asking the engine whether it is still
        // running rather than by parsing the message, and only puts the
        // engine's own sentence here when it got an answer, so this row and
        // the retrying row above it are mutually exclusive by construction.
        //
        // A line of text rather than a dialog, for the reason the clamped
        // row gives: the overlay keeps drawing the last list it had, which
        // looks correct, and the operator finds out when a track they can
        // see on the waterfall never gets a box.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: engineLink.connected && engineLink.detectionFault.length > 0

            Label {
                Layout.alignment: Qt.AlignTop
                text: "detector refused:"
                color: window.inkBad
                font.pixelSize: 12
                font.bold: true
            }

            Label {
                Layout.fillWidth: true
                text: engineLink.detectionFault
                color: window.ink
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
        }

        // ------------------------------------------------------------------
        // The detector's two thresholds, which are two different knobs
        // ------------------------------------------------------------------
        // docs/detection.md puts both of these on the operator and says why:
        // where a person wants them depends on the band, the antenna and
        // what they are doing, and a default that suits a quiet VHF band
        // buries an HF evening.
        //
        // They are not the same knob and the row has to make that legible.
        // The confidence bar is this window's: it is the min_confidence
        // argument to Client::detections and it only filters what comes
        // back, so moving it changes this display and nothing else. The dB
        // threshold is the engine's: it changes what the detector decides at
        // all, every client sees the result, and the last writer wins. So
        // the number beside the second slider is the value IN FORCE, read
        // back from DetectionList::detection_threshold_db, rather than the
        // one this window last asked for.
        //
        // One more difference is visible in how the two behave, and the row
        // reads the two numbers from different places because of it. The
        // confidence bar is local, so the handle is the value and the label
        // is taken from the handle. The dB threshold is a round trip and a
        // poll, so the handle is a request, the label is taken from the
        // link, and a third label appears if those two part company.
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            visible: engineLink.connected && engineLink.spectrumEnabled

            Label {
                Layout.minimumWidth: 0
                text: "detections"
                color: window.inkTune
                font.pixelSize: 12
                font.bold: true
                elide: Text.ElideRight
            }

            Label {
                Layout.minimumWidth: 0
                text: "confidence"
                color: window.inkDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            Slider {
                id: confidenceSlider

                Layout.preferredWidth: 120
                from: 0.0
                // Not 1. core/rpc/client.h refuses a bar of exactly 1 rather
                // than answering emptily, because a track's confidence
                // approaches 1 without reaching it, so a bar of 1 lists
                // nothing however strong the signal is and an empty list is
                // what a dead band looks like too.
                to: 0.95
                stepSize: 0.01
                // EngineLink starts the bar at zero, which is everything
                // the engine will send, and the handle starts there with
                // it.
                value: 0.0
                onMoved: engineLink.confidenceBar = value

                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: "This window only. Filters what the engine sends back; "
                              + "the detector still tracks everything below it."
            }

            // From the handle and not from the link, which is the opposite
            // of the rule the row beside it follows, and for a reason
            // EngineLink states: setConfidenceBar emits nothing, because
            // the bar changes what the NEXT poll asks for and signalling
            // now would tell the overlay to redraw a list fetched at the
            // old bar. So confidenceBar notifies only when a poll comes
            // back different, and a window reading it would show a stale
            // number over a band where nothing was changing. This window is
            // the only writer of that property, so the handle is the value.
            Label {
                Layout.minimumWidth: 0
                text: confidenceSlider.value.toFixed(2) + "  this window"
                color: window.ink
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            Label {
                Layout.minimumWidth: 0
                text: "detect"
                color: window.inkDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            // NEITHER HANDLE IS BOUND TO THE LINK, WHICH IS DELIBERATE
            //
            // A handle bound to the property it writes fights the drag. Qt
            // Quick's Slider sets value itself while the handle is moving,
            // which breaks a `value:` binding on the first drag, and a
            // Binding element with `when: !pressed` reactivates on release
            // and pulls the handle back to whatever the link last reported,
            // which for the engine-side knob is a poll behind. That flicker
            // was on screen this session, and the first arrangement of it
            // also put the confidence handle at zero on its own.
            //
            // So a plain binding, which does exactly the right thing twice
            // and is then out of the way. Until the first drag it follows
            // the value in force, so the handle starts where the engine
            // already is however that engine was configured. The first drag
            // breaks it, as Qt Quick sliders do, and from then on the
            // handle is this window's request while the label beside it
            // stays the value in force. When those two part company the row
            // says so rather than moving the handle out from under whoever
            // is holding it.
            Slider {
                id: thresholdSlider

                Layout.preferredWidth: 120
                from: -3.0
                to: 40.0
                stepSize: 0.5
                value: engineLink.detectionThresholdDb
                onMoved: engineLink.detectionThresholdDb = value

                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: "The engine's, shared by every client. Changes what the "
                              + "detector finds at all. Last writer wins."
            }

            Label {
                Layout.minimumWidth: 0
                text: engineLink.detectionThresholdDb.toFixed(1)
                      + " dB SNR in 2500 Hz in force, engine-wide"
                color: window.ink
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            // Only when somebody else has moved it. The detector answers a
            // write on the next poll, so a gap wider than one step that is
            // still there is another client and not this one in flight.
            Label {
                Layout.minimumWidth: 0
                visible: engineLink.detectionDecisions > 0
                         && Math.abs(engineLink.detectionThresholdDb
                                     - thresholdSlider.value) > 0.6
                text: "(this window asked for " + thresholdSlider.value.toFixed(1) + ")"
                color: window.inkWarn
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            Item { Layout.fillWidth: true }

            // Zero decisions is the detector having been built by this
            // client's first poll and not having decided yet, which
            // core/rpc/client.h is explicit is not an empty band.
            Label {
                Layout.minimumWidth: 0
                text: engineLink.detectionDecisions === 0
                      ? "detector starting"
                      : engineLink.detectionCount + " of " + engineLink.detectionTotal
                        + " tracks shown"
                color: engineLink.detectionDecisions === 0 ? window.inkWarn : window.inkDim
                font.pixelSize: 12
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
                selectedDetection: window.selectedDetection
                onTuneRequested: (id, centerHz, bandwidthHz, candidates, rank, exhausted) =>
                                 window.takeTune(id, centerHz, bandwidthHz, candidates, rank,
                                                 exhausted)
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
                visible: window.drawing
                text: spectrum.drawCeilingDb.toFixed(1) + " dBFS ceiling"
            }

            Plate {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 4
                visible: window.drawing
                text: spectrum.drawFloorDb.toFixed(1) + " dBFS floor"
            }

            Plate {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
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
            selectedDetection: window.selectedDetection
            onTuneRequested: (id, centerHz, bandwidthHz, candidates, rank, exhausted) =>
                             window.takeTune(id, centerHz, bandwidthHz, candidates, rank,
                                             exhausted)
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

        // ------------------------------------------------------------------
        // What the last click resolved to
        // ------------------------------------------------------------------
        // NOTHING CONSUMES THIS YET, AND THE ROW SAYS SO
        //
        // Receiver control from the window is not built: EngineLink holds no
        // add_vrx and the window has no receiver list, so a click cannot
        // produce a receiver however much it looks as though it should. The
        // frequency is displayed instead of being acted on, and the second
        // line says both of the things that are wrong with acting on it, so
        // that nobody reads the number as a tuning solution.
        //
        // The centre caveat is the one that outlives the plumbing.
        // rpc::Detection::center_hz is the centre of the measured occupied
        // band, which core/detect/detector.h says in as many words is not
        // the logical centre docs/ui-spectrum.md wants. For AM it is the
        // carrier and it is right. For RTTY it lands on whichever of mark
        // and space was louder, where the logical centre is the midpoint
        // between them and has no energy in it at all. For SSB it is about
        // half a bandwidth from the suppressed carrier and moves with what
        // the speaker is saying. Nothing in the tree computes a logical
        // centre, the schema leaves it out on purpose, and the detector's
        // only Classification value is Unknown, so this is the
        // known-approximate answer rather than the intended one.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            visible: window.tunedHz > 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.minimumWidth: 0
                    text: window.tunedId > 0 ? "tune track " + window.tunedId : "tune"
                    color: window.inkTune
                    font.pixelSize: 13
                    font.bold: true
                    elide: Text.ElideRight
                }

                Label {
                    Layout.minimumWidth: 0
                    text: (window.tunedHz / 1.0e6).toFixed(6) + " MHz  ("
                          + Math.round(window.tunedHz) + " Hz)"
                    color: window.ink
                    font.pixelSize: 13
                    font.bold: true
                    elide: Text.ElideRight
                }

                Label {
                    Layout.minimumWidth: 0
                    visible: window.tunedBandwidthHz > 0
                    text: "·  " + window.bandwidthText(window.tunedBandwidthHz) + " wide"
                    color: window.ink
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }

                // WHAT ELSE WAS UNDER THAT CLICK
                //
                // The detector finds narrow sub-tracks inside a wide signal
                // and they are real, so a click on a broadcast block is
                // regularly inside a dozen boxes at once. The rule picks
                // one of them by how centrally the pointer sits in each,
                // which means the answer is a choice and the row has to say
                // that a choice was made and how to get the others.
                //
                // The second half of the sentence is read off the click
                // rather than assumed. This promised "click again for the
                // next" unconditionally, and the next click restarted the
                // walk whenever the track it was going to advance past had
                // left the list, which on a live band it usually had. The
                // cycle now survives that, and exhausted says which of the
                // two sentences is true for the next click.
                //
                // Only when there is more than one. On a lone carrier "1 of
                // 1" is noise beside a number the operator is reading.
                //
                // rank counts the cycle and candidates counts what is under
                // the pointer now, so on a band where the detector splits and
                // merges tracks between two clicks the first can pass the
                // second. That is not an error and it gets its own sentence
                // rather than being printed as "5 of 4".
                Label {
                    Layout.minimumWidth: 0
                    visible: window.tunedCandidates > 1
                    text: "·  "
                          + (window.tunedRank > window.tunedCandidates
                             ? window.tunedRank + " shown, " + window.tunedCandidates
                               + " here now, "
                             : window.tunedRank + " of " + window.tunedCandidates + " here, ")
                          + (window.tunedExhausted ? "click again to start over"
                                                   : "click again for the next")
                    color: window.inkDim
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }

                Item { Layout.fillWidth: true }
            }

            Label {
                Layout.fillWidth: true
                text: window.tunedId > 0
                      ? "no receiver was created: GUI receiver control is not built. "
                        + "This is the measured centre of the occupied band, not the mode's "
                        + "logical centre, so it is the carrier for AM and it is wrong for "
                        + "RTTY and SSB."
                      : "no detection there, so this is the frequency under the pointer."
                color: window.inkDim
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
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
