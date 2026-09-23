// The palette and the type sizes, in one place.
//
// These were readonly properties on the window and literal pixel sizes at
// every label, which worked while the whole interface was one file and stops
// working the moment it is several: a component file cannot see the window's
// id, and a literal repeated in fifteen files is fifteen places to miss.
//
// Nothing here is new. Every value is the one Main.qml carried when it was
// split, so the split changes where a colour is written and not what it is.

pragma Singleton

import QtQuick

QtObject {
    // The window's own background.
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

    // Behind a label drawn over the trace. See Plate.qml.
    readonly property color plate: "#b3060810"

    // Three sizes and no more. A section heading, the body of a row, and the
    // small print under it.
    readonly property int sizeTitle: 13
    readonly property int sizeBody: 12
    readonly property int sizeSmall: 11
}
