// The palette, the type and the shape of a control, in one place.
//
// These were readonly properties on the window and literal pixel sizes at
// every label, which worked while the whole interface was one file and stops
// working the moment it is several: a component file cannot see the window's
// id, and a literal repeated in fifteen files is fifteen places to miss.
//
// THE HIERARCHY THIS ENCODES
//
// The spectrum and the waterfall are the loudest things in either window and
// nothing here competes with them. Receiver controls are second, and carry
// their receiver's colour so a strip, a dial and a marker on the span read as
// one thing. Everything else is chrome and is drawn to recede: dark surfaces a
// step off the background, dim ink, one accent reserved for what can be
// operated.

pragma Singleton

import QtQuick

QtObject {
    // The window's own background, near black so the colour map's dynamic
    // range has somewhere to live.
    readonly property color background: "#06080e"

    readonly property color inkDim: "#6f7b8c"
    readonly property color ink: "#c6d0dd"
    readonly property color inkWarn: "#d6a24a"
    readonly property color inkBad: "#d6624a"

    // The detection colour, and it is the one hue render/spectrum_scale.cpp
    // never produces. Kept the same on both sides so a box, its label and
    // this readout are visibly one thing.
    readonly property color inkTune: "#ff58c8"

    // A control that exists and cannot be used right now: a band button over
    // a source that cannot retune, a save with no receiver to save.
    readonly property color inkOff: "#3a4250"

    // THE ONE ACCENT. It marks what can be operated and what is being
    // operated: a focused field, a checked toggle, the active digit of a
    // dial, the filled part of a slider. Teal, because the two hues already
    // spoken for mean something else: magenta is a detection and azure is
    // the first receiver.
    readonly property color accent: "#45c4b0"
    readonly property color accentDim: "#2a7a6e"

    // Surfaces, from the window up. A panel floats over the span, and it is
    // only just translucent: at 85% the detection labels under the status
    // drawer read through its own text. A control sits on a panel and is a
    // step lighter than it.
    readonly property color panel: "#f20b0f17"
    readonly property color panelSolid: "#0b0f17"
    readonly property color control: "#141a24"
    readonly property color controlHover: "#1c2430"
    readonly property color controlDown: "#0f141c"
    readonly property color border: "#263040"

    // Behind a label drawn over the trace. See Plate.qml.
    // At 70% the trace read through the floor plates, which sit on the
    // tallest part of it; 90% leaves the label legible and still shows that
    // there is a trace behind it.
    readonly property color plate: "#e6060810"

    // RECEIVER COLOURS, one per receiver in the order they were made, and the
    // same colour on the span marker, the rack strip and the receiver's dial.
    //
    // The first is the azure the receiver marker has always been drawn in
    // (kReceiverEdge in render/spectrum_item.cpp), chosen there because the
    // colour map never produces it. The rest are spread in hue at similar
    // lightness so none of them reads as louder than another. They have NOT
    // been checked against a colour vision deficiency simulator, and the
    // engine holds one receiver per window today, so only the first is drawn.
    readonly property var receiverColours: [
        "#80c4ff", "#ffa94d", "#69db7c", "#ffd43b",
        "#b197fc", "#ff8787", "#63e6be", "#e599f7"
    ]

    // One sans for the interface and one monospace for every number an
    // operator reads as a number. Both ship with Windows 11, so nothing is
    // bundled and the installer does not change.
    readonly property string uiFont: "Segoe UI Variable Text"
    readonly property string monoFont: "Cascadia Mono"

    // Three sizes for text and one for the dials.
    readonly property int sizeTitle: 13
    readonly property int sizeBody: 12
    readonly property int sizeSmall: 11
    readonly property int sizeDial: 26

    // The shape of a control.
    readonly property int controlHeight: 26
    readonly property int radius: 4
    readonly property int gap: 8
}
