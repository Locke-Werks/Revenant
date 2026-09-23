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

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

GridLayout {
    // A grid in the detections panel: one row per bar, the label, the
    // handle and the number in force lined up down three columns. It was one
    // row across the whole window.
    columns: 3
    rowSpacing: 6
    columnSpacing: 10
    visible: engineLink.connected && engineLink.spectrumEnabled

    Label {
        Layout.row: 0
        Layout.column: 0
        Layout.minimumWidth: 0
        text: "detections"
        color: Theme.inkTune
        font.pixelSize: Theme.sizeBody
        font.bold: true
        elide: Text.ElideRight
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
    // the word was wrong, so the word changed.
    //
    // The margin beside a tuned track is the number that answers the
    // question the old label implied. See detectionMargin.
    Label {
        Layout.row: 1
        Layout.column: 0
        Layout.minimumWidth: 0
        text: "held for"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }

    RSlider {
        Layout.row: 1
        Layout.column: 1
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

        // SEEDED FROM THE LINK AND NOT BOUND TO IT, on the same
        // pattern the volume slider uses and for the same reason:
        // a binding is broken by the first drag anyway, and a
        // half-live binding is worse than none. This control is
        // the only writer.
        //
        // WHAT THIS USED TO BE. A literal 0.0, with a comment
        // saying EngineLink starts the bar at zero. It no longer
        // does: the bar is remembered across launches, so a
        // literal here would put the handle at the bottom while
        // the link filtered at last night's setting, and the
        // number beside the handle is read off the handle.
        Component.onCompleted: value = engineLink.confidenceBar

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
        Layout.row: 1
        Layout.column: 2
        Layout.minimumWidth: 0
        text: (confidenceSlider.bar >= engineLink.maxConfidenceBar
               ? "saturated only"
               : confidenceSlider.bar.toFixed(2)) + "  this window"
        color: Theme.ink
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }

    // THE OTHER BAR, AND THE ONE THE OLD LABEL WAS PROMISING. It
    // filters on Detection::marginConfidence, which is how far a
    // detection stood above the detection threshold, so it hides what
    // is weak rather than what is new. The two are independent and a
    // track has to clear both, which is how "strong AND settled"
    // becomes expressible with two handles and no third call.
    //
    // IT STILL DOES NOT FILTER OUT INTERFERENCE and the label does not
    // pretend to. A strong intermodulation product stands well above
    // the noise and clears any margin bar, correctly; the front-end
    // line above is what speaks to that.
    //
    // Starts at zero and is not remembered across launches, unlike the
    // bar beside it. A margin bar left high hides weak signals, which
    // looks exactly like a quiet band, so a window coming up with one
    // set would be making a claim about a band it had not looked at.
    Label {
        Layout.row: 2
        Layout.column: 0
        Layout.minimumWidth: 0
        text: "stronger than"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }

    RSlider {
        Layout.row: 2
        Layout.column: 1
        id: marginSlider

        Layout.preferredWidth: 120
        from: 0.0
        to: engineLink.maxConfidenceBar
        stepSize: 0.01

        // The same pin the confidence handle uses, and for the same
        // reason: the engine refuses a bar of exactly one, so a handle
        // at full travel has to arrive as the largest value below it
        // rather than as one.
        readonly property double bar: {
            if (marginSlider.value >= engineLink.maxConfidenceBar) {
                return engineLink.maxConfidenceBar
            }
            const step = marginSlider.stepSize > 0 ? marginSlider.stepSize : 0.01
            return Math.min(Math.round(marginSlider.value / step) * step,
                            engineLink.maxConfidenceBar)
        }

        onMoved: engineLink.marginBar = marginSlider.bar
    }

    // Below a half passes the whole list, because every published
    // detection cleared the detection threshold and the margin map is
    // exactly a half at it. Saying so stops the first half of the
    // travel reading as a filter that does nothing for no reason.
    Label {
        Layout.row: 2
        Layout.column: 2
        Layout.minimumWidth: 0
        text: marginSlider.bar < 0.5
              ? "everything"
              : marginSlider.bar.toFixed(2) + "  this window"
        color: Theme.ink
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }

    Label {
        Layout.row: 3
        Layout.column: 0
        Layout.minimumWidth: 0
        text: "detect"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeBody
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
    RSlider {
        Layout.row: 3
        Layout.column: 1
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
        Layout.row: 3
        Layout.column: 2
        Layout.minimumWidth: 0
        text: engineLink.detectionThresholdDb.toFixed(1)
              + " dB SNR in 2500 Hz in force, engine-wide"
        color: Theme.ink
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }

    // Only when somebody else has moved it. The detector answers a
    // write on the next poll, so a gap wider than one step that is
    // still there is another client and not this one in flight.
    Label {
        Layout.row: 4
        Layout.column: 1
        Layout.columnSpan: 2
        Layout.minimumWidth: 0
        visible: engineLink.detectionDecisions > 0
                 && Math.abs(engineLink.detectionThresholdDb
                             - thresholdSlider.value) > 0.6
        text: "(this window asked for " + thresholdSlider.value.toFixed(1) + ")"
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }

    // Zero decisions is the detector having been built by this
    // client's first poll and not having decided yet, which
    // core/rpc/client.h is explicit is not an empty band.
    Label {
        Layout.row: 0
        Layout.column: 1
        Layout.columnSpan: 2
        Layout.minimumWidth: 0
        Layout.preferredWidth: 220
        horizontalAlignment: Text.AlignRight
        text: engineLink.detectionDecisions === 0
              ? "detector starting"
              : engineLink.detectionCount + " of " + engineLink.detectionTotal
                + " tracks shown"
        color: engineLink.detectionDecisions === 0 ? Theme.inkWarn : Theme.inkDim
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }
}
