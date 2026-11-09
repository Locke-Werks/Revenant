// One tiled layout: the tree in a PaneLayoutStore (models/pane_layout_store.h)
// drawn as frames and the borders between them.
//
// The contents themselves are made once by whoever owns this host and handed
// in through contents, a map from pane name to PaneContent. A frame
// re-parents its content into itself, so moving a pane moves the display
// rather than rebuilding it: the waterfall keeps its history and the rack its
// scroll position.
//
// A pane is moved by dragging its title strip onto an edge of another pane in
// the same host. The marker that follows the pointer while dragging is the
// one thing drawn over the panes, and only while the button is held.

import QtQuick
import QtQuick.Controls
import Revenant

Item {
    id: host

    // The PaneLayoutStore this host draws.
    property var store: null

    // Pane name to its PaneContent.
    property var contents: ({})

    // Kept apart per host, so a pane dragged out of one host is not offered
    // to another's drop zones.
    readonly property string dragKey: "pane-" + Math.random().toString(36).slice(2)

    // The pane being dragged, empty when none is.
    property string dragging: ""

    readonly property alias marker: marker

    function titleOf(pane) {
        const content = contents[pane]
        return content && content.title ? content.title : pane
    }

    PaneNode {
        anchors.fill: parent
        host: host
        node: host.store ? host.store.tree : null
    }

    Rectangle {
        id: marker

        width: Math.min(180, label.implicitWidth + 20)
        height: 24
        radius: Theme.radius
        visible: host.dragging !== ""
        z: 1000
        color: Theme.panel
        border.width: 1
        border.color: Theme.accent

        Drag.active: host.dragging !== ""
        Drag.keys: [host.dragKey]
        Drag.hotSpot.x: width / 2
        Drag.hotSpot.y: height / 2

        Label {
            id: label
            anchors.centerIn: parent
            text: host.titleOf(host.dragging)
            color: Theme.ink
            font.pixelSize: Theme.sizeBody
        }
    }
}
