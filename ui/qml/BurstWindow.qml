// The burst timeline: every transmission the detector saw, one lane per
// frequency, against wall clock, with what was decoded and said during it.
//
// ITS OWN WINDOW, on VrxWindow.qml's terms: a peer of the main one, so it can
// sit on a second screen through a long watch. models/burst_timeline_timeline.h
// does the bookkeeping; this file only draws the window it is asked for.
//
// THE RIGHT EDGE FOLLOWS NOW until the operator pans or zooms away from it,
// and "live" puts it back. A click tunes the focused receiver to the burst;
// a double click replays it out of the capture when the engine keeps one.

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Revenant

Window {
    id: win

    objectName: "burstWindow"
    title: "Revenant  ·  bursts"
    width: 1100
    height: 560
    minimumWidth: 640
    minimumHeight: 320
    color: Theme.background
    transientParent: null
    visible: false

    BurstTimelineModel {
        id: timeline
        link: engineLink
    }

    // The visible span, milliseconds. endMs is the right edge, which tracks
    // the model's clock while live.
    readonly property var spans: [
        { label: "1 min", ms: 60000 },
        { label: "5 min", ms: 300000 },
        { label: "15 min", ms: 900000 },
        { label: "1 h", ms: 3600000 },
        { label: "6 h", ms: 21600000 },
        { label: "24 h", ms: 86400000 }
    ]
    property double spanMs: 300000
    property bool live: true
    property double heldEndMs: 0
    readonly property double endMs: live ? timeline.nowMs : heldEndMs
    readonly property double startMs: endMs - spanMs

    readonly property int laneHeight: 22
    readonly property int labelWidth: 112

    property var shown: []
    property var hovered: null

    function toggle() {
        if (win.visible) {
            win.hide()
        } else {
            win.show()
            win.raise()
            win.requestActivate()
        }
    }

    function refresh() {
        if (!win.visible)
            return
        win.shown = timeline.bursts(win.startMs, win.endMs)
    }

    function xOf(ms) {
        return (ms - win.startMs) / win.spanMs * plot.width
    }

    function laneIndex(hz) {
        const lanes = timeline.lanes
        for (let i = 0; i < lanes.length; ++i) {
            if (lanes[i].hz === hz)
                return i
        }
        return -1
    }

    function zoomAround(factor, atMs) {
        const next = Math.max(10000, Math.min(86400000, win.spanMs * factor))
        const right = win.endMs + (atMs - win.endMs) * (1 - next / win.spanMs)
        win.heldEndMs = Math.min(right, timeline.nowMs)
        win.live = win.heldEndMs >= timeline.nowMs - 1
        win.spanMs = next
    }

    function clockText(ms) {
        return Qt.formatTime(new Date(ms), win.spanMs > 3600000 ? "HH:mm" : "HH:mm:ss")
    }

    function durationText(ms) {
        const s = ms / 1000
        if (s < 10)
            return s.toFixed(1) + " s"
        if (s < 60)
            return Math.round(s) + " s"
        return Math.floor(s / 60) + ":" + String(Math.round(s) % 60).padStart(2, "0")
    }

    onVisibleChanged: refresh()
    onStartMsChanged: refresh()
    onSpanMsChanged: refresh()

    Connections {
        target: timeline
        function onChanged() { win.refresh() }
    }

    FileDialog {
        id: saveDialog
        title: "Export the visible bursts"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "csv"
        nameFilters: ["CSV files (*.csv)"]
        onAccepted: timeline.exportCsv(win.startMs, win.endMs, selectedFile)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Label {
                text: "bursts"
                color: Theme.ink
                font.pixelSize: Theme.sizeTitle
                font.bold: true
            }

            Label {
                text: timeline.count + (timeline.count === 1 ? " burst, " : " bursts, ")
                      + timeline.lanes.length + (timeline.lanes.length === 1 ? " lane" : " lanes")
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
            }

            Item { Layout.fillWidth: true }

            Repeater {
                model: win.spans
                RButton {
                    required property var modelData
                    flat: true
                    text: modelData.label
                    checkable: true
                    checked: win.spanMs === modelData.ms
                    onClicked: win.spanMs = modelData.ms
                }
            }

            RButton {
                flat: true
                text: "live"
                checkable: true
                checked: win.live
                onClicked: {
                    win.heldEndMs = timeline.nowMs
                    win.live = !win.live
                }
            }

            RButton {
                flat: true
                text: "export csv"
                ink: Theme.inkDim
                onClicked: saveDialog.open()
            }

            RButton {
                flat: true
                text: "clear"
                ink: Theme.inkDim
                onClicked: timeline.clear()
            }
        }

        // The time axis, over the plot only.
        Item {
            Layout.fillWidth: true
            implicitHeight: 18

            Repeater {
                model: 7
                Label {
                    required property int index
                    readonly property double ms: win.startMs + win.spanMs * index / 6
                    x: win.labelWidth + (plot.width * index / 6) - (index === 6 ? width : index === 0 ? 0 : width / 2)
                    text: win.clockText(ms)
                    color: Theme.inkDim
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.panelSolid
            border.width: 1
            border.color: Theme.border
            radius: Theme.radius
            clip: true

            Label {
                anchors.centerIn: parent
                visible: timeline.count === 0
                text: engineLink.connected ? "no transmissions yet" : "no engine"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody
            }

            Flickable {
                id: lanesView
                anchors.fill: parent
                anchors.margins: 1
                contentWidth: width
                contentHeight: timeline.lanes.length * win.laneHeight
                boundsBehavior: Flickable.StopAtBounds
                interactive: false
                ScrollBar.vertical: RScrollBar {}

                // Lane labels and their stripes.
                Repeater {
                    model: timeline.lanes
                    Item {
                        required property var modelData
                        required property int index
                        y: index * win.laneHeight
                        width: lanesView.width
                        height: win.laneHeight

                        Rectangle {
                            anchors.fill: parent
                            color: index % 2 === 0 ? "transparent" : Theme.control
                            opacity: 0.5
                        }

                        Label {
                            x: 6
                            width: win.labelWidth - 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            color: Theme.ink
                            elide: Text.ElideRight
                            font.family: Theme.monoFont
                            font.pixelSize: Theme.sizeSmall
                        }
                    }
                }

                Item {
                    id: plot
                    x: win.labelWidth
                    width: lanesView.width - win.labelWidth
                    height: Math.max(lanesView.height, lanesView.contentHeight)

                    Rectangle {
                        width: 1
                        height: parent.height
                        color: Theme.border
                    }

                    Repeater {
                        model: win.shown
                        Rectangle {
                            id: bar
                            required property var modelData
                            readonly property int lane: win.laneIndex(modelData.laneHz)
                            readonly property double x0: Math.max(0, win.xOf(modelData.startMs))
                            readonly property double x1: Math.min(plot.width, win.xOf(modelData.endMs))
                            visible: lane >= 0
                            x: x0
                            y: lane * win.laneHeight + 4
                            width: Math.max(2, x1 - x0)
                            height: win.laneHeight - 8
                            radius: 2
                            color: modelData.open ? Theme.accent : Theme.accentDim
                            border.width: win.hovered !== null && win.hovered.id === modelData.id ? 1 : 0
                            border.color: Theme.ink

                            Label {
                                anchors.left: parent.left
                                anchors.leftMargin: 3
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 6
                                visible: parent.width > 40
                                text: modelData.mode
                                color: Theme.background
                                elide: Text.ElideRight
                                font.pixelSize: Theme.sizeSmall - 1
                            }

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton
                                onEntered: win.hovered = bar.modelData
                                onExited: {
                                    if (win.hovered !== null && win.hovered.id === bar.modelData.id)
                                        win.hovered = null
                                }
                                onClicked: timeline.tune(bar.modelData.id)
                                onDoubleClicked: timeline.openCapture(bar.modelData.id)
                            }
                        }
                    }

                    // Pan by drag, zoom by wheel around the pointer.
                    MouseArea {
                        anchors.fill: parent
                        z: -1
                        property double pressX: 0
                        property double pressEnd: 0
                        onPressed: (mouse) => {
                            pressX = mouse.x
                            pressEnd = win.endMs
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed)
                                return
                            const shift = (mouse.x - pressX) / plot.width * win.spanMs
                            win.heldEndMs = Math.min(pressEnd - shift, timeline.nowMs)
                            win.live = false
                        }
                        onWheel: (wheel) => {
                            if (wheel.modifiers & Qt.ShiftModifier) {
                                lanesView.contentY = Math.max(0, Math.min(
                                    lanesView.contentHeight - lanesView.height,
                                    lanesView.contentY - wheel.angleDelta.y / 2))
                                return
                            }
                            const at = win.startMs + wheel.x / plot.width * win.spanMs
                            win.zoomAround(wheel.angleDelta.y > 0 ? 0.8 : 1.25, at)
                        }
                    }
                }
            }

            // The hover card.
            Rectangle {
                visible: win.hovered !== null
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 8
                width: Math.min(parent.width - 16, 380)
                height: card.implicitHeight + 16
                radius: Theme.radius + 2
                color: Theme.panel
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    id: card
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 8
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: win.hovered === null ? ""
                              : win.hovered.frequency + "   " + (win.hovered.mode || "unlabelled")
                        color: Theme.ink
                        font.bold: true
                        font.pixelSize: Theme.sizeBody
                    }
                    Label {
                        Layout.fillWidth: true
                        text: win.hovered === null ? ""
                              : win.clockText(win.hovered.startMs) + "  for "
                                + win.durationText(win.hovered.endMs - win.hovered.startMs)
                                + (win.hovered.open ? "  (on air)" : "")
                                + (win.hovered.snr > 0 ? "   " + win.hovered.snr.toFixed(1) + " dB" : "")
                        color: Theme.inkDim
                        font.pixelSize: Theme.sizeSmall
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: win.hovered === null ? "" : win.hovered.ids
                        color: Theme.inkTune
                        wrapMode: Text.Wrap
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: win.hovered === null ? "" : win.hovered.decoded
                        color: Theme.ink
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        font.pixelSize: Theme.sizeSmall
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: win.hovered === null ? "" : win.hovered.transcript
                        color: Theme.ink
                        font.italic: true
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        font.pixelSize: Theme.sizeSmall
                    }
                    Label {
                        Layout.fillWidth: true
                        text: "click to tune" + (timeline.captureAvailable ? ", double click to replay" : "")
                        color: Theme.inkOff
                        font.pixelSize: Theme.sizeSmall
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: timeline.note
            color: Theme.inkDim
            elide: Text.ElideRight
            font.pixelSize: Theme.sizeSmall
        }
    }
}
