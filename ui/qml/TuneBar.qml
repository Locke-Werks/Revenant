// Tuning the front end: the dial, the bands, and what the device did with
// the last tune. It sits at the left of the top bar, which is the one place
// in the window that is about the radio rather than about the picture.
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

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

RowLayout {
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

    // The bands somebody actually reaches for, as shortcuts, and the rest
    // in a menu beside them. Each one is the CENTRE the front end is put
    // at, not the edge of the allocation: the span the engine captures is
    // centred there and reaches half a source rate either side. The table
    // and where its edges came from are models/band_plan.h, and 462.5625
    // MHz, the standing real-radio test band from docs/, is one of the
    // shortcuts there as it was here.
    BandPicker {}


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
