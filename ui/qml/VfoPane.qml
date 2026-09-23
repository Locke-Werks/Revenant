// The VFO detail display, and the filter drawn over it
// ------------------------------------------------------------------
// This is the receiver's OWN passband, transformed by the engine,
// and the two rules over it are that receiver's filter edges. The
// edges are dragged here, which is the one interaction in this
// window that changes what the engine is doing rather than what the
// window is showing.
//
// The pane appears when a receiver exists and not before. There is
// nothing to draw and nothing to drag without one, and an empty
// pane with handles in it invites a gesture that cannot do
// anything.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    Layout.fillWidth: true
    spacing: 2
    visible: engineLink.receiverId > 0

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            text: "vfo"
            color: Theme.inkTune
            font.pixelSize: Theme.sizeTitle
            font.bold: true
        }

        // The receiver's own dial, in the receiver's colour. Its limits are
        // the span, because a receiver outside the span is one the engine
        // removes. A step is a move by hand, so it goes through tuneReceiver
        // and not the detection entry point: there is no measurement behind
        // where the operator wheeled it to.
        FrequencyDial {
            value: engineLink.receiverCenterHz
            low: engineLink.spanLowHz
            high: engineLink.spanHighHz
            tint: Theme.receiverColours[0]
            pixelSize: 20
            onStepped: (hz) => engineLink.tuneReceiver(hz, "")
            onTyped: (text) => {
                const hz = engineLink.parseHz(text)
                if (hz > 0)
                    engineLink.tuneReceiver(hz, "")
            }
        }

        // The mode, as the eight buttons an operator actually
        // reaches for. Changing one is a remove and an add
        // underneath, because the demodulator is the stage; the
        // pane keeps its identity across that and the operator sees
        // a mode change.
        RSegmented {
            options: ["am", "nfm", "wfm", "usb", "lsb", "dsb", "cw", "raw"]
            current: engineLink.receiverDemod
            tint: Theme.receiverColours[0]
            onPicked: (mode) => engineLink.setReceiverDemod(mode)
        }

        Item { Layout.fillWidth: true }

        Label {
            visible: engineLink.receiverDemodRate > 0
            text: (engineLink.receiverDemodRate / 1000).toFixed(1) + " kS/s"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        Label {
            visible: engineLink.receiverLevelDbfs > -199
            text: engineLink.receiverLevelDbfs.toFixed(1) + " dBFS"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        Label {
            text: "clear"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody

            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                cursorShape: Qt.PointingHandCursor
                onClicked: engineLink.removeReceiver()
            }
        }
    }

    Item {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.round(Window.height * 0.18)

        PassbandItem {
            id: passband
            anchors.fill: parent
            link: engineLink
        }

        Plate {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 4
            visible: !engineLink.passbandActive
            text: "waiting for the first passband frame"
        }
    }

    // The readout, which is the whole of what a drag says back.
    // Live while one is running and the granted pair when one is
    // not, so the strip never goes blank and never shows a stale
    // gesture. The colour is the one thing this file decides: the
    // item reports that an edge is against the channel limit and
    // the window chooses how loudly to say so.
    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            Layout.minimumWidth: 0
            text: passband.readout
            color: passband.atLimit ? Theme.inkWarn
                   : engineLink.receiverClamped ? Theme.inkWarn : Theme.ink
            font.pixelSize: Theme.sizeBody
            font.bold: passband.dragging
            elide: Text.ElideRight
        }

        Item { Layout.fillWidth: true }

        Label {
            Layout.minimumWidth: 0
            text: "drag an edge, shift-drag to widen both, [ ] \\ select, "
                  + "arrows move, up and down widen, home resets"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            elide: Text.ElideRight
        }
    }

    // WHY THIS RECEIVER IS WRONG FOR THIS SIGNAL, when it is.
    //
    // Two mismatches bit on 2026-09-20 and neither said anything.
    // A click on a 145 kHz detection produced a 16 kHz NFM
    // receiver, and every number on screen was individually
    // correct: the detection row said 145 kHz, the readout above
    // said 16 kHz, and nothing put them beside each other. A
    // second receiver asked for 200 kHz and got 71, which the
    // overlay answered with a dimmer shade of the same colour.
    //
    // The arithmetic and the wording are in
    // models/receiver_match.h with their own cases in ui/tests.
    // This row decides the colour, and it is inkWarn rather than
    // inkBad: the receiver works, it is pointed at the wrong
    // shape of thing, and that is something the operator fixes
    // with the mode buttons or the handles above.
    Label {
        Layout.fillWidth: true
        visible: engineLink.receiverFitText.length > 0
        text: engineLink.receiverFitText
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }

    Label {
        Layout.fillWidth: true
        visible: engineLink.receiverFault.length > 0
        text: engineLink.receiverFault
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeSmall
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }
}
