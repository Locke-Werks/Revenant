// A panel that opens over the span from a button in the top bar, and closes
// on Escape or a click anywhere else.
//
// NOT MODAL. Nothing behind it is dimmed or blocked, because what is behind
// it is the spectrum an operator is choosing a radio or a threshold while
// watching, and taking that away is the reason the device picker was an
// inline panel rather than a dialog to begin with.
//
// Anchored under the control that opened it and right-aligned to it, so a
// popover from the right-hand end of the bar opens leftwards into the window
// rather than off its edge.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Popup {
    id: popover

    default property alias content: column.data

    // The caller sets parent to the control the popover hangs from, which is
    // what places it in that control's coordinates.
    x: parent ? Math.min(0, parent.width - width) : 0
    y: parent ? parent.height + 6 : 0
    padding: 12
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

    background: Rectangle {
        radius: Theme.radius + 2
        color: Theme.panel
        border.width: 1
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        id: column
        spacing: 8
    }
}
