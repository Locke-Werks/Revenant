// The detector's three settings, in the control row under the top bar
// ------------------------------------------------------------------
// docs/detection.md puts all of these on the operator and says why:
// where a person wants them depends on the band, the antenna and
// what they are doing, and a default that suits a quiet VHF band
// buries an HF evening.
//
// IN THE CONTROL ROW AND NOT A PANEL, since 2026-09-23. The owner
// asked for the row the fault strip used to take to hold these and
// the gain instead, so they are on screen while the band they judge
// is. There is one of each: the detections panel they lived in is
// gone, and its key puts the arrow keys on the threshold instead.
//
// WHAT THIS PARAGRAPH USED TO SAY, AS THE FILE'S TITLE: "The
// detector's two thresholds, which are two different knobs".
//
// They are not the same knob and the row has to make that legible.
// The confidence bar ("held") is this window's: it is the
// min_confidence argument to Client::detections and it only filters
// what comes back, so moving it changes this display and nothing
// else. The margin bar ("stronger") is this window's the same way.
// The dB threshold ("detect") is the engine's: it changes what the
// detector decides at all, every client sees the result, and the last
// writer wins. So the number beside that slider is the value IN
// FORCE, read back from DetectionList::detection_threshold_db, rather
// than the one this window last asked for, and it turns the warning
// colour when another client has set something else.
//
// All three are remembered across a restart, and the threshold is
// sent again on each connection; models/detector_settings.h has the
// rules, and why a tune no longer puts the threshold back to 6 dB.
//
// One more difference is visible in how the two kinds behave, and the
// row reads the numbers from different places because of it. A bar is
// local, so the handle is the value and the label is taken from the
// handle, through the one expression that also writes the link. The dB
// threshold is a round trip and a poll, so the handle is a request and
// the label is taken from the link.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

RowLayout {
    id: detector

    spacing: 6
    enabled: engineLink.connected && engineLink.spectrumEnabled

    // The arrow keys onto the threshold, which is what the detections key
    // does now that there is no panel for it to open.
    function focusThreshold() {
        thresholdSlider.forceActiveFocus(Qt.ShortcutFocusReason)
    }

    Label {
        text: "detect"
        color: detector.enabled ? Theme.inkDim : Theme.inkOff
        font.pixelSize: Theme.sizeSmall
    }

    // THE HANDLE IS THIS WINDOW'S REQUEST, AND IT STARTS WHERE THE
    // OPERATOR LEFT IT. detectionThresholdWanted is the value this window
    // last asked for, remembered across a restart, or the value in force
    // when it never has. A plain binding, which a drag breaks, as Qt Quick
    // sliders do; from then on the handle is where the operator put it,
    // which is also what the property says, so nothing is lost.
    //
    // NOT BOUND TO THE VALUE IN FORCE, WHICH IS DELIBERATE. A handle bound
    // to the property it writes fights the drag: Slider sets value itself
    // while the handle moves, and a Binding with `when: !pressed` pulls the
    // handle back on release to whatever the link last reported, which for
    // an engine-side knob is a poll behind. That flicker was on screen once.
    RSlider {
        id: thresholdSlider

        Layout.preferredWidth: 96
        from: -3.0
        to: 40.0
        stepSize: 0.5
        value: engineLink.detectionThresholdWanted
        onMoved: engineLink.detectionThresholdDb = value

        Tip {
            visible: parent.hovered || parent.visualFocus
            delay: 400
            text: "The engine's detection threshold, in dB of SNR in 2500 Hz. Shared by "
                  + "every client, and the last writer wins. Changes what the detector finds at all."
                  + (engineLink.detectionThresholdOverridden
                     ? "\nAnother client set " + engineLink.detectionThresholdDb.toFixed(1)
                       + " dB; this window asked for " + thresholdSlider.value.toFixed(1) + "."
                     : "")
        }
    }

    // The value in force, engine-wide. The warning colour is another client
    // having set something else since this window's write, which the
    // detector answers on the next poll, so a gap that outlives a decision is
    // not this window in flight.
    Readout {
        widest: "-00.0 dB"
        // Nothing in force to show without a detector to hold it.
        text: detector.enabled ? engineLink.detectionThresholdDb.toFixed(1) + " dB" : "-- dB"
        color: !detector.enabled ? Theme.inkOff
               : engineLink.detectionThresholdOverridden ? Theme.inkWarn : Theme.ink
    }

    Rectangle {
        Layout.preferredWidth: 1
        Layout.preferredHeight: 14
        Layout.leftMargin: 4
        Layout.rightMargin: 4
        color: Theme.border
    }

    // NOT LABELLED "confidence", WHICH IS WHAT IT FILTERS ON AND NOT
    // WHAT IT MEANS. The wire field is called confidence and this bar
    // is passed to it, but core/detect/detector.h computes it from
    // consecutive detections and nothing else: it is one minus
    // (1 - rise) to the n, it reads no SNR and no shape, and it
    // reaches 1.00 in about 1.3 seconds for anything that stays put.
    // An operator read the old word as "how sure are we this is real",
    // moved the bar to filter out interference, and filtered by how
    // long each signal had been there instead. The bar is useful and
    // the word was wrong, so the word changed; "held" is "held for"
    // shortened to fit the row.
    Label {
        text: "held"
        color: detector.enabled ? Theme.inkDim : Theme.inkOff
        font.pixelSize: Theme.sizeSmall
    }

    RSlider {
        id: confidenceSlider

        Layout.preferredWidth: 80
        from: 0.0
        // The engine's own bound, not a copy of it. core/rpc/client.h
        // refuses a bar of exactly 1 rather than answering emptily,
        // because a track's confidence approaches 1 without reaching
        // it, so a bar of 1 lists nothing however strong the signal
        // is and an empty list is what a dead band looks like too.
        // maxConfidenceBar is the largest double below 1.
        //
        // Slider will not hold this number. QQuickSlider::setTo drops
        // an assignment that is qFuzzyCompare-equal to the value the
        // property already holds, the property starts at 1, and
        // maxConfidenceBar is 1.1e-16 short of 1. Measured on Qt
        // 6.8.3: a `to` of 1 - 1e-11 is taken and reads back, a `to`
        // of 1 - 1e-12 is dropped and `to` stays exactly 1. So the top
        // of the travel is 1, which is the one value the engine
        // refuses, and what makes the stop legal is the `bar`
        // expression below and setConfidenceBar's clamp, nothing here.
        // The binding stays anyway: it follows the engine's bound if
        // that ever moves anywhere a person would move it.
        to: engineLink.maxConfidenceBar
        stepSize: 0.01

        // SEEDED FROM THE LINK AND NOT BOUND TO IT, on the same pattern
        // the volume slider uses and for the same reason: a binding is
        // broken by the first drag anyway, and a half-live binding is
        // worse than none. This control is the only writer, and the bar
        // is remembered across launches, so a literal here would put the
        // handle at the bottom while the link filtered at last night's
        // setting.
        Component.onCompleted: value = engineLink.confidenceBar

        // The bar this handle is asking for. The label prints this and
        // the link is written this, from one expression, so the number
        // on screen is the number the next poll carries.
        //
        // A value at or past the stop becomes the stop exactly, because
        // that is what setConfidenceBar clamps it to. Everything below
        // is quantised to the control's own step and capped one step
        // short of 1, so no position can produce a bar that prints as
        // the refused value. Measured on Qt 6.8.3 offscreen across every
        // pixel of a 1000 px slider: Slider rounds the value to stepSize
        // whichever snapMode is in force, 101 distinct values, top of
        // travel exactly 1. Adding snapMode would read as a fix and
        // change nothing; what the label rests on is this expression.
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

        Tip {
            visible: parent.hovered
            delay: 400
            text: "Held for: this window only. Filters what the engine sends back by how long "
                  + "each track has been detected without a break; the detector still tracks "
                  + "everything below it.\n"
                  + "At the right-hand stop only a saturated track clears it: "
                  + "85 consecutive detections, about 8.4 s of unbroken carrier "
                  + "at the shipped settings, and one missed decision costs most "
                  + "of that back. A bursty signal never reaches it."
        }
    }

    // From the handle and not from the link, for a reason EngineLink
    // states: setConfidenceBar emits nothing, because the bar changes
    // what the NEXT poll asks for, so confidenceBar notifies only when a
    // poll comes back different and would show a stale number here.
    //
    // The stop gets its own word rather than a number. toFixed(2) at the
    // top of the travel prints 1.00, the one value the engine refuses,
    // and a track reaches this bar after 85 consecutive detections and
    // no sooner, so what is listed is a carrier that has not stopped and
    // a band of bursty traffic reads as empty. That is worth a word
    // because an empty list is also what a dead band looks like.
    Readout {
        widest: "saturated"
        horizontalAlignment: Text.AlignLeft
        text: confidenceSlider.bar >= engineLink.maxConfidenceBar
              ? "saturated" : confidenceSlider.bar.toFixed(2)
        color: detector.enabled ? Theme.ink : Theme.inkOff
    }

    Rectangle {
        Layout.preferredWidth: 1
        Layout.preferredHeight: 14
        Layout.leftMargin: 4
        Layout.rightMargin: 4
        color: Theme.border
    }

    // THE OTHER BAR, AND THE ONE THE OLD "confidence" LABEL WAS PROMISING.
    // It filters on Detection::marginConfidence, which is how far a
    // detection stood above the detection threshold, so it hides what is
    // weak rather than what is new. The two are independent and a track
    // has to clear both, which is how "strong AND settled" becomes
    // expressible with two handles and no third call.
    //
    // IT STILL DOES NOT FILTER OUT INTERFERENCE and the label does not
    // pretend to. A strong intermodulation product stands well above the
    // noise and clears any margin bar, correctly; the front end's note in
    // the status drawer is what speaks to that.
    //
    // Remembered across launches like the bar beside it, since 2026-09-23.
    // WHAT THIS PARAGRAPH USED TO SAY: "Starts at zero and is not
    // remembered across launches, unlike the bar beside it. A margin bar
    // left high hides weak signals, which looks exactly like a quiet band".
    // The row now shows its value at all times, so a window that comes up
    // filtering weak signals says so where the operator is looking.
    Label {
        text: "stronger"
        color: detector.enabled ? Theme.inkDim : Theme.inkOff
        font.pixelSize: Theme.sizeSmall
    }

    RSlider {
        id: marginSlider

        Layout.preferredWidth: 80
        from: 0.0
        to: engineLink.maxConfidenceBar
        stepSize: 0.01

        Component.onCompleted: value = engineLink.marginBar

        // The same pin the confidence handle uses, and for the same reason:
        // the engine refuses a bar of exactly one, so a handle at full
        // travel has to arrive as the largest value below it.
        readonly property double bar: {
            if (marginSlider.value >= engineLink.maxConfidenceBar) {
                return engineLink.maxConfidenceBar
            }
            const step = marginSlider.stepSize > 0 ? marginSlider.stepSize : 0.01
            return Math.min(Math.round(marginSlider.value / step) * step,
                            engineLink.maxConfidenceBar)
        }

        onMoved: engineLink.marginBar = marginSlider.bar

        Tip {
            visible: parent.hovered
            delay: 400
            text: "Stronger than: this window only. Hides detections that stood only a little "
                  + "above the detection threshold. Below a half passes everything, because "
                  + "every published detection cleared the threshold and the margin is a half at it."
        }
    }

    // Below a half passes the whole list, because every published
    // detection cleared the detection threshold and the margin map is
    // exactly a half at it. Saying so stops the first half of the travel
    // reading as a filter that does nothing for no reason.
    Readout {
        widest: "saturated"
        horizontalAlignment: Text.AlignLeft
        text: marginSlider.bar < 0.5 ? "all" : marginSlider.bar.toFixed(2)
        color: detector.enabled ? Theme.ink : Theme.inkOff
    }

    // How many of the detector's tracks the two bars let through. Zero
    // decisions is the detector having been built by this client's first
    // poll and not having decided yet, which core/rpc/client.h is explicit
    // is not an empty band.
    Readout {
        widest: "starting"
        horizontalAlignment: Text.AlignLeft
        text: !detector.enabled ? ""
              : engineLink.detectionDecisions === 0
              ? "starting"
              : engineLink.detectionCount + "/" + engineLink.detectionTotal
        color: !detector.enabled ? Theme.inkOff
               : engineLink.detectionDecisions === 0 ? Theme.inkWarn : Theme.inkDim

        HoverHandler { id: countHover }

        Tip {
            visible: countHover.hovered
            delay: 400
            text: engineLink.detectionDecisions === 0
                  ? "The detector is built and has not decided yet. That is not an empty band."
                  : engineLink.detectionCount + " of the detector's " + engineLink.detectionTotal
                    + " tracks clear both bars and are drawn."
        }
    }
}
