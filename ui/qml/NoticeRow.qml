// One line of news: a short heading and the sentence it introduces.
//
// Six rows in the window had exactly this shape, each written out by hand:
// the source's pace, a refused tune, the front end, a reconnect, a clamp and
// a refused detector. What differs between them is the words and how loudly
// they are said, so those are the properties and the shape is here once.
//
// Whether the row is shown at all stays with the caller. Each of the six has
// its own reason for appearing and that reason is part of what the row means.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: notice

    property string heading
    property color headingColor: Theme.inkDim
    property bool headingBold: true

    property string body
    property color bodyColor: Theme.ink
    property bool bodyBold: false

    // How many lines the sentence may take before it elides.
    property int lines: 2

    Layout.fillWidth: true
    spacing: 8

    Label {
        Layout.alignment: Qt.AlignTop
        text: notice.heading
        color: notice.headingColor
        font.pixelSize: Theme.sizeBody
        font.bold: notice.headingBold
    }

    Label {
        Layout.fillWidth: true
        text: notice.body
        color: notice.bodyColor
        font.pixelSize: Theme.sizeBody
        font.bold: notice.bodyBold
        wrapMode: Text.WordWrap
        maximumLineCount: notice.lines
        elide: Text.ElideRight
    }
}
