// Two sentences about receivers the pane is not showing: ones the engine
// holds that this window is not on, and one the engine let go.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    Layout.fillWidth: true
    spacing: 6

    // Shown whenever either sentence has something to say.
    visible: engineLink.strandedReceiverText.length > 0 || engineLink.receiverGoneText.length > 0

    // ------------------------------------------------------------------
    // Receivers this window does not hold
    // ------------------------------------------------------------------
    //
    // A receiver outlives the client that created it, which is what lets
    // two windows each hold their own. Closing a window releases its
    // receiver; a window killed outright does not, and the engine then
    // carries a channelizer slot and its GPU work until it is restarted.
    //
    // THE RELEASE IS NOT OFFERED AS A REPAIR, because from here an orphan
    // and another operator's working receiver are the same thing: nothing
    // on the wire says who created one. So the row states the count and
    // the action says exactly what it will do, and somebody who knows
    // whether another window is open decides.
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: engineLink.strandedReceiverText.length > 0

        Label {
            Layout.minimumWidth: 0
            Layout.maximumWidth: Window.width * 0.6
            text: engineLink.strandedReceiverText
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideRight
        }

        Label {
            Layout.minimumWidth: 0
            text: "release them"
            color: Theme.inkWarn
            font.pixelSize: Theme.sizeBody

            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                cursorShape: Qt.PointingHandCursor
                onClicked: engineLink.releaseStrandedReceivers()
            }
        }

        Item { Layout.fillWidth: true }
    }

    // ------------------------------------------------------------------
    // A receiver the engine let go
    // ------------------------------------------------------------------
    //
    // OUTSIDE THE VFO PANE ON PURPOSE. The pane is visible only while a
    // receiver exists, so a line inside it explaining that the receiver
    // has gone is a line that goes with it. This sits above, beside the
    // tuning row that caused it, and clears when the operator tunes
    // somewhere rather than when the pane comes back.
    //
    // The wheel is what makes this worth a row of its own: a long sweep
    // walks the front end several spans, and the receiver is dropped
    // somewhere in the middle of a gesture that is still going.
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: engineLink.receiverGoneText.length > 0

        Label {
            Layout.minimumWidth: 0
            Layout.maximumWidth: Window.width * 0.9
            text: engineLink.receiverGoneText
            color: Theme.inkWarn
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideRight
        }
    }
}
