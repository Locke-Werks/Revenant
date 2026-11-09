// The border between two panes of a split, dragged to trade length between
// them. The store refuses to take either below its minimum, so the border
// stops where one of them would be cut. A double click shares the split
// evenly between all its panes.

import QtQuick
import Revenant

Rectangle {
    id: splitter

    property var host: null
    property Item split: null
    property int border: 0
    property bool horizontal: false

    color: mouse.pressed ? Theme.accentDim
           : mouse.containsMouse ? Theme.border
           : Theme.panelSolid

    // A grip in the middle, so the border reads as something that moves
    // rather than as a gap between two displays.
    Rectangle {
        anchors.centerIn: parent
        width: splitter.horizontal ? 1 : Math.min(36, splitter.width)
        height: splitter.horizontal ? Math.min(36, splitter.height) : 1
        color: mouse.pressed ? Theme.accent : Theme.inkOff
    }

    MouseArea {
        id: mouse

        property real last: 0

        // A little wider than the border it sits on, so it is easy to catch.
        anchors.fill: parent
        anchors.margins: -2
        hoverEnabled: true
        preventStealing: true
        cursorShape: splitter.horizontal ? Qt.SplitHCursor : Qt.SplitVCursor

        function along(mouseX, mouseY) {
            const point = mapToItem(splitter.split, mouseX, mouseY)
            return splitter.horizontal ? point.x : point.y
        }

        onPressed: mouseEvent => last = along(mouseEvent.x, mouseEvent.y)
        onPositionChanged: mouseEvent => {
            if (!pressed)
                return
            const now = along(mouseEvent.x, mouseEvent.y)
            splitter.host.store.drag(splitter.split.node.path, splitter.border,
                                     now - last, splitter.split.length)
            last = now
        }
        onReleased: splitter.host.store.commit()
        onDoubleClicked: splitter.host.store.equalise(splitter.split.node.path)
    }
}
