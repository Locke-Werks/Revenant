// One pane: a title strip over the display the host hands in for it.
//
// The strip is what the operator drags to move the pane. Dropped on another
// pane of the same host, it lands against whichever edge of that pane the
// pointer is nearest, and the half it will take is shaded while the pointer
// is there. The display itself never moves under the pointer and is never
// drawn over: the shading is the only thing on top, and only mid-drag.

import QtQuick
import QtQuick.Controls
import Revenant

Item {
    id: frame

    property var host: null
    property string pane: ""

    readonly property Item content: host && host.contents ? (host.contents[pane] || null) : null

    function attach() {
        if (!content)
            return
        content.parent = holder
        content.visible = true
    }

    onContentChanged: attach()
    Component.onCompleted: attach()

    Rectangle {
        id: strip

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: frame.host ? frame.host.stripFor(frame.pane) : 0
        visible: height > 0
        color: grip.containsMouse || grip.drag.active ? Theme.border : Theme.panelSolid

        // Six dots, the usual mark for "this drags".
        Grid {
            id: dots
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            columns: 3
            spacing: 2
            Repeater {
                model: 6
                Rectangle {
                    width: 2
                    height: 2
                    color: Theme.inkOff
                }
            }
        }

        Label {
            anchors.left: dots.right
            anchors.leftMargin: 6
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: frame.host ? (frame.host.titles[frame.pane] || frame.pane) : frame.pane
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody - 1
            elide: Text.ElideRight
        }

        MouseArea {
            id: grip

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            drag.target: frame.host ? frame.host.marker : null
            drag.threshold: 6

            onPressed: mouseEvent => {
                const marker = frame.host.marker
                const at = mapToItem(frame.host, mouseEvent.x, mouseEvent.y)
                marker.x = at.x - marker.width / 2
                marker.y = at.y - marker.height / 2
            }
            drag.onActiveChanged: {
                if (drag.active)
                    frame.host.dragging = frame.pane
            }
            onReleased: {
                const marker = frame.host.marker
                if (frame.host.dragging !== "" && marker.Drag.target)
                    marker.Drag.drop()
                frame.host.dragging = ""
            }
            onCanceled: frame.host.dragging = ""
        }
    }

    Item {
        id: holder

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: strip.bottom
        anchors.bottom: parent.bottom
    }

    DropArea {
        id: drop

        property string edge: ""

        anchors.fill: parent
        keys: frame.host ? [frame.host.dragKey] : []
        enabled: frame.host && frame.host.dragging !== "" && frame.host.dragging !== frame.pane

        // Whichever edge the pointer is proportionally nearest.
        function nearest(x, y) {
            const fx = x / Math.max(1, width)
            const fy = y / Math.max(1, height)
            const sides = [["left", fx], ["right", 1 - fx], ["top", fy], ["bottom", 1 - fy]]
            sides.sort((a, b) => a[1] - b[1])
            return sides[0][0]
        }

        onEntered: dragEvent => edge = nearest(dragEvent.x, dragEvent.y)
        onPositionChanged: dragEvent => edge = nearest(dragEvent.x, dragEvent.y)
        onExited: edge = ""
        // Later rather than now: the move rebuilds the frames, this one and
        // the one whose strip is still handling the release among them.
        onDropped: dropEvent => {
            const store = frame.host.store
            const moved = frame.host.dragging
            const target = frame.pane
            const side = edge
            edge = ""
            Qt.callLater(() => store.move(moved, target, side))
        }
    }

    // The half the moved pane will take.
    Rectangle {
        visible: drop.containsDrag && drop.edge !== ""
        z: 10
        x: drop.edge === "right" ? parent.width / 2 : 0
        y: drop.edge === "bottom" ? parent.height / 2 : 0
        width: drop.edge === "left" || drop.edge === "right" ? parent.width / 2 : parent.width
        height: drop.edge === "top" || drop.edge === "bottom" ? parent.height / 2 : parent.height
        color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.18)
        border.width: 2
        border.color: Theme.accent
    }
}
