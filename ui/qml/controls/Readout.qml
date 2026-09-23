// A live number that does not move anything beside it.
//
// A label whose text updates several times a second changes its width with
// every digit that comes and goes, and a layout that sizes to it moves every
// neighbour by a few pixels each time. The receiver window's divider was seen
// doing exactly that on 2026-09-22, jiggling back and forth with the level
// meter's dBFS figure.
//
// So a readout is as wide as the widest string it can print, measured once
// from `widest`, in the monospace family so the digits themselves do not
// change width either, and its text sits right-aligned inside that width the
// way a column of figures does. It can still be squeezed below that width
// when a window is too narrow for everything in the row, and elides then,
// because a minimum width here pushed the receiver window's controls off its
// right-hand edge at the window's own default size.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Label {
    id: readout

    // The widest thing this readout will ever say.
    property string widest: "-000.0 dBFS"

    Layout.preferredWidth: widthOf.advanceWidth + 2
    horizontalAlignment: Text.AlignRight
    font.family: Theme.monoFont
    font.pixelSize: Theme.sizeBody
    color: Theme.inkDim
    elide: Text.ElideLeft

    TextMetrics {
        id: widthOf
        font: readout.font
        text: readout.widest
    }
}
