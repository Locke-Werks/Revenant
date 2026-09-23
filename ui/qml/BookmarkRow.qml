// Bookmarks
// ------------------------------------------------------------------
//
// HERE AND NOT IN THE VFO PANE, although a bookmark is made from the
// receiver. The pane appears only when a receiver exists, and the
// half of this that matters when there is no receiver is the recall:
// an operator opening the window wants to get back to a station, and
// a list that was hidden until they had already tuned somewhere
// would be a list they never saw. Beside the band buttons instead,
// which is the one row already about going to a frequency.
//
// A CLICK RECALLS AND THE CROSS FORGETS, rather than a mode or a
// menu. The list is a row of places; the destructive action is its
// own target so that a mis-aimed click on a station name cannot
// delete it.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    Layout.fillWidth: true
    spacing: 8
    visible: engineLink.connected

    Label {
        text: "marks"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeTitle
        font.bold: true
    }

    TextField {
        id: bookmarkName

        Layout.preferredWidth: 130
        placeholderText: "name"
        color: Theme.ink
        font.pixelSize: Theme.sizeBody

        // Enter saves, so naming and saving is one gesture. The field
        // is cleared on the way out rather than left holding a name
        // already used, which would make the next save look like a
        // rename of the last one.
        onAccepted: {
            engineLink.saveBookmark(text)
            text = ""
        }
    }

    // Greyed with no receiver rather than hidden, on the same argument
    // the tune box makes: a control that appears when something else
    // happens is one an operator has to discover twice.
    Label {
        text: "save"
        color: engineLink.receiverId > 0 ? Theme.inkTune : Theme.inkOff
        font.pixelSize: Theme.sizeBody
        font.bold: engineLink.receiverId > 0

        MouseArea {
            anchors.fill: parent
            anchors.margins: -3
            enabled: engineLink.receiverId > 0
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                engineLink.saveBookmark(bookmarkName.text)
                bookmarkName.text = ""
            }
        }
    }

    // The receiver is already in the list. Said rather than left to be
    // noticed, because the frequency under the pointer and the
    // frequency on a saved row are the same station well before they
    // are the same number.
    Label {
        Layout.minimumWidth: 0
        visible: engineLink.receiverBookmarked
        text: "already saved"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }

    Repeater {
        model: engineLink.bookmarkLabels

        RowLayout {
            id: bookmarkRow

            required property string modelData
            required property int index

            spacing: 2

            Label {
                text: bookmarkRow.modelData
                color: Theme.inkTune
                font.pixelSize: Theme.sizeBody

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -3
                    cursorShape: Qt.PointingHandCursor
                    onClicked: engineLink.recallBookmark(bookmarkRow.index)
                }
            }

            Label {
                text: "×"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -3
                    cursorShape: Qt.PointingHandCursor
                    onClicked: engineLink.removeBookmark(bookmarkRow.index)
                }
            }
        }
    }

    Item { Layout.fillWidth: true }

    // Why the last save or recall did not happen, or what is being
    // waited for. Its own string and not the receiver's fault line,
    // because the commonest refusal is about the source: a bookmark
    // outside a recording's span is not the receiver having failed.
    Label {
        Layout.minimumWidth: 0
        Layout.maximumWidth: Window.width * 0.4
        visible: engineLink.bookmarkFault.length > 0
        text: engineLink.bookmarkFault
        color: Theme.inkWarn
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }
}
