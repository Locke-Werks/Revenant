// The front end: whether the source is keeping up, where it is tuned, what
// refused a tune, and what the front end itself is doing.
//
// Four rows of the window's column, kept in the order they had there. Each
// row's own reason for being where it is stays with the row below.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    Layout.fillWidth: true
    spacing: 6

    // Shown whenever any of the four is. The tune row and the two notes
    // are all gated on a connection; a refused tune is the one sentence
    // that can still be on screen after the connection has gone.
    visible: engineLink.connected || engineLink.tuneFault.length > 0

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
    // So the line sits under the frame rate, which is the other
    // measurement of how fast the engine is producing, and the two
    // read together: rows/s is what is reaching the display and this
    // is whether the radio is supplying it.
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
            color: engineLink.sourceCanRetune ? Theme.inkTune : Theme.inkDim
            font.pixelSize: Theme.sizeTitle
            font.bold: true
        }

        // THE DIAL, BOUND TO WHERE THE RADIO IS. The text box it replaced
        // was seeded from sourceCenterHz and deliberately not bound, because
        // a binding rewrote the box under an operator halfway through
        // typing every time a retune landed. The dial has no such problem:
        // the digits are a reading and typing happens in an editor of its
        // own that opens empty, so the reading can follow the radio.
        FrequencyDial {
            id: tuneDial

            enabled: engineLink.sourceCanRetune
            value: engineLink.sourceCenterHz
            low: engineLink.sourceTuneLowHz
            high: engineLink.sourceTuneHighHz
            placeholder: "95.1 or 95.1M"
            onStepped: (hz) => engineLink.tuneSourceHz(hz)
            onTyped: (text) => engineLink.tuneSource(text)
        }

        // What the text resolves to, before it is sent. Red when it
        // does not resolve at all, which is the one state where
        // pressing return does nothing useful.
        Label {
            Layout.minimumWidth: 0
            visible: tuneDial.editing && tuneDial.editText.length > 0
            text: engineLink.tuneTextValid(tuneDial.editText)
                  ? "→ " + engineLink.previewTune(tuneDial.editText)
                  : "not a frequency"
            color: engineLink.tuneTextValid(tuneDial.editText)
                   ? Theme.inkDim : Theme.inkBad
            font.pixelSize: Theme.sizeBody
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
                color: engineLink.sourceCanRetune ? Theme.inkDim : Theme.inkOff
                font.pixelSize: Theme.sizeBody

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -3
                    enabled: engineLink.sourceCanRetune
                    cursorShape: Qt.PointingHandCursor
                    onClicked: engineLink.tuneSourceHz(parent.modelData.hz)
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
            color: Theme.inkWarn
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideRight
        }

        // THE WHEEL IS THE OTHER WAY TO TUNE AND NOTHING ON SCREEN SHOWS
        // IT. A box and a row of band buttons look like the whole of the
        // surface, so an operator who wants to sweep types a frequency,
        // looks, types another. Scrolling over either span display walks
        // the front end along instead, a twentieth of the span a notch,
        // and a gesture with no mark anywhere in the window is a feature
        // nobody finds. Beside the box it names rather than in a help
        // pane, because this row is where somebody is already tuning.
        //
        // Only while the source will take it, so it is not offered over a
        // recording, where the line below explains the dead box instead.
        Label {
            Layout.minimumWidth: 0
            visible: engineLink.sourceCanRetune
            text: "or scroll over the spectrum"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
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
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideRight
        }
    }

    // The engine refused a frequency, in its own words, or this window
    // refused the text before sending it.
    NoticeRow {
        visible: engineLink.tuneFault.length > 0
        heading: "tune refused:"
        headingColor: Theme.inkWarn
        body: engineLink.tuneFault
    }

    // What the front end is doing, under the controls that point it.
    //
    // HERE AND NOT BESIDE A GAIN CONTROL, because this window has no
    // gain control: gain is set in the source URI the engine was
    // started with and nothing in the client can change it. So the line
    // sits at the bottom of the front-end block, under the tune box,
    // which is the only part of this window that is about the radio
    // rather than about the graph, and its own text names gain as the
    // thing to change. When a gain control lands, this row moves to sit
    // under it.
    //
    // The wording is in models/front_end_note.h with its own cases in
    // ui/tests, and the measurement is core/detect/front_end.h. This
    // file chooses the colour and nothing else.
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
}
