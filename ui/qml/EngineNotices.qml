// What the engine is saying that is not a picture: that it cannot be
// reached, that it built something other than what was asked for, and
// that its detector is refusing.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    Layout.fillWidth: true
    spacing: 6

    // Shown whenever any of the three has something to say.
    visible: (!engineLink.connected && engineLink.errorText.length > 0)
             || (engineLink.connected && engineLink.clamped)
             || (engineLink.connected && engineLink.detectionFault.length > 0)

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
    NoticeRow {
        visible: !engineLink.connected && engineLink.errorText.length > 0
        heading: "retrying:"
        headingColor: Theme.inkBad
        body: engineLink.errorText
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
    NoticeRow {
        visible: engineLink.connected && engineLink.detectionFault.length > 0
        heading: "detector refused:"
        headingColor: Theme.inkBad
        body: engineLink.detectionFault
    }
}
