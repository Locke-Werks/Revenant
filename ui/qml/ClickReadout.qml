// What the last click resolved to.
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

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: readout

    // What the click resolved to. See TuneSelection.qml.
    required property TuneSelection selection

    function bandwidthText(hz) {
        if (hz >= 1.0e6)
            return (hz / 1.0e6).toFixed(3) + " MHz"
        if (hz >= 1000)
            return (hz / 1000).toFixed(2) + " kHz"
        return Math.round(hz) + " Hz"
    }

    Layout.fillWidth: true
    spacing: 0
    visible: readout.selection.tunedHz > 0

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            Layout.minimumWidth: 0
            text: readout.selection.tunedId > 0 ? "tune track " + readout.selection.tunedId : "tune"
            color: Theme.inkTune
            font.pixelSize: Theme.sizeTitle
            font.bold: true
            elide: Text.ElideRight
        }

        Label {
            Layout.minimumWidth: 0
            text: (readout.selection.tunedHz / 1.0e6).toFixed(6) + " MHz  ("
                  + Math.round(readout.selection.tunedHz) + " Hz)"
            color: Theme.ink
            font.pixelSize: Theme.sizeTitle
            font.bold: true
            elide: Text.ElideRight
        }

        Label {
            Layout.minimumWidth: 0
            visible: readout.selection.tunedBandwidthHz > 0
            text: "·  " + readout.bandwidthText(readout.selection.tunedBandwidthHz) + " wide"
            color: Theme.ink
            font.pixelSize: Theme.sizeTitle
            elide: Text.ElideRight
        }

        // How far this track stood above the detection threshold, on
        // the same zero-to-one scale the bar above uses and measuring
        // something completely different: that one is how long, this
        // is how strong.
        //
        // Bound to a lookup rather than carried in from the click, so
        // it follows the track while the row is up and disappears when
        // the track does. A negative reading is the list no longer
        // holding this id, which is why it is a visibility test rather
        // than a zero.
        //
        // IT DOES NOT SAY THE SIGNAL IS REAL. A strong interferer
        // stands well above the noise and reads high, correctly.
        Label {
            Layout.minimumWidth: 0

            readonly property double margin:
                readout.selection.tunedId > 0 ? engineLink.detectionMargin(readout.selection.tunedId) : -1.0

            visible: margin >= 0.0
            text: "·  margin " + margin.toFixed(2)
            color: Theme.inkDim
            font.pixelSize: Theme.sizeTitle
            elide: Text.ElideRight
        }

        // Whether the band is SHAPED like a transmission, which is
        // the third different question on this row and the one the
        // other two cannot answer: "held for" is how long, margin is
        // how strong, and a raised patch of noise floor scores well
        // on both. Measured on 20 m, an 11.7 kHz patch at confidence
        // 1.00 read 0.02 here while the carriers beside it read 0.59
        // to 0.86.
        //
        // Hidden rather than zeroed when negative, which here covers
        // the track having left the list AND the shape not having
        // been measured. engine_link.h folds the two together on
        // purpose: an unmeasured band arrives as 0.0 and 0.0 is the
        // most noise-like reading there is, so drawing it would be
        // reporting "we could not tell" as "certainly junk".
        //
        // LABELLED WITH WHAT IT MEASURES AND NOT WITH A VERDICT.
        // "concentrated" against "spread" was written here first and
        // taken out: picking the word needs a threshold, no threshold
        // has been chosen, and core/detect/shape.h is explicit that
        // choosing one is a measurement against known truth rather
        // than a constant somebody liked. Inventing one in a QML
        // string would be the same mistake "confidence" made, made
        // where it is hardest to find.
        //
        // So the row says what the number is. A reader who wants to
        // know whether 0.31 is good has the bandwidth on the same
        // row, which is the comparison that answers it.
        Label {
            Layout.minimumWidth: 0

            readonly property double concentration:
                readout.selection.tunedId > 0
                    ? engineLink.detectionConcentration(readout.selection.tunedId)
                    : -1.0

            visible: concentration >= 0.0
            text: "·  " + concentration.toFixed(2) + " of it in 3 bins"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeTitle
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
            visible: readout.selection.tunedCandidates > 1
            text: "·  "
                  + (readout.selection.tunedRank > readout.selection.tunedCandidates
                     ? readout.selection.tunedRank + " shown, " + readout.selection.tunedCandidates
                       + " here now, "
                     : readout.selection.tunedRank + " of " + readout.selection.tunedCandidates + " here, ")
                  + (readout.selection.tunedExhausted ? "click again to start over"
                                           : "click again for the next")
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideRight
        }

        Item { Layout.fillWidth: true }
    }

    Label {
        Layout.fillWidth: true
        text: readout.selection.tunedId > 0
              ? "the receiver moved here. This is the measured centre of the "
                + "occupied band, not the mode's logical centre, so it is the "
                + "carrier for AM and it is wrong for RTTY and SSB: drag the "
                + "filter edges below to put the passband where the signal is."
              : "no detection there, so this is the frequency under the pointer."
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }
}
