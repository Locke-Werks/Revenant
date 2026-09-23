// What is wrong, when something is, and nothing when nothing is.
//
// A strip under the top bar that exists only while a fault or a refusal does.
// Everything here used to be a row of the window's column of its own, drawn
// whenever its condition held; the conditions and the words are unchanged,
// and each row's reasoning moved with it. What changed is that a healthy
// engine no longer draws any of them, so the spectrum starts under the top bar
// and the strip that pushes it down is itself the news.
//
// Which conditions come forward here and which wait in the status drawer is
// models/status_summary.h, which has the cases. Notes, a clamp or receivers
// other windows hold, are in the drawer and never here.
//
// NO SENTENCES IN THE STRIP. A refusal used to print across the whole window,
// which on a wide screen is a line too long to read in one sweep of the eye,
// and the strip began as a column of those. Each is now a chip naming the
// problem, with the engine's or this client's sentence verbatim on hover; see
// controls/StatusChip.qml.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Rectangle {
    id: banner

    readonly property bool retrying: !engineLink.connected && engineLink.errorText.length > 0
    readonly property bool detectorRefused: engineLink.connected
                                            && engineLink.detectionFault.length > 0
    readonly property bool behind: engineLink.connected && engineLink.sourceBehind
                                   && engineLink.pacingText.length > 0
    readonly property bool frontEndFault: engineLink.connected && engineLink.frontEndFault
                                          && engineLink.frontEndText.length > 0
    readonly property bool tuneRefused: engineLink.tuneFault.length > 0
    readonly property bool radioRefused: engineLink.sourceFault.length > 0
    readonly property bool receiverGone: engineLink.receiverGoneText.length > 0

    visible: retrying || detectorRefused || behind || frontEndFault || tuneRefused
             || radioRefused || receiverGone
    implicitHeight: rows.implicitHeight + 12
    // The fault ink at a tenth, from the Theme rather than written out here.
    color: Qt.rgba(Theme.inkBad.r, Theme.inkBad.g, Theme.inkBad.b, 0.10)
    border.width: 0

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }

    // Chips in a row, wrapping if there are ever more than fit. Each is the
    // name of the problem, and its sentence, verbatim, is its tooltip.
    Flow {
        id: rows

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        anchors.leftMargin: 10
        spacing: 8

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
        StatusChip {
            visible: banner.retrying
            label: "no engine"
            detail: engineLink.errorText
            ink: Theme.inkBad
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
        // A line of text rather than a dialog: the overlay keeps drawing the
        // last list it had, which looks correct, and the operator finds out
        // when a track they can see on the waterfall never gets a box.
        StatusChip {
            visible: banner.detectorRefused
            label: "detector refused"
            detail: engineLink.detectionFault
            ink: Theme.inkBad
        }

        // The source is behind real time. The same row the status drawer
        // carries while the source is keeping up, promoted here when it is
        // not, because then the picture is short at the far end and every
        // stage downstream is doing the right thing with too little. See the
        // drawer for why this is about the radio and not the audio.
        StatusChip {
            visible: banner.behind
            label: "source behind"
            detail: engineLink.pacingText
            ink: Theme.inkBad
        }

        // The front end is overloaded. The same sentence the drawer and the
        // radio panel carry, brought forward while it is a fault.
        StatusChip {
            visible: banner.frontEndFault
            label: "front end overloaded"
            detail: engineLink.frontEndText
            ink: Theme.inkBad
        }

        // The engine refused a frequency, in its own words, or this window
        // refused the text before sending it.
        StatusChip {
            visible: banner.tuneRefused
            label: "tune refused"
            detail: engineLink.tuneFault
            ink: Theme.inkWarn
        }

        // What the last open or close said when it refused. A registry
        // refusal names the backends it does know, and that sentence is
        // longer than a status strip.
        StatusChip {
            visible: banner.radioRefused
            label: "radio refused"
            detail: engineLink.sourceFault
            ink: Theme.inkBad
        }

        // ------------------------------------------------------------------
        // A receiver the engine let go
        // ------------------------------------------------------------------
        //
        // OUTSIDE THE RECEIVER'S OWN CONTROLS ON PURPOSE. Those are drawn only
        // while a receiver exists, so a line among them explaining that the
        // receiver has gone is a line that goes with it. This sits under the
        // tuning controls that caused it, and clears when the operator tunes
        // somewhere rather than when a receiver comes back. The receiver
        // window says it too, where the strip was.
        //
        // The wheel is what makes this worth a row of its own: a long sweep
        // walks the front end several spans, and the receiver is dropped
        // somewhere in the middle of a gesture that is still going.
        StatusChip {
            visible: banner.receiverGone
            label: "receiver let go"
            detail: engineLink.receiverGoneText
            ink: Theme.inkWarn
        }

        // The one removal an add answers. A retune that moved a receiver to a
        // place in its channel needing a different filter was refused only
        // because the engine builds a new filter for a new receiver, so this
        // puts one back at the frequency it was on, in the same mode. Offered
        // for nothing else, because anything else would be refused again; see
        // receiver_can_come_back in models/receiver_gone.h. A chip that acts,
        // not a dialog: the sentence beside it is the question.
        StatusChip {
            visible: banner.receiverGone && engineLink.receiverComebackHz > 0
            label: "add it back"
            detail: "Adds a receiver again on the frequency the retune let go, in the same mode. "
                    + "The engine builds the filter its new place in the channel needs."
            ink: Theme.inkWarn

            Accessible.role: Accessible.Button
            Accessible.name: "add the receiver back"

            TapHandler {
                onTapped: engineLink.addGoneReceiverBack()
            }
        }
    }
}
