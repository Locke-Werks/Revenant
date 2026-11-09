// One pane: a title strip over the content the host hands in for it.
//
// THE STRIP. Dragged, it moves the pane: dropped on another pane of the same
// host it lands against whichever edge of that pane the pointer is nearest,
// and the half it will take is shaded while the pointer is there. At its right
// end the pane's own scale, minus, percentage and plus, and the button that
// collapses the pane to the strip alone. Ctrl and the wheel over the strip
// scale too; a double click on the title collapses or opens the pane.
//
// COLLAPSED, a pane is its strip when it is above or below its neighbours,
// and a narrow bar with its title on its side when it is beside them.
//
// SCALED, the content is laid out at the pane's size divided by its scale and
// drawn magnified or reduced to fit, so a pane at 80% holds a fifth more and
// its minimum is a fifth smaller.
//
// Nothing here draws outside the frame: the content is clipped to it. The
// minimums in models/pane_layout.h are what keep that from cutting anything,
// so a clip that bites is a minimum that is wrong.

import QtQuick
import QtQuick.Controls
import Revenant

Item {
    id: frame

    property var host: null
    property string pane: ""

    // This pane's place in the drawn tree, and the area it sits in, which a
    // drop with Shift held goes against.
    property string path: ""
    readonly property string areaPath: {
        const at = path.lastIndexOf(".")
        return path === "" ? "" : (at < 0 ? "" : path.slice(0, at))
    }

    readonly property var store: host ? host.store : null
    readonly property Item content: host && host.contents ? (host.contents[pane] || null) : null

    readonly property bool collapsed: store !== null && (store.revision, store.collapsed.indexOf(pane) >= 0)
    readonly property real paneScale: store !== null && pane !== ""
                                      ? (store.revision, store.scales[pane] || 1) : 1

    // Collapsed beside its neighbours rather than above or below them.
    readonly property bool bar: collapsed && width < height

    readonly property real strip: store ? store.strip : 20

    component StripButton: Label {
        id: stripButton
        signal clicked()
        width: Math.max(16, implicitWidth + 8)
        height: frame.strip
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: buttonArea.containsMouse ? Theme.ink : Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        MouseArea {
            id: buttonArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: stripButton.clicked()
        }
    }

    function attach() {
        if (!content)
            return
        content.parent = scaler
        content.visible = true
        report()
    }

    // On the next tick rather than now. A minimum reported in the middle of a
    // layout pass re-lays out every pane, which resizes this one, which can
    // change what its content needs, all inside one binding evaluation: a
    // loop Qt breaks only after the frame is lost.
    function report() {
        if (!reportQueued) {
            reportQueued = true
            Qt.callLater(sendReport)
        }
    }

    property bool reportQueued: false

    function sendReport() {
        reportQueued = false
        if (content && store && pane !== "")
            store.setMinimum(pane, content.needWidth, content.needHeight)
    }

    function stepScale(steps) {
        store.setScale(pane, paneScale + 0.05 * steps)
    }

    onContentChanged: attach()
    Component.onCompleted: attach()

    Connections {
        target: frame.content
        ignoreUnknownSignals: true
        function onNeedWidthChanged() { frame.report() }
        function onNeedHeightChanged() { frame.report() }
    }

    // ------------------------------------------------------------------
    // The strip
    // ------------------------------------------------------------------
    Rectangle {
        id: stripBox

        anchors.left: parent.left
        anchors.top: parent.top
        width: frame.bar ? parent.width : parent.width
        height: frame.bar ? parent.height : frame.strip
        visible: frame.pane !== ""
        color: grip.containsMouse || grip.drag.active ? Theme.border : Theme.panelSolid

        // Six dots, the usual mark for "this drags".
        Grid {
            id: dots
            x: frame.bar ? (parent.width - width) / 2 : 6
            y: frame.bar ? 6 : (parent.height - height) / 2
            columns: frame.bar ? 2 : 3
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
            id: title

            text: frame.host ? frame.host.titleOf(frame.pane) : frame.pane
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            rotation: frame.bar ? 90 : 0
            transformOrigin: Item.TopLeft
            x: frame.bar ? (parent.width + height) / 2 : dots.x + dots.width + 6
            y: frame.bar ? dots.y + dots.height + 8 : (parent.height - height) / 2
        }

        MouseArea {
            id: grip

            anchors.left: parent.left
            anchors.right: frame.bar ? parent.right : controls.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
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
            onPositionChanged: mouseEvent => {
                if (frame.host)
                    frame.host.dragShift = (mouseEvent.modifiers & Qt.ShiftModifier) !== 0
            }
            onReleased: {
                const marker = frame.host.marker
                if (frame.host.dragging !== "" && marker.Drag.target)
                    marker.Drag.drop()
                frame.host.dragging = ""
            }
            onCanceled: {
                frame.host.dragging = ""
                frame.host.aimArea("-", "")
            }
            onDoubleClicked: frame.store.toggleCollapsed(frame.pane)
            onWheel: wheelEvent => {
                if (!(wheelEvent.modifiers & Qt.ControlModifier) || frame.bar) {
                    wheelEvent.accepted = false
                    return
                }
                frame.stepScale(wheelEvent.angleDelta.y > 0 ? 1 : -1)
            }
        }

        Row {
            id: controls

            anchors.right: parent.right
            anchors.rightMargin: 2
            anchors.verticalCenter: frame.bar ? undefined : parent.verticalCenter
            anchors.bottom: frame.bar ? parent.bottom : undefined
            spacing: 0
            visible: frame.pane !== ""

            StripButton {
                visible: !frame.collapsed
                text: "−"
                onClicked: frame.stepScale(-1)
            }
            StripButton {
                visible: !frame.collapsed
                text: Math.round(frame.paneScale * 100) + "%"
                // The widest it gets, so the buttons beside it do not move.
                width: 40
                onClicked: frame.store.setScale(frame.pane, 1)
            }
            StripButton {
                visible: !frame.collapsed
                text: "+"
                onClicked: frame.stepScale(1)
            }
            StripButton {
                text: frame.collapsed ? (frame.bar ? "▸" : "▾") : "▴"
                onClicked: frame.store.toggleCollapsed(frame.pane)
            }
        }
    }

    // ------------------------------------------------------------------
    // The content
    // ------------------------------------------------------------------
    Item {
        id: holder

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: stripBox.bottom
        anchors.bottom: parent.bottom
        visible: !frame.collapsed
        clip: true

        Item {
            id: scaler
            width: holder.width / frame.paneScale
            height: holder.height / frame.paneScale
            scale: frame.paneScale
            transformOrigin: Item.TopLeft
        }
    }

    DropArea {
        id: drop

        property string edge: ""

        anchors.fill: parent
        keys: frame.host ? [frame.host.dragKey] : []
        enabled: frame.host !== null && frame.host.dragging !== "" && frame.host.dragging !== frame.pane

        // Whichever edge the pointer is proportionally nearest.
        function nearest(x, y) {
            const fx = x / Math.max(1, width)
            const fy = y / Math.max(1, height)
            const sides = [["left", fx], ["right", 1 - fx], ["top", fy], ["bottom", 1 - fy]]
            sides.sort((a, b) => a[1] - b[1])
            return sides[0][0]
        }

        readonly property bool toArea: frame.host !== null && frame.host.dragShift

        function aim(x, y) {
            edge = nearest(x, y)
            if (toArea)
                frame.host.aimArea(frame.areaPath, edge)
            else if (frame.host.areaPath !== "-")
                frame.host.aimArea("-", "")
        }

        onEntered: dragEvent => aim(dragEvent.x, dragEvent.y)
        onPositionChanged: dragEvent => aim(dragEvent.x, dragEvent.y)
        onToAreaChanged: if (containsDrag) aim(drag.x, drag.y)
        onExited: {
            edge = ""
            if (toArea)
                frame.host.aimArea("-", "")
        }

        // Later rather than now: the move rebuilds the frames, this one and
        // the one whose strip is still handling the release among them.
        onDropped: dropEvent => {
            if (toArea) {
                const side = edge
                edge = ""
                frame.host.dropOnArea(frame.areaPath, side)
                return
            }
            const store = frame.store
            const moved = frame.host.dragging
            const target = frame.pane
            const side = edge
            edge = ""
            Qt.callLater(() => store.move(moved, target, side))
        }
    }

    // The half the moved pane will take.
    Rectangle {
        visible: drop.containsDrag && drop.edge !== "" && !drop.toArea
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
