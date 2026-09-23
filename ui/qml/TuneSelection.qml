// What the last click resolved to, and which track is drawn as chosen.
//
// THE SELECTION LIVES HERE AND NOT IN EITHER ITEM
//
// Two displays show the same detections and there is one chosen track, not
// one per display. So a click emits tuneRequested, this object decides what
// that meant, and both items read selectedDetection back as a binding. An
// item that set its own would break the binding the other one is reading,
// and the two would disagree about which box is chosen.
//
// It was a set of properties on the window until the window was split into
// files. One object, made once in Main.qml and handed to the span view that
// writes it and the readout that reads it, is the same arrangement with the
// owner named.

import QtQuick
import Revenant

QtObject {
    id: selection

    // What the last click resolved to. tunedId is zero for a click that
    // landed on bare spectrum, which is a frequency and not a track: track
    // ids are issued from one, per core/detect/detector.h.
    property real tunedHz: 0

    // Whether there has been a click since the connection came up. Zero hertz
    // is a frequency on a synthetic scene centred there, so tunedHz cannot
    // stand in for this.
    property bool tuned: false
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

    function record(id, centerHz, bandwidthHz, candidates, rank, exhausted) {
        selection.selectedDetection = id
        selection.tuned = true
        selection.tunedId = id
        selection.tunedHz = centerHz
        selection.tunedBandwidthHz = bandwidthHz
        selection.tunedCandidates = candidates
        selection.tunedRank = rank
        selection.tunedExhausted = exhausted
    }

    // One click, whichever display it came from. pointerHz is the frequency
    // under the pointer, which EngineLink::spanClick reads to tell a click
    // inside another receiver's band, which focuses that receiver and tunes
    // nothing, from a click anywhere else. Only a click that tuned is a new
    // reading for the readout.
    function takeTune(id, centerHz, bandwidthHz, candidates, rank, exhausted, pointerHz) {
        if (engineLink.spanClick(pointerHz, centerHz, bandwidthHz))
            selection.record(id, centerHz, bandwidthHz, candidates, rank, exhausted)
    }

    // The second click of a double click: a new receiver there, when
    // EngineLink::spanDoubleClick adds one.
    function takeAdd(id, centerHz, bandwidthHz, pointerHz) {
        if (engineLink.spanDoubleClick(pointerHz, centerHz, bandwidthHz))
            selection.record(id, centerHz, bandwidthHz, 0, 0, false)
    }

    // WHAT takeTune USED TO DO: record the click and then call
    // engineLink.tuneReceiverToDetection(centerHz, "", bandwidthHz) itself.
    // spanClick makes that call now, after deciding the click was not a
    // focus, and this note is about that call.
    //
    // The passband is NOT taken from the detection's measured width. The
    // detector reports the band that has energy in it, and on USB that band
    // is entirely above the suppressed carrier, so handing it over as a width
    // parks the filter straddling a carrier that is not being transmitted.
    // The engine's own per-mode default is used instead, which is what an
    // empty passband on VrxParams asks for, and the operator drags from
    // there.
    //
    // The measured width IS handed over, on a separate entry point, and it is
    // not used as a passband for the reason just given. It does two other
    // things. It says whether the filter the engine built fits the signal
    // that was clicked on, which is the only moment in the whole window where
    // both numbers are in one place. And it CHOOSES THE MODE.
    //
    // The empty string used to mean "leave the mode alone", and that is what
    // gave a 145 kHz broadcast block a 16 kHz NFM receiver: a paragraph here
    // once argued the mode should not be guessed from the bandwidth, and the
    // bandwidth was the only evidence anybody had. It now means "choose from
    // the measurement", which EngineLink::tuneReceiverToDetection does
    // through ui::demod_for_detection. A named mode still wins, for a caller
    // that knows something the detector does not.

    // Put the readout away without changing the selection.
    function dismiss() {
        selection.tuned = false
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
    property Connections resetOnConnection: Connections {
        target: engineLink
        function onConnectionChanged() {
            selection.selectedDetection = 0
            selection.tuned = false
            selection.tunedId = 0
            selection.tunedHz = 0
            selection.tunedBandwidthHz = 0
            selection.tunedCandidates = 0
            selection.tunedRank = 0
            selection.tunedExhausted = false
        }
    }
}
