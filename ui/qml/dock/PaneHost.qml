// One tiled layout: the tree in a PaneLayoutStore (models/pane_layout_store.h)
// drawn as frames and the borders between them.
//
// The displays themselves are made once by whoever owns this host and handed
// in through contents, keyed by pane name. A frame re-parents its display
// into itself, so moving a pane moves the display rather than rebuilding it:
// the waterfall keeps its history and the rack its scroll position.
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

    // Pane name to the Item drawn in it.
    property var contents: ({})

    // Pane name to the title on its strip.
    property var titles: ({})

    // Whether panes have a title strip and can be dragged to a new place.
    property bool movable: true

    // Panes drawn without a title strip, which then cannot be picked up but
    // can still have another pane dropped against them. The receiver dock is
    // one: its own three panes each have a strip, and a fourth over them said
    // nothing they did not.
    property var stripless: []

    // The title strip's height, which callers add to a pane's minimum.
    readonly property real stripHeight: movable ? 18 : 0

    function stripFor(pane) {
        return stripless.indexOf(pane) >= 0 ? 0 : stripHeight
    }

    // Kept apart per host, so a pane dragged out of one host is not offered
    // to another's drop zones.
    readonly property string dragKey: "pane-" + Math.random().toString(36).slice(2)

    // The pane being dragged, empty when none is.
    property string dragging: ""

    readonly property alias marker: marker

    PaneNode {
        anchors.fill: parent
        host: host
        node: host.store ? host.store.tree : null
    }

    Rectangle {
        id: marker

        property string pane: ""

        width: Math.min(160, label.implicitWidth + 20)
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
            text: host.titles[host.dragging] || host.dragging
            color: Theme.ink
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideRight
            width: Math.min(implicitWidth, 140)
        }
    }
}
