// The contrast pivot as a vertical slider on the waterfall's right edge, the
// owner's request of 2026-10-11. Up is a higher pivot, the same direction the
// pivot moves on the colour map. Dragging sets ScaleSettings.waterfallPivot as
// it goes, so the waterfall recolours under the pointer. The curve pops up
// beside it while the pointer is over the strip or dragging it.
//
// Dim below 100% contrast, where the pivot does nothing, and still usable, so
// it can be set ahead of turning the contrast up.

import QtQuick
import Revenant

Item {
    id: strip

    readonly property real minimum: 0.05
    readonly property real maximum: 0.95
    readonly property bool active: ScaleSettings.waterfallContrast > 1.0
    readonly property bool showing: area.containsMouse || area.pressed

    width: 18

    function fractionAt(y) {
        const span = track.height
        const t = 1.0 - Math.max(0, Math.min(span, y - track.y)) / span
        return minimum + t * (maximum - minimum)
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.panel
        opacity: strip.showing ? 0.9 : 0.5
    }

    Rectangle {
        id: track

        anchors.horizontalCenter: parent.horizontalCenter
        y: 10
        width: 4
        height: parent.height - 20
        radius: 2
        color: Theme.border

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            radius: 2
            height: parent.height * (ScaleSettings.waterfallPivot - strip.minimum)
                    / (strip.maximum - strip.minimum)
            color: strip.active ? Theme.accent : Theme.inkOff
        }
    }

    Rectangle {
        id: handle

        anchors.horizontalCenter: parent.horizontalCenter
        width: 12
        height: 12
        radius: 6
        y: track.y + track.height * (1.0 - (ScaleSettings.waterfallPivot - strip.minimum)
                                     / (strip.maximum - strip.minimum)) - height / 2
        color: area.pressed ? Theme.accent : Theme.panelSolid
        border.width: 2
        border.color: !strip.active ? Theme.inkOff
                      : strip.showing ? Theme.accent : Theme.ink
    }

    MouseArea {
        id: area

        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.SizeVerCursor
        onPressed: (mouse) => ScaleSettings.waterfallPivot = strip.fractionAt(mouse.y)
        onPositionChanged: (mouse) => {
            if (pressed)
                ScaleSettings.waterfallPivot = strip.fractionAt(mouse.y)
        }
        onWheel: (wheel) => {
            const step = wheel.angleDelta.y > 0 ? 0.01 : -0.01
            ScaleSettings.waterfallPivot = Math.max(strip.minimum,
                                                    Math.min(strip.maximum,
                                                             ScaleSettings.waterfallPivot + step))
        }
    }

    // The popup: the curve, with the pivot and contrast written under it,
    // to the strip's left and following the handle up and down.
    Rectangle {
        visible: strip.showing
        width: 252
        height: 150
        x: -width - 6
        y: Math.max(0, Math.min(strip.height - height, handle.y + handle.height / 2 - height / 2))
        radius: Theme.radius
        color: Theme.panel
        border.width: 1
        border.color: Theme.border

        ContrastCurve {
            id: popupCurve
            x: 6
            y: 6
            width: parent.width - 12
            height: parent.height - 30
        }

        Text {
            x: 8
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 5
            text: "pivot " + Math.round(ScaleSettings.waterfallPivot * 100) + "%  ·  contrast "
                  + Math.round(ScaleSettings.waterfallContrast * 100) + "%"
                  + (strip.active ? "" : "  ·  acts above 100%")
            color: strip.active ? Theme.ink : Theme.inkDim
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeSmall
        }

        onVisibleChanged: if (visible) popupCurve.requestPaint()
    }
}
