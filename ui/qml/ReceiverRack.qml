// The receiver rack: one compact strip per receiver, like the channel strips
// of a mixing console. Each strip carries its receiver's colour, which is the
// colour of its marker on the span and its dial in the detail beside it.
//
// ONE STRIP TODAY. The engine holds any number of receivers, and this client
// holds one per window: EngineLink's receiver surface is a single pane's, and
// the rest of what the engine has is counted in the status drawer as
// receivers this window is not on. The rack is built as a column of strips so
// a second one drops in when the client can hold it, and the strip is its own
// file for that reason.
//
// Solo is drawn and not offered. With one receiver there is nothing to solo
// against, and a control that does nothing reads as broken; the strip's
// tooltip says why it is there.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    spacing: 8

    Label {
        text: "receivers"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        font.bold: true
    }

    ReceiverStrip {
        Layout.fillWidth: true
        visible: engineLink.receiverId > 0
        tint: Theme.receiverColours[0]
        focused: true
    }

    // No receiver. Said where the strip would be, with the way to make one,
    // and with the reason the last one went when the engine let it go.
    Label {
        Layout.fillWidth: true
        visible: engineLink.receiverId === 0
        text: engineLink.receiverGoneText.length > 0
              ? engineLink.receiverGoneText
              : "No receiver. Click a signal on the spectrum, or a place on the ruler, "
                + "and one opens here."
        color: engineLink.receiverGoneText.length > 0 ? Theme.inkWarn : Theme.inkDim
        font.pixelSize: Theme.sizeBody
        wrapMode: Text.WordWrap
    }

    Item { Layout.fillHeight: true }
}
