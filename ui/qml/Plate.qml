// A label with enough background behind it to be read over the trace.

import QtQuick
import QtQuick.Controls
import Revenant

Rectangle {
    property alias text: plateText.text

    implicitWidth: plateText.implicitWidth + 8
    implicitHeight: plateText.implicitHeight + 4
    color: Theme.plate
    radius: 2

    Label {
        id: plateText
        anchors.centerIn: parent
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
    }
}
