// A scroll bar drawn from the Theme: a thin handle in the border's colour that
// brightens under the pointer, over no track at all. The Basic style's grey
// bar and track read as a light stripe down the edge of every dark list.

import QtQuick
import QtQuick.Controls
import Revenant

ScrollBar {
    id: bar

    policy: ScrollBar.AsNeeded
    padding: 2

    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 6
        radius: 3
        color: bar.pressed ? Theme.inkDim
               : bar.hovered ? Qt.lighter(Theme.border, 1.6)
               : Theme.border
        opacity: bar.policy === ScrollBar.AlwaysOn || bar.size < 1.0 ? 1.0 : 0.0
    }

    background: Item {}
}
