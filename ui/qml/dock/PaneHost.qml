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

    // Shift is held while dragging: a drop goes against the whole area the
    // hovered pane sits in rather than the pane alone.
    property bool dragShift: false

    // Where an area drop will land, while one is aimed: the view path of the
    // area ("" is the whole window) and the edge. PaneNode shades it.
    property string areaPath: "-"
    property string areaEdge: ""

    function aimArea(path, edge) {
        areaPath = path
        areaEdge = edge
    }

    function dropOnArea(path, edge) {
        const st = store
        const moved = dragging
        aimArea("-", "")
        // Later: the move rebuilds the frames, the dragged one among them.
        Qt.callLater(() => st.moveBeside(moved, path, edge))
    }

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

    // The window's own edges: a pane dropped on one goes down that whole
    // side, full height or full width. Only there while a pane is dragged.
    Repeater {
        model: ["left", "right", "top", "bottom"]

        DropArea {
            required property string modelData

            readonly property real band: 28
            z: 500
            enabled: host.dragging !== ""
            keys: [host.dragKey]
            x: modelData === "right" ? host.width - band : 0
            y: modelData === "bottom" ? host.height - band : 0
            width: modelData === "left" || modelData === "right" ? band : host.width
            height: modelData === "top" || modelData === "bottom" ? band : host.height

            onEntered: host.aimArea("", modelData)
            onExited: if (host.areaPath === "" && host.areaEdge === modelData) host.aimArea("-", "")
            onDropped: host.dropOnArea("", modelData)

            Rectangle {
                anchors.fill: parent
                visible: host.dragging !== ""
                color: parent.containsDrag ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.35)
                                           : Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.08)
            }
        }
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
