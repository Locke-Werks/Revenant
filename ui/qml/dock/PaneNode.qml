// One node of a tiled layout: a pane's frame when the node is a leaf, its
// children side by side or stacked with a border between each pair when it is
// a split. Children are this same file, loaded by URL because a QML type
// cannot name itself.
//
// Lengths come from the store's sizes(), which is where every pane's minimum
// is enforced, so nothing here clamps anything.

import QtQuick
import Revenant

Item {
    id: nodeItem

    property var host: null
    property var node: null

    readonly property bool leaf: !node || node.leaf
    readonly property bool horizontal: !leaf && node.horizontal
    readonly property real length: horizontal ? width : height

    // Re-read whenever a border moves or a minimum changes, which bumps the
    // store's revision without rebuilding the tree.
    readonly property var sizes: leaf || !host || !host.store
                                 ? []
                                 : (host.store.revision, host.store.sizes(node.path, length))

    function offset(index) {
        let at = 0
        for (let i = 0; i < index && i < sizes.length; ++i)
            at += sizes[i] + UiRules.paneHandle
        return at
    }

    Loader {
        anchors.fill: parent
        active: nodeItem.leaf && nodeItem.node !== null
        sourceComponent: PaneFrame {
            host: nodeItem.host
            pane: nodeItem.node ? nodeItem.node.pane : ""
        }
    }

    Repeater {
        model: nodeItem.leaf ? [] : nodeItem.node.children

        delegate: Loader {
            required property var modelData
            required property int index

            x: nodeItem.horizontal ? nodeItem.offset(index) : 0
            y: nodeItem.horizontal ? 0 : nodeItem.offset(index)
            width: nodeItem.horizontal ? (nodeItem.sizes[index] || 0) : nodeItem.width
            height: nodeItem.horizontal ? nodeItem.height : (nodeItem.sizes[index] || 0)

            source: Qt.resolvedUrl("PaneNode.qml")
            onLoaded: {
                item.host = Qt.binding(() => nodeItem.host)
                item.node = Qt.binding(() => modelData)
            }
        }
    }

    Repeater {
        model: nodeItem.leaf ? 0 : Math.max(0, nodeItem.node.children.length - 1)

        delegate: PaneSplitter {
            required property int index

            host: nodeItem.host
            split: nodeItem
            border: index
            horizontal: nodeItem.horizontal
            x: nodeItem.horizontal ? nodeItem.offset(index + 1) - UiRules.paneHandle : 0
            y: nodeItem.horizontal ? 0 : nodeItem.offset(index + 1) - UiRules.paneHandle
            width: nodeItem.horizontal ? UiRules.paneHandle : nodeItem.width
            height: nodeItem.horizontal ? nodeItem.height : UiRules.paneHandle
        }
    }
}
