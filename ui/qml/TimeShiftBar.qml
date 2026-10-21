// The time-shift scrub bar under the waterfall: live at the right, the oldest
// sample the engine still holds at the left.
//
// Released off live, the span switches to a replay of that instant and the
// receiver in the pane moves onto it, so a click on the span tunes a signal
// that has already ended exactly as it would a live one. "live" brings both
// back. While dragging, the waterfall slides back through rows it kept
// itself, which costs the engine nothing; the replay starts on release,
// because each start is a second graph on the engine and a drag would build
// dozens. models/timeshift_scrub.h has the arithmetic, timeshift_link.cpp the
// order a transition happens in.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

RowLayout {
    id: bar

    visible: engineLink.connected && engineLink.timeShiftCapacitySeconds > 0
    spacing: 8

    readonly property bool replaying: engineLink.timeShiftReplaying

    // The span's waterfall, which previews the scrub.
    property var previewTarget: null
    property bool settling: false

    function endPreview() {
        bar.settling = false
        settleLimit.stop()
        if (bar.previewTarget)
            bar.previewTarget.scrubSecondsAgo = 0
    }

    // An engine that never answers still lets the preview go.
    Timer {
        id: settleLimit
        interval: 3000
        onTriggered: bar.endPreview()
    }

    Label {
        text: "time-shift"
        color: bar.replaying ? Theme.accent : Theme.inkDim
        font.family: Theme.uiFont
        font.pixelSize: Theme.sizeSmall
        Layout.leftMargin: 8
    }

    RSlider {
        id: scrub
        Layout.fillWidth: true
        from: 0
        to: 1
        enabled: engineLink.timeShiftAvailable
        tint: bar.replaying ? Theme.accent : Theme.inkDim

        // Follows the replay while nobody holds it, and holds still from a
        // release until the engine answers, so the handle does not snap
        // back to live under the pointer while the replay starts.
        value: engineLink.timeShiftPosition()
        Connections {
            target: engineLink
            function onTimeShiftChanged() {
                if (bar.settling && (engineLink.timeShiftReplaying
                                     || engineLink.timeShiftNote !== "")) {
                    bar.endPreview()
                }
                if (!scrub.pressed && !bar.settling) {
                    scrub.value = engineLink.timeShiftPosition()
                }
            }
        }
        // While held, the waterfall slides back to the instant under the
        // handle; render/waterfall_item.h, scrubSecondsAgo.
        onValueChanged: {
            if (pressed && bar.previewTarget)
                bar.previewTarget.scrubSecondsAgo = engineLink.timeShiftSecondsAgoAt(value)
        }
        onPressedChanged: {
            if (pressed) {
                bar.settling = false
                return
            }
            const ago = engineLink.timeShiftSecondsAgoAt(value)
            if (ago > 0) {
                bar.settling = true
                settleLimit.restart()
            } else {
                bar.endPreview()
            }
            engineLink.timeShiftScrub(value)
        }

        Tip {
            visible: parent.hovered
            text: "Drag left to go back in time, up to "
                  + Math.round(engineLink.timeShiftHeldSeconds) + " s. Release to replay "
                  + "the span from there; click a signal to open a receiver on it."
        }
    }

    Label {
        // The drag's target while held, the replay's position otherwise.
        text: scrub.pressed ? engineLink.timeShiftLabel(scrub.value)
                            : engineLink.timeShiftLabel(engineLink.timeShiftPosition())
        color: bar.replaying ? Theme.accent : Theme.ink
        font.family: Theme.monoFont
        font.pixelSize: Theme.sizeSmall
        Layout.minimumWidth: 44
        horizontalAlignment: Text.AlignRight
    }

    RComboBox {
        id: pace
        Layout.preferredWidth: 64
        model: ["1x", "2x", "4x", "8x"]
        currentIndex: Math.max(0, [1, 2, 4, 8].indexOf(engineLink.timeShiftSpeed))
        onActivated: (index) => engineLink.timeShiftSpeed = [1, 2, 4, 8][index]
        Tip { visible: parent.hovered; text: "Replay pace, as a multiple of real time" }
    }

    RButton {
        flat: true
        implicitHeight: 20
        text: "live"
        enabled: bar.replaying
        onClicked: engineLink.timeShiftLive()
        Tip { visible: parent.hovered; text: "Back to the live span" }
    }

    Label {
        visible: engineLink.timeShiftNote !== ""
        text: engineLink.timeShiftNote
        color: Theme.accent
        font.pixelSize: Theme.sizeSmall
        elide: Text.ElideRight
        Layout.maximumWidth: 260
        Layout.rightMargin: 8
    }
}
