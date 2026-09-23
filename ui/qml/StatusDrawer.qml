// Everything the window knows about the engine that is not a fault, behind
// the status pill in the top bar.
//
// These were rows of the window's column, on screen all the time. They are
// true and they are ordinary, and together they pushed the spectrum down and
// made a healthy engine look like a list of complaints. Each keeps its words
// and its reasoning here; the ones that can become a fault are also brought
// forward by NoticeBanner.qml while they are one, and
// models/status_summary.h decides which is which.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    spacing: 8

    // What the engine is: its device, its geometry, and whether frames are
    // arriving. See StatusRow.qml for why each label elides.
    StatusRow {}

    // ------------------------------------------------------------------
    // Whether the source is keeping up
    // ------------------------------------------------------------------
    //
    // HERE AND NOT BESIDE THE VOLUME SLIDER. On 2026-09-20 a synthetic
    // source at 0.20x produced chopped audio and the only thing on
    // screen that said anything was "starving" next to the audio
    // counters, which sent the operator into the audio path for twenty
    // minutes. Starving means the audio is LATE, which points at this
    // machine; a source that is behind is short at the far end and
    // every stage downstream of it is doing the right thing with what
    // it was given.
    //
    // So the line sits with the frame rate, which is the other
    // measurement of how fast the engine is producing, and the two
    // read together: rows/s is what is reaching the display and this
    // is whether the radio is supplying it. While the source is behind,
    // the same sentence is in the banner as well, because then it is news.
    //
    // The wording and the threshold are in models/source_pacing.h with
    // their own cases in ui/tests. This file chooses the colour and
    // nothing else, which is the rule the whole window follows.
    NoticeRow {
        visible: engineLink.connected && engineLink.pacingText.length > 0
        heading: engineLink.sourceBehind ? "source:" : "pace:"
        headingColor: engineLink.sourceBehind ? Theme.inkBad : Theme.inkDim
        headingBold: engineLink.sourceBehind
        body: engineLink.pacingText
        bodyColor: engineLink.sourceBehind ? Theme.ink : Theme.inkDim
        bodyBold: engineLink.sourceBehind
    }

    // What the front end is doing. The radio panel carries the same line
    // under the gain control it names; see SourcePicker.qml.
    NoticeRow {
        visible: engineLink.connected && engineLink.frontEndText.length > 0
        heading: "front end:"
        headingColor: engineLink.frontEndFault ? Theme.inkBad : Theme.inkDim
        headingBold: engineLink.frontEndFault
        body: engineLink.frontEndText
        bodyColor: engineLink.frontEndFault ? Theme.ink : Theme.inkDim
        bodyBold: engineLink.frontEndFault
        lines: 3
    }

    // ------------------------------------------------------------------
    // What the engine built, when it is not what was asked for
    // ------------------------------------------------------------------
    // A clamp is the ordinary case and not a fault: a ring rounded down
    // to a power of two, a grid that took half the channels asked for, a
    // dispatch capped by the channel ring. Frames keep arriving and they
    // look correct, which is why this has to be reachable at all.
    // core/engine/engine.cpp says what happens otherwise, where it packs
    // the note into the ring's field: the operator finds out when a
    // frequency lands in the wrong channel.
    //
    // So a line of text and not a dialog, and the sentence rather than
    // the flag. Which limit bound, what was asked for and what was built
    // are all in the sentence; clamped only decides whether the row is
    // here. "clamped:" is the word tools/cli/main.cpp prints for the same
    // field, so the two clients describe one engine the same way. The pill
    // says there is a note to read while one is here.
    NoticeRow {
        visible: engineLink.connected && engineLink.clamped
        heading: "clamped:"
        headingColor: Theme.inkWarn
        // Every producer of the flag writes a reason with it, so the
        // fallback is unreachable today. It is here because a bare
        // "clamped:" with nothing after it would read as this row
        // failing rather than as the engine saying nothing.
        body: engineLink.clampReason.length > 0
              ? engineLink.clampReason
              : "the engine built something other than what was asked for and gave no reason"
        lines: 3
    }

    // ------------------------------------------------------------------
    // Receivers this window does not hold
    // ------------------------------------------------------------------
    //
    // A receiver outlives the client that created it, which is what lets
    // two windows each hold their own. Closing a window releases its
    // receiver; a window killed outright does not, and the engine then
    // carries a channelizer slot and its GPU work until it is restarted.
    //
    // THE RELEASE IS NOT OFFERED AS A REPAIR, because from here an orphan
    // and another operator's working receiver are the same thing: nothing
    // on the wire says who created one. So the row states the count and
    // the action says exactly what it will do, and somebody who knows
    // whether another window is open decides.
    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 8
        visible: engineLink.strandedReceiverText.length > 0

        Label {
            Layout.minimumWidth: 0
            Layout.fillWidth: true
            text: engineLink.strandedReceiverText
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideRight
        }

        RButton {
            text: "release them"
            ink: Theme.inkWarn
            onClicked: engineLink.releaseStrandedReceivers()
        }
    }

    Label {
        Layout.fillWidth: true
        visible: engineLink.connected && !engineLink.spectrumEnabled
        text: "This engine was built with no spectrum stage, so there are no frames to draw."
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeBody
        wrapMode: Text.WordWrap
    }
}
