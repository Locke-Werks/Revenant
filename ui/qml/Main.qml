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

        // And the receiver actually moves, which it did not before this.
        //
        // The passband is NOT taken from the detection's measured width.
        // The detector reports the band that has energy in it, and on USB
        // that band is entirely above the suppressed carrier, so handing it
        // over as a width parks the filter straddling a carrier that is not
        // being transmitted. The engine's own per-mode default is used
        // instead, which is what an empty passband on VrxParams asks for,
        // and the operator drags from there.
        //
        // The centre caveat below still stands and is why the mode is left
        // as it is rather than guessed from the bandwidth.
        engineLink.tuneReceiver(centerHz, "")
    }

    // THE SELECTION AND THE READOUT GO WITH THE CONNECTION.
    //
    // EngineLink::adopt clears its detection list on both edges and says
    // why: the rows are absolute frequencies measured by one engine's
    // detector, and track ids are issued from one by each detector, so a
    // held selection silently becomes a different signal. Everything it
    // could clear it does. It cannot clear these, because they live here.
    //
    // Left alone, the first track the next engine issues comes up already
    // selected and outlined on both displays, chosen by nobody, and the row
    // beside it still names the previous engine's frequency and bandwidth
    // as "tune track N". That reads as a measurement of the band now on
    // screen and is a measurement of a band that may never have been
    // scanned.
    //
    // On both edges rather than only the way up, for the reason adopt gives
    // for the same choice: the reading that has just been orphaned is as
    // wrong as the one that would be inherited.
    Connections {
        target: engineLink
        function onConnectionChanged() {
            window.selectedDetection = 0
            window.tunedId = 0
            window.tunedHz = 0
            window.tunedBandwidthHz = 0
            window.tunedCandidates = 0
            window.tunedRank = 0
            window.tunedExhausted = false
        }
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
        // Tuning the front end
        // ------------------------------------------------------------------
        //
        // THE CONTROL IS GREYED RATHER THAN OFFERED AND REFUSED. A file and
        // a synthetic source cannot retune, and a control that always fails
        // teaches an operator that the window lies. sourceCanRetune is
        // asked once per connection and this row reads it; when it is
        // false, the reason is on screen beside the dead box rather than
        // arriving as a refusal after the first attempt.
        //
        // THE ECHO UNDER THE BOX IS NOT DECORATION. A bare number has to be
        // guessed at, and models/frequency_entry.h guesses megahertz below
        // a million. previewTune says which reading was taken before
        // anything is sent, and the granted line afterwards says what the
        // device actually did with it, because a tuning step rounds.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: engineLink.connected

            Label {
                text: "tune"
                color: engineLink.sourceCanRetune ? window.inkTune : window.inkDim
                font.pixelSize: 13
                font.bold: true
            }

            TextField {
                id: tuneField

                Layout.preferredWidth: 150
                enabled: engineLink.sourceCanRetune
                font.pixelSize: 13
                placeholderText: "95.1 or 95.1M"
                selectByMouse: true

                // Seeded once from where the radio is, not bound. A binding
                // would rewrite the box under an operator who is halfway
                // through typing, every time a retune landed.
                Component.onCompleted:
                    text = engineLink.sourceCenterHz > 0
                           ? (engineLink.sourceCenterHz / 1.0e6).toFixed(6) : ""

                onAccepted: {
                    if (engineLink.tuneSource(tuneField.text))
                        tuneField.selectAll()
                }
            }

            // What the text resolves to, before it is sent. Red when it
            // does not resolve at all, which is the one state where
            // pressing return does nothing useful.
            Label {
                Layout.minimumWidth: 0
                visible: tuneField.enabled && tuneField.text.length > 0
                text: engineLink.tuneTextValid(tuneField.text)
                      ? "→ " + engineLink.previewTune(tuneField.text)
                      : "not a frequency"
                color: engineLink.tuneTextValid(tuneField.text)
                       ? window.inkDim : window.inkBad
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            // The bands somebody actually reaches for. Each one is the
            // CENTRE the front end is put at, not the edge of the
            // allocation: the span the engine captures is centred here and
            // reaches half a source rate either side.
            //
            // 462.5625 MHz is the standing real-radio test band from
            // docs/, which is why it is on this row rather than only in a
            // document.
            Repeater {
                model: [
                    { "label": "FM", "hz": 98100000 },
                    { "label": "AIR", "hz": 124000000 },
                    { "label": "2m", "hz": 145000000 },
                    { "label": "70cm", "hz": 435000000 },
                    { "label": "GMRS", "hz": 462562500 }
                ]

                Label {
                    required property var modelData

                    text: modelData.label
                    color: engineLink.sourceCanRetune ? window.inkDim : "#3a4250"
                    font.pixelSize: 12

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        enabled: engineLink.sourceCanRetune
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            engineLink.tuneSourceHz(parent.modelData.hz)
                            tuneField.text =
                                (parent.modelData.hz / 1.0e6).toFixed(6)
                        }
                    }
                }
            }

            Item { Layout.fillWidth: true }

            // WHAT THE DEVICE ACTUALLY TOOK. Only when it differs from what
            // was asked, because on a source with a fine enough step the
            // two agree and a line saying so is noise. An RTL-SDR's PLL
            // step is a few hundred hertz and this is where that shows up.
            Label {
                Layout.minimumWidth: 0
                visible: engineLink.tuneAnswered
                         && engineLink.tuneGrantedHz !== engineLink.tuneRequestedHz
                text: "asked " + (engineLink.tuneRequestedHz / 1.0e6).toFixed(6)
                      + ", took " + (engineLink.tuneGrantedHz / 1.0e6).toFixed(6)
                      + " MHz"
                color: window.inkWarn
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            // Why the box is dead. Two causes and they are different news:
            // a recording cannot retune, and a client built against a wire
            // with no such call is this window's own limitation.
            Label {
                Layout.minimumWidth: 0
                visible: !engineLink.sourceCanRetune
                         && engineLink.sourceRetuneUnavailable.length > 0
                text: engineLink.sourceRetuneUnavailable
                color: window.inkDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        // The engine refused a frequency, in its own words, or this window
        // refused the text before sending it.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: engineLink.tuneFault.length > 0

            Label {
                Layout.alignment: Qt.AlignTop
                text: "tune refused:"
                color: window.inkWarn
                font.pixelSize: 12
                font.bold: true
            }

            Label {
                Layout.fillWidth: true
                text: engineLink.tuneFault
                color: window.ink
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                maximumLineCount: 2
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
        // is taken from the handle, through the one expression that also
        // writes the link. The dB threshold is a round trip and a poll, so
        // the handle is a request, the label is taken from the link, and a
        // third label appears if those two part company.
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
                // The engine's own bound, not a copy of it. core/rpc/client.h
                // refuses a bar of exactly 1 rather than answering emptily,
                // because a track's confidence approaches 1 without reaching
                // it, so a bar of 1 lists nothing however strong the signal
                // is and an empty list is what a dead band looks like too.
                // maxConfidenceBar is the largest double below 1. This read
                // 0.95 until 2026-09-20: a round number that was not the
                // engine's rule and could only drift from it.
                //
                // The range is this wide because the engine's bound is where
                // it is, not because the top of it is a setting anybody
                // should leave a display on.
                //
                // WHAT THIS PARAGRAPH USED TO SAY
                //
                // Until 2026-09-20 it finished "which is the same constant
                // setConfidenceBar clamps to, so the handle cannot reach a
                // value the link would quietly pull back". The handle reaches
                // exactly 1 and the link does quietly pull it back.
                //
                // Slider will not hold this number. QQuickSlider::setTo drops
                // an assignment that is qFuzzyCompare-equal to the value the
                // property already holds, the property starts at 1, and
                // maxConfidenceBar is 1.1e-16 short of 1. Measured on Qt
                // 6.8.3: a `to` of 1 - 1e-11 is taken and reads back, a `to`
                // of 1 - 1e-12 is dropped and `to` stays exactly 1, three
                // ways of writing it (this binding, a literal, an imperative
                // assignment) all reading back 1. So the top of the travel is
                // 1, which is the one value the engine refuses, and what
                // makes the stop legal is setConfidenceBar's clamp and
                // nothing here.
                //
                // The binding stays anyway. It is inert only for a bound
                // within 1e-12 of 1; move the engine's bound anywhere a
                // person would actually move it and this follows it, which a
                // hardcoded number would not.
                to: engineLink.maxConfidenceBar
                stepSize: 0.01
                // EngineLink starts the bar at zero, which is everything
                // the engine will send, and the handle starts there with
                // it.
                value: 0.0

                // The bar this handle is asking for. The label prints this
                // and the link is written this, from one expression, so the
                // number on screen is the number the next poll carries
                // instead of a rounded picture of it.
                //
                // Two things it has to survive, neither of which the handle
                // position guarantees on its own. A value at or past the stop
                // becomes the stop exactly, because that is what
                // setConfidenceBar clamps it to and the label would otherwise
                // be naming a bar the link never used. Everything below is
                // quantised to the control's own step, and capped one step
                // short of 1, so no position can produce a bar that prints as
                // the refused value. Today stepSize already makes every
                // reachable position a hundredth, measured; this holds if
                // that stops being true.
                readonly property double bar: {
                    if (confidenceSlider.value >= engineLink.maxConfidenceBar) {
                        return engineLink.maxConfidenceBar
                    }
                    const step = confidenceSlider.stepSize > 0
                                 ? confidenceSlider.stepSize : 0.01
                    return Math.min(Math.round(confidenceSlider.value / step) * step,
                                    1.0 - step)
                }

                onMoved: engineLink.confidenceBar = confidenceSlider.bar

                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: "This window only. Filters what the engine sends back; "
                              + "the detector still tracks everything below it.\n"
                              + "At the right-hand stop only a saturated track clears it: "
                              + "85 consecutive detections, about 8.4 s of unbroken carrier "
                              + "at the shipped settings, and one missed decision costs most "
                              + "of that back. A bursty signal never reaches it."
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
            //
            // The stop gets its own text rather than a number. toFixed(2) at
            // the top of the travel prints 1.00, which is the one value the
            // engine refuses, so the readout was naming a bar that would have
            // failed every poll.
            //
            // WHAT THIS PARAGRAPH USED TO SAY
            //
            // Until 2026-09-20 it carried the reasoning: "stepSize is 0.01
            // from zero, so every other reachable position is a hundredth and
            // rounds to itself; the stop is the only value that can round
            // up". The conclusion holds on this Qt. The reason given for it
            // is not the reason it holds, which makes it a rule a reader
            // cannot check and a reader who tried would have concluded the
            // opposite: stepSize is documented against snapMode, this slider
            // never set snapMode, and the default is Slider.NoSnap, first in
            // the enum at C:/Qt/6.8.3/msvc2022_64/qml/QtQuick/Templates/
            // plugins.qmltypes, which is the mode where a drag is supposed to
            // be continuous.
            //
            // Measured rather than argued, Qt 6.8.3 offscreen, a QtTest drag
            // and a groove click across every pixel of a 1000 px slider: the
            // VALUE lands on a hundredth under NoSnap and under SnapAlways
            // alike, 101 distinct values either way, top of travel exactly 1.
            // Slider rounds the value to stepSize whichever snapMode is in
            // force; snapMode moves the handle, not the value. The same sweep
            // with stepSize removed gives 991 values, five of them printing
            // 1.00 from below the stop, which is the failure this label is
            // here to prevent.
            //
            // Which is why the obvious repair was not taken. Adding
            // snapMode: Slider.SnapAlways would read as the fix and change
            // nothing measurable. What the label rests on now is
            // confidenceSlider.bar, the same expression the link is written,
            // so the printed number is the bar the next poll carries whatever
            // stepSize and snapMode do later.
            //
            // The word says what the stop does, which the number never did.
            // ui/models/engine_link.h has the arithmetic: a track reaches
            // this bar after 85 consecutive detections and no sooner, so
            // what is listed here is a carrier that has not stopped, and a
            // band of bursty traffic reads as empty. That is worth a label
            // because an empty list is also what a dead band looks like.
            Label {
                Layout.minimumWidth: 0
                text: (confidenceSlider.bar >= engineLink.maxConfidenceBar
                       ? "saturated only"
                       : confidenceSlider.bar.toFixed(2)) + "  this window"
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
        // The VFO detail display, and the filter drawn over it
        // ------------------------------------------------------------------
        // This is the receiver's OWN passband, transformed by the engine,
        // and the two rules over it are that receiver's filter edges. The
        // edges are dragged here, which is the one interaction in this
        // window that changes what the engine is doing rather than what the
        // window is showing.
        //
        // The pane appears when a receiver exists and not before. There is
        // nothing to draw and nothing to drag without one, and an empty
        // pane with handles in it invites a gesture that cannot do
        // anything.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            visible: engineLink.receiverId > 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: "vfo"
                    color: window.inkTune
                    font.pixelSize: 13
                    font.bold: true
                }

                Label {
                    text: (engineLink.receiverCenterHz / 1.0e6).toFixed(6) + " MHz"
                    color: window.ink
                    font.pixelSize: 13
                    font.bold: true
                }

                // The mode, as the eight buttons an operator actually
                // reaches for. Changing one is a remove and an add
                // underneath, because the demodulator is the stage; the
                // pane keeps its identity across that and the operator sees
                // a mode change.
                Repeater {
                    model: ["am", "nfm", "wfm", "usb", "lsb", "dsb", "cw", "raw"]

                    Label {
                        required property string modelData

                        text: modelData
                        color: engineLink.receiverDemod === modelData
                               ? window.inkTune : window.inkDim
                        font.pixelSize: 12
                        font.bold: engineLink.receiverDemod === modelData

                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -3
                            cursorShape: Qt.PointingHandCursor
                            onClicked: engineLink.setReceiverDemod(parent.modelData)
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Label {
                    visible: engineLink.receiverDemodRate > 0
                    text: (engineLink.receiverDemodRate / 1000).toFixed(1) + " kS/s"
                    color: window.inkDim
                    font.pixelSize: 12
                }

                Label {
                    visible: engineLink.receiverLevelDbfs > -199
                    text: engineLink.receiverLevelDbfs.toFixed(1) + " dBFS"
                    color: window.inkDim
                    font.pixelSize: 12
                }

                Label {
                    text: "clear"
                    color: window.inkDim
                    font.pixelSize: 12

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        cursorShape: Qt.PointingHandCursor
                        onClicked: engineLink.removeReceiver()
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.round(window.height * 0.18)

                PassbandItem {
                    id: passband
                    anchors.fill: parent
                    link: engineLink
                }

                Plate {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: 4
                    visible: !engineLink.passbandActive
                    text: "waiting for the first passband frame"
                }
            }

            // The readout, which is the whole of what a drag says back.
            // Live while one is running and the granted pair when one is
            // not, so the strip never goes blank and never shows a stale
            // gesture. The colour is the one thing this file decides: the
            // item reports that an edge is against the channel limit and
            // the window chooses how loudly to say so.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.minimumWidth: 0
                    text: passband.readout
                    color: passband.atLimit ? window.inkWarn
                           : engineLink.receiverClamped ? window.inkWarn : window.ink
                    font.pixelSize: 12
                    font.bold: passband.dragging
                    elide: Text.ElideRight
                }

                Item { Layout.fillWidth: true }

                Label {
                    Layout.minimumWidth: 0
                    text: "drag an edge, shift-drag to widen both, [ ] \\ select, "
                          + "arrows move, up and down widen, home resets"
                    color: window.inkDim
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }

            Label {
                Layout.fillWidth: true
                visible: engineLink.receiverFault.length > 0
                text: engineLink.receiverFault
                color: window.inkWarn
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
        }

        // ------------------------------------------------------------------
        // Audio
        // ------------------------------------------------------------------
        //
        // ONE RECEIVER AT A TIME, AND IT IS THE PANE'S. The engine serves an
        // audio subscription per receiver and this window takes exactly one,
        // on whichever receiver the pane above holds. That is stated here
        // rather than left for an operator to infer from the absence of a
        // second control: it is the same decision the pane already makes,
        // that one pane shows one receiver, extended to the one pair of
        // speakers a machine has. Mixing two streams into them is a mixer
        // with a gain and a pan per source, and nothing here makes that
        // decision on the operator's behalf.
        //
        // WHY THIS SECTION OUTLIVES THE PANE ABOVE IT. Its visibility is not
        // receiverId alone. A receiver removed out from under a live stream
        // is the one failure on this path with no symptom of its own, and
        // the words explaining it would disappear with the pane exactly when
        // they are needed. The same holds for a device that refused the
        // format.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            // audioWanted is in here and not only receiverId. The switch is
            // sticky across a retune, a mode change and a reconnect, which
            // is what makes listening survive a rebuild, and a sticky
            // switch that cannot be seen is one the operator cannot turn
            // off: clearing the receiver would hide it armed, and the next
            // receiver they tuned would start making noise with no control
            // on screen that explains why. Observed 2026-09-20 by clearing
            // a receiver while listening.
            visible: engineLink.receiverId > 0
                     || engineLink.audioWanted
                     || engineLink.audioEndedReason.length > 0
                     || engineLink.audioFault.length > 0
                     || audioPlayer.fault.length > 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: "audio"
                    color: window.inkTune
                    font.pixelSize: 13
                    font.bold: true
                }

                // The switch. It is what the operator asked for and stays
                // on across a retune, a mode change and a reconnect;
                // audioActive beside it is whether there is a stream.
                Label {
                    text: engineLink.audioWanted ? "listening" : "listen"
                    color: engineLink.audioWanted ? window.inkTune : window.inkDim
                    font.pixelSize: 12
                    font.bold: engineLink.audioWanted

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        cursorShape: Qt.PointingHandCursor
                        onClicked: engineLink.audioWanted = !engineLink.audioWanted
                    }
                }

                // Which receiver, named rather than assumed. The switch can
                // be on with nothing subscribed, which is the ordinary
                // state before anything is tuned.
                Label {
                    text: engineLink.audioActive
                          ? "vfo " + engineLink.audioReceiverId
                          : (engineLink.audioWanted ? "no receiver" : "")
                    color: engineLink.audioActive ? window.ink : window.inkDim
                    font.pixelSize: 12
                }

                Label {
                    text: audioPlayer.muted ? "muted" : "mute"
                    color: audioPlayer.muted ? window.inkWarn : window.inkDim
                    font.pixelSize: 12
                    font.bold: audioPlayer.muted

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        cursorShape: Qt.PointingHandCursor
                        onClicked: audioPlayer.muted = !audioPlayer.muted
                    }
                }

                Slider {
                    id: volumeSlider

                    Layout.preferredWidth: 110
                    from: 0.0
                    to: 1.0
                    enabled: !audioPlayer.muted

                    // Seeded once rather than bound, because a binding to
                    // audioPlayer.volume is broken by the first drag
                    // anyway and a half-live binding is worse than none.
                    // This control is the only writer.
                    Component.onCompleted: value = audioPlayer.volume
                    onMoved: audioPlayer.volume = value
                }

                Label {
                    text: Math.round(audioPlayer.volume * 100) + "%"
                    color: audioPlayer.muted ? window.inkDim : window.ink
                    font.pixelSize: 12
                }

                ComboBox {
                    id: deviceBox

                    Layout.preferredWidth: 220
                    model: audioPlayer.devices
                    font.pixelSize: 12
                    onActivated: audioPlayer.device = currentIndex

                    // NOT currentIndex: audioPlayer.device. ComboBox writes
                    // its own currentIndex when the user picks, and a
                    // direct assignment to a property destroys the binding
                    // on it, so the picker followed audioPlayer.device
                    // exactly until the first selection and never again.
                    // After that, a device pulled out of its socket moved
                    // the property and left the picker showing a device
                    // that has gone.
                    //
                    // A Binding object is the standard answer: it WRITES
                    // the value rather than installing a binding on the
                    // property, so the ComboBox's own assignment does not
                    // destroy it and it reasserts on the next change.
                    Binding {
                        target: deviceBox
                        property: "currentIndex"
                        value: audioPlayer.device
                        restoreMode: Binding.RestoreNone
                    }

                    // And a reassert for the case the Binding cannot see:
                    // refresh_devices can rebuild the list without moving
                    // the selection, ComboBox resets currentIndex to 0 on
                    // a model change, and audioPlayer.device has not
                    // changed so nothing above re-fires.
                    onCountChanged: currentIndex = audioPlayer.device
                }

                Item { Layout.fillWidth: true }

                // WHAT IS COMING OUT OF THE SPEAKER, in a word or two. Five
                // of the six are silence and they are five different
                // things: see FrameSource in ui/audio/audio_ring.h for the
                // first five and the source property in
                // ui/audio/audio_player.h for "format mismatch", which is
                // the player's own. It follows the frames reaching the card
                // and not the newest chunk off the wire, so it is in step
                // with what is audible.
                Label {
                    text: audioPlayer.source
                    color: audioPlayer.source === "audio" ? window.inkTune
                           : audioPlayer.source === "squelched" ? window.inkDim
                           : audioPlayer.source === "waiting" ? window.inkDim
                           : window.inkWarn
                    font.pixelSize: 12
                    font.bold: audioPlayer.source === "audio"
                }

                // The three depths, so the derivation is on screen. The
                // grant is what the engine holds and what the ring is sized
                // from; the last figure is the sink's own buffer, which is
                // the only one of the three that is always latency.
                Label {
                    visible: engineLink.audioActive
                    text: engineLink.audioGrantedMillis + " ms granted  ·  "
                          + audioPlayer.bufferedMillis + "/" + audioPlayer.ringMillis
                          + " ms buffered  ·  " + audioPlayer.sinkMillis + " ms out"
                    color: window.inkDim
                    font.pixelSize: 11
                }
            }

            // The counters, and only when there is something to say. Split
            // by cause, because a wire drop, a starved card and a resync
            // sound alike and have three different fixes.
            Label {
                Layout.fillWidth: true
                visible: engineLink.audioActive
                         && (audioPlayer.gapEvents > 0 || audioPlayer.starvedFrames > 0
                             || audioPlayer.overrunFrames > 0
                             || engineLink.audioFramesDropped > 0)
                text: "gaps " + audioPlayer.gapEvents + " ("
                      + audioPlayer.gapWireFrames + " wire, "
                      + audioPlayer.gapUpstreamFrames + " upstream frames, "
                      + audioPlayer.gapFilledFrames + " filled with silence)"
                      + "  ·  resyncs " + audioPlayer.resyncs
                      + "  ·  starved " + audioPlayer.starvedFrames + " frames"
                      + "  ·  overran " + audioPlayer.overrunFrames + " frames"
                      + "  ·  engine dropped " + engineLink.audioFramesDropped
                      + " in " + engineLink.audioDropEvents + " events"
                color: window.inkWarn
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            // THE RECEIVER WENT AWAY. This is the sentence the whole ended
            // callback exists for: audio that simply stops is what a quiet
            // channel sounds like, so the engine says so and this is where
            // its words go. inkBad rather than inkWarn because the stream
            // is over and will not come back on its own.
            Label {
                Layout.fillWidth: true
                visible: engineLink.audioEndedReason.length > 0
                text: "the audio stopped: " + engineLink.audioEndedReason
                      + ". Tune a receiver and click listen again."
                color: window.inkBad
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            // The engine refused the subscription, in its own words.
            Label {
                Layout.fillWidth: true
                visible: engineLink.audioFault.length > 0
                text: engineLink.audioFault
                color: window.inkWarn
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            // The sound card refused, or went away. Kept apart from the
            // line above it because one is fixed by picking another output
            // and the other is not.
            Label {
                Layout.fillWidth: true
                visible: audioPlayer.fault.length > 0
                text: audioPlayer.fault
                color: window.inkWarn
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            // Something the player adapted, which is not a fault and is not
            // coloured as one. Today the only one is a mono stream copied
            // onto a device that takes no mono, which is the ordinary case
            // on a machine whose default output is stereo only.
            Label {
                Layout.fillWidth: true
                visible: audioPlayer.note.length > 0
                text: audioPlayer.note
                color: window.inkDim
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
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
        // WHAT THIS ROW USED TO SAY
        //
        // Until the passband work it said that receiver control from the
        // window was not built, that EngineLink held no add_vrx, and that a
        // click therefore could not produce a receiver however much it
        // looked as though it should. All three are now false: takeTune
        // above calls tuneReceiver, the detail pane below draws that
        // receiver's passband, and its filter edges are dragged there. The
        // sentence is recorded rather than deleted because it was the
        // explanation for the whole of this row's shape.
        //
        // The centre caveat is the one that outlived the plumbing.
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
                      ? "the receiver moved here. This is the measured centre of the "
                        + "occupied band, not the mode's logical centre, so it is the "
                        + "carrier for AM and it is wrong for RTTY and SSB: drag the "
                        + "filter edges below to put the passband where the signal is."
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
