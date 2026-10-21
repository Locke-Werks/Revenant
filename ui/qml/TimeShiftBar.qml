// The time-shift scrub bar under the waterfall: live at the right, the oldest
// sample the engine still holds at the left.
//
// Released off live, the span switches to a replay of that instant and the
// receiver in the pane moves onto it, so a click on the span tunes a signal
// that has already ended exactly as it would a live one. "live" brings both
// back. While dragging only the label moves; the replay starts on release,
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

        // Follows the replay while nobody holds it.
        value: engineLink.timeShiftPosition()
        Connections {
            target: engineLink
            function onTimeShiftChanged() {
                if (!scrub.pressed) {
                    scrub.value = engineLink.timeShiftPosition()
                }
            }
        }
        onPressedChanged: {
            if (!pressed) {
                engineLink.timeShiftScrub(value)
            }
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
