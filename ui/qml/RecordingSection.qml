// The recording section of the device picker: a recording is a source beside
// the radios, opened through the same close-then-open.
//
// The owner asked for it on 2026-09-23: "I want to be able to load recordings
// from the UI, switching from the dongle." What opening one does to the window
// is what opening a radio does, because it is the same EngineLink::openSource:
// receivers, audio and decoders go, the waterfall starts again, and
// sourceEpoch tells every index held that it restarted.
//
// THE ONE MODAL THING IN THE WINDOW. The picker is an inline panel on purpose
// and says why at the top of SourcePicker.qml. Choosing a file is the
// operating system's job, its dialog is modal, and a hand-drawn browser inside
// the panel would be a worse copy of it. Everything after the choice is
// inline again: the preview, the boxes and the open.
//
// Every rule is in ui/models: the header in recording_header.h, what is sent
// and when in recording_plan.h, the list in recent_recordings.h. This file
// binds names and chooses colours.

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: section

    Layout.fillWidth: true
    spacing: 6
    visible: engineLink.connected

    FileDialog {
        id: dialog

        title: "Open a recording"
        fileMode: FileDialog.OpenFile
        nameFilters: recordingLink.fileFilters
        currentFolder: recordingLink.folder
        onAccepted: recordingLink.chooseUrl(selectedFile)
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 4
        implicitHeight: 1
        color: Theme.border
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            text: "recording"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeTitle
            font.bold: true
        }

        RButton {
            text: "open recording…"
            font.pixelSize: Theme.sizeBody
            onClicked: dialog.open()
        }

        Label {
            Layout.minimumWidth: 0
            Layout.fillWidth: true
            visible: recordingLink.playing
            text: (recordingLink.ended ? "ended: " : "playing ") + recordingLink.playingName
            color: recordingLink.ended ? Theme.inkDim : Theme.accent
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideMiddle
        }

        Item {
            Layout.fillWidth: true
            visible: !recordingLink.playing
        }
    }

    // THE LAST TEN, only those still on disk. A pick puts the file in the
    // preview with the boxes it was last opened with, which is what makes a
    // recording that states no centre a one-click reopen.
    Repeater {
        model: recordingLink.recent

        RowLayout {
            id: recentRow

            required property var modelData
            required property int index

            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.minimumWidth: 0
                Layout.preferredWidth: 330
                text: recentRow.modelData.name
                color: recentHover.hovered ? Theme.accent : Theme.ink
                font.pixelSize: Theme.sizeBody
                elide: Text.ElideMiddle

                HoverHandler { id: recentHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: recordingLink.chooseRecent(recentRow.index) }
            }

            Label {
                visible: recentRow.modelData.centerText.length > 0
                text: recentRow.modelData.centerText
                color: Theme.inkTune
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
            }

            Label {
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                text: recentRow.modelData.folder
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
                elide: Text.ElideLeft
            }
        }
    }

    // WHAT THE FILE SAYS, before anything is sent. Read by this window from
    // its own disk; recording_header.h says why, and the preview adds a note
    // when the engine is somewhere else and would open its own copy.
    GridLayout {
        Layout.fillWidth: true
        visible: recordingLink.chosen
        columns: 4
        columnSpacing: 10
        rowSpacing: 2

        Repeater {
            model: [
                ["file", recordingLink.preview.name],
                ["container", recordingLink.preview.container],
                ["format", recordingLink.preview.format],
                ["rate", recordingLink.preview.rate],
                ["channels", recordingLink.preview.channels],
                ["length", recordingLink.plan.length.length > 0
                           ? recordingLink.plan.length : "needs the rate and format"],
                ["centre", recordingLink.preview.center],
                ["folder", recordingLink.preview.folder]
            ]

            delegate: RowLayout {
                id: fact

                required property var modelData

                Layout.fillWidth: true
                Layout.columnSpan: 2
                spacing: 8

                Label {
                    Layout.preferredWidth: 62
                    text: fact.modelData[0]
                    color: Theme.inkDim
                    font.pixelSize: Theme.sizeBody
                }

                Label {
                    Layout.minimumWidth: 0
                    Layout.fillWidth: true
                    text: fact.modelData[1] === undefined ? "" : fact.modelData[1]
                    color: Theme.ink
                    font.pixelSize: Theme.sizeBody
                    elide: Text.ElideMiddle
                }
            }
        }
    }

    Repeater {
        model: recordingLink.chosen ? recordingLink.preview.notes : []

        Label {
            required property string modelData

            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: "note: " + modelData
            color: Theme.inkWarn
            font.pixelSize: Theme.sizeBody
            wrapMode: Text.WordWrap
        }
    }

    StatusChip {
        visible: recordingLink.chosen && recordingLink.preview.refusal.length > 0
        label: "engine would refuse"
        detail: recordingLink.preview.refusal
        ink: Theme.inkBad
    }

    // The boxes the file cannot fill for itself, and the open.
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: recordingLink.chosen && recordingLink.preview.refusal.length === 0

        Label {
            text: "centre"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        // PREFILLED WHEN THE FILE RECORDS ONE AND REQUIRED WHEN IT DOES NOT.
        // Typing a centre that differs from a recorded one is refused with the
        // engine's reasoning before anything is sent.
        RTextField {
            Layout.preferredWidth: 120
            mono: true
            text: recordingLink.centerText
            placeholderText: "required"
            onTextEdited: recordingLink.centerText = text
        }

        Label {
            visible: recordingLink.preview.needsRate === true
            text: "rate"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        RTextField {
            visible: recordingLink.preview.needsRate === true
            Layout.preferredWidth: 110
            mono: true
            text: recordingLink.rateText
            placeholderText: "2400000 S/s"
            onTextEdited: recordingLink.rateText = text
        }

        Label {
            visible: recordingLink.preview.raw === true
            text: "format"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        RComboBox {
            readonly property string effective: recordingLink.formatText.length > 0
                                                ? recordingLink.formatText
                                                : recordingLink.preview.formatName

            visible: recordingLink.preview.raw === true
            Layout.preferredWidth: 80
            model: ["cu8", "cs8", "cs16", "cs24", "cf32"]
            currentIndex: model.indexOf(effective)
            displayText: currentIndex < 0 ? "choose" : currentText
            onActivated: (index) => recordingLink.formatText = model[index]
        }

        Item { Layout.fillWidth: true }

        // What will be sent, composed by the same plan the button sends.
        Label {
            Layout.minimumWidth: 0
            Layout.maximumWidth: 360
            visible: recordingLink.plan.ready
            text: recordingLink.plan.uri
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            elide: Text.ElideMiddle
        }

        RButton {
            text: "open"
            font.pixelSize: Theme.sizeBody
            enabled: recordingLink.plan.ready
            onClicked: recordingLink.open()
        }
    }

    // A centre read off the name, offered and never filled in on its own.
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: recordingLink.chosen && recordingLink.guessText.length > 0
                 && recordingLink.centerText.length === 0

        Label {
            Layout.minimumWidth: 0
            Layout.fillWidth: true
            text: recordingLink.guessText
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideRight
        }

        RButton {
            flat: true
            text: "use it"
            ink: Theme.inkTune
            onClicked: recordingLink.useGuess()
        }
    }

    // Why the open is dead, in one sentence.
    Label {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: recordingLink.chosen && !recordingLink.plan.ready
                 && recordingLink.preview.refusal.length === 0
        text: recordingLink.plan.blocker
        color: Theme.inkDim
        font.pixelSize: Theme.sizeBody
        wrapMode: Text.WordWrap
    }

    // What the engine opened, when it is not what this window read.
    StatusChip {
        visible: recordingLink.mismatch.length > 0
        label: "engine opened another file"
        detail: recordingLink.mismatch
        ink: Theme.inkWarn
    }
}
