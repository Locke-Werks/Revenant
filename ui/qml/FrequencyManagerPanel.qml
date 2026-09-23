// The frequency manager: every memory, searchable, sortable and filtered by
// tag, with recall, edit in place, delete with undo, import, export and scan
// lists. It sits in a popover from the top bar's "memories" button, which is
// not modal: the span keeps drawing under it and a click elsewhere closes it.
//
// Layout and binding only. The list, the search, the sort, the file and every
// importer are ui/models/frequency_manager.h over models/memories.h and
// models/memory_import.h, which ui/tests hold. The keys are the table's
// Memories context in models/key_actions.h, looked up through KeyMap and
// never written here.
//
// Frequencies are in the monospace family at a fixed width, so a column of
// them reads as figures; tags are chips, the one accent marks what is chosen.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Item {
    id: panel

    // The popover this lives in, so Esc and the close key can close it.
    property var popover: null

    // "memories", "files" or "scan", the three views the segment switches.
    property string view: "memories"

    readonly property var manager: frequencyManager
    readonly property int rowHeight: 26

    // The key of the memory the list's current row is on, zero with none.
    readonly property var currentKey: list.currentItem ? list.currentItem.key : 0

    implicitWidth: body.implicitWidth
    implicitHeight: body.implicitHeight

    function focusSearch() {
        panel.view = "memories"
        search.forceActiveFocus()
        search.selectAll()
    }

    // What the popover calls as it opens: the search, unless an import is
    // waiting for an answer, which is then what the operator sees first.
    function opened() {
        if (panel.manager.importPending)
            panel.view = "files"
        else
            panel.focusSearch()
    }

    function focusList() {
        if (list.currentIndex < 0 && list.count > 0)
            list.currentIndex = 0
        list.forceActiveFocus()
    }

    function selectKey(key) {
        const row = panel.manager.rowOf(key)
        if (row >= 0)
            list.currentIndex = row
    }

    function recallCurrent(newReceiver) {
        if (panel.currentKey !== 0 && engineLink.sourceOpen)
            panel.manager.recall(panel.currentKey, newReceiver)
    }

    // One dispatcher for the table's Memories keys, from the list and from the
    // search field. The field hands over only what is not text editing.
    function command(name, fromField) {
        switch (name) {
        case "next":
            list.currentIndex = Math.min(list.currentIndex + 1, list.count - 1)
            return true
        case "previous":
            list.currentIndex = Math.max(list.currentIndex - 1, 0)
            return true
        case "recall":
            panel.recallCurrent(false)
            return true
        case "recallNew":
            panel.recallCurrent(true)
            return true
        case "edit":
            if (panel.currentKey !== 0) {
                nameEdit.forceActiveFocus()
                nameEdit.selectAll()
            }
            return true
        case "delete":
            if (fromField)
                return false
            if (panel.currentKey !== 0) {
                const row = list.currentIndex
                panel.manager.remove(panel.currentKey)
                list.currentIndex = Math.min(row, list.count - 1)
            }
            return true
        case "undo":
            if (fromField)
                return false
            panel.manager.undo()
            return true
        case "search":
            panel.focusSearch()
            return true
        case "close":
            if (panel.popover !== null)
                panel.popover.close()
            return true
        }
        return false
    }

    component Head: Text {
        required property string key
        property string title: key

        readonly property bool active: panel.manager.sortKey === key

        text: title + (active ? (panel.manager.descending ? " ▾" : " ▴") : "")
        color: active ? Theme.accent : Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        font.bold: active
        elide: Text.ElideRight

        TapHandler {
            onTapped: {
                if (parent.active) {
                    panel.manager.descending = !panel.manager.descending
                } else {
                    panel.manager.sortKey = parent.key
                    panel.manager.descending = false
                }
            }
        }

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
    }

    component FieldLabel: Label {
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
    }

    // A field in the editor under the list. See the editor for why it is
    // filled rather than bound.
    component EditField: RTextField {
        id: field

        property bool dirty: false

        onTextEdited: field.dirty = true
        onEditingFinished: editor.finish(field)
        Keys.onEscapePressed: {
            field.dirty = false
            editor.load()
            panel.focusList()
        }
    }

    // A file dropped anywhere on the panel is previewed for import.
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: (drop) => {
            if (drop.hasUrls && drop.urls.length > 0) {
                panel.manager.previewImport(drop.urls[0].toString())
                panel.view = "files"
            }
        }
    }

    Connections {
        target: panel.manager

        // An import waiting for an answer is shown wherever it came from.
        function onImportChanged() {
            if (panel.manager.importPending)
                panel.view = "files"
        }
    }

    ColumnLayout {
        id: body

        width: parent.width
        spacing: 8

        // --------------------------------------------------------------------
        // The header: search, the count, the three views, saving the receiver
        // --------------------------------------------------------------------

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: "memories"
                color: Theme.ink
                font.pixelSize: Theme.sizeTitle
                font.bold: true
            }

            RTextField {
                id: search

                Layout.fillWidth: true
                placeholderText: "search names, tags, notes, modes or 146.52"
                text: panel.manager.query
                onTextEdited: panel.manager.query = text

                Keys.onPressed: (event) => {
                    const name = KeyMap.memoryCommand(event.key, event.modifiers)
                    if (name === "")
                        return
                    if (name === "search") {
                        search.selectAll()
                        event.accepted = true
                        return
                    }
                    event.accepted = panel.command(name, true)
                    if (event.accepted && (name === "next" || name === "previous"))
                        list.forceActiveFocus()
                }
            }

            Readout {
                widest: "00000 of 00000"
                text: panel.manager.listed + " of " + panel.manager.total
            }

            RSegmented {
                options: ["memories", "files", "scan lists"]
                current: panel.view === "scan" ? "scan lists" : panel.view
                onPicked: (option) => panel.view = option === "scan lists" ? "scan" : option
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            RTextField {
                id: saveName
                Layout.preferredWidth: 180
                placeholderText: "name for the receiver"
                onAccepted: {
                    if (panel.manager.addFromReceiver(text) >= 0)
                        text = ""
                }
            }

            RButton {
                text: "save the receiver"
                enabled: engineLink.receiverId > 0
                onClicked: {
                    const row = panel.manager.addFromReceiver(saveName.text)
                    if (row >= 0) {
                        saveName.text = ""
                        list.currentIndex = row
                    }
                }
            }

            KeyChip {
                keys: KeyMap.keysText("memory.save")
            }

            // Said rather than left to be noticed, on the argument the bookmark
            // row made: the pointer's frequency and a saved one are the same
            // station well before they are the same number.
            StatusChip {
                visible: engineLink.receiverId > 0
                         && panel.manager.savedNear(engineLink.receiverCenterHz,
                                                    (engineLink.receiverPassbandHigh
                                                     - engineLink.receiverPassbandLow) / 2,
                                                    panel.manager.revision)
                label: "receiver already saved"
                ink: Theme.inkDim
            }

            Item { Layout.fillWidth: true }

            // Why the last recall did not happen, or what it is waiting for.
            StatusChip {
                visible: engineLink.bookmarkFault.length > 0
                label: "recall"
                detail: engineLink.bookmarkFault
                ink: Theme.inkWarn
            }
        }

        // --------------------------------------------------------------------
        // Memories
        // --------------------------------------------------------------------

        ColumnLayout {
            Layout.fillWidth: true
            visible: panel.view === "memories"
            spacing: 6

            // The tag filter. Every chosen tag has to be on a memory for it to be
            // listed, so each chip narrows.
            Flow {
                Layout.fillWidth: true
                spacing: 4
                visible: panel.manager.tags.length > 0

                Repeater {
                    model: panel.manager.tags

                    Rectangle {
                        id: chip

                        required property var modelData

                        readonly property color ink: modelData.selected ? Theme.accent : Theme.inkDim

                        implicitWidth: chipText.implicitWidth + 16
                        implicitHeight: 20
                        radius: 10
                        color: Qt.rgba(ink.r, ink.g, ink.b, modelData.selected ? 0.18 : 0.08)
                        border.width: 1
                        border.color: Qt.rgba(ink.r, ink.g, ink.b, 0.45)

                        Text {
                            id: chipText
                            anchors.centerIn: parent
                            text: chip.modelData.tag + "  " + chip.modelData.count
                            color: chip.modelData.selected ? Theme.accent : Theme.ink
                            font.pixelSize: Theme.sizeSmall
                        }

                        TapHandler {
                            onTapped: panel.manager.toggleTag(chip.modelData.tag)
                        }

                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
                }

                RButton {
                    flat: true
                    visible: panel.manager.selectedTags.length > 0
                    text: "clear tags"
                    ink: Theme.inkDim
                    implicitHeight: 20
                    onClicked: panel.manager.clearTags()
                }
            }

            // Column heads, each a sort. A second click on the column in force
            // turns it round.
            RowLayout {
                id: heads

                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                spacing: 10

                Head { key: "name"; Layout.fillWidth: true; Layout.minimumWidth: 120 }
                Head { key: "frequency"; title: "MHz"; Layout.preferredWidth: 104; horizontalAlignment: Text.AlignRight }
                Head { key: "mode"; Layout.preferredWidth: 58 }
                Head { key: "tags"; Layout.preferredWidth: 170 }
                Head { key: "used"; title: "last used"; Layout.preferredWidth: 110 }
                Item { Layout.preferredWidth: 96 }
            }

            ListView {
                id: list

                Layout.fillWidth: true
                Layout.preferredHeight: panel.rowHeight * 11
                clip: true
                model: panel.manager
                boundsBehavior: Flickable.StopAtBounds
                keyNavigationEnabled: false
                ScrollBar.vertical: RScrollBar {}
                onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

                Keys.onPressed: (event) => {
                    event.accepted = panel.command(KeyMap.memoryCommand(event.key, event.modifiers), false)
                }

                delegate: Rectangle {
                    id: row

                    required property int index
                    required property var key
                    required property string label
                    required property string freqText
                    required property string modeLabel
                    required property string tagsText
                    required property string usedText
                    required property string group
                    required property string mode
                    required property string notes
                    required property string name

                    width: ListView.view.width
                    height: panel.rowHeight
                    radius: Theme.radius
                    color: row.ListView.isCurrentItem ? Theme.controlHover
                           : rowHover.hovered ? Theme.control : "transparent"

                    // The accent on the current row's edge: it is what Return recalls.
                    Rectangle {
                        visible: row.ListView.isCurrentItem
                        width: 2
                        height: parent.height - 8
                        anchors.verticalCenter: parent.verticalCenter
                        x: 3
                        radius: 1
                        color: Theme.accent
                    }

                    HoverHandler { id: rowHover }

                    TapHandler {
                        onTapped: {
                            list.currentIndex = row.index
                            list.forceActiveFocus()
                        }
                        onDoubleTapped: {
                            list.currentIndex = row.index
                            panel.recallCurrent(false)
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 120
                            spacing: 8

                            Text {
                                Layout.minimumWidth: 0
                                Layout.maximumWidth: 260
                                text: row.label
                                color: Theme.ink
                                elide: Text.ElideRight
                                font.pixelSize: Theme.sizeBody
                            }

                            // The group, dimmer, since it is where the memory
                            // came from rather than what it is called.
                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: row.group
                                color: Theme.inkDim
                                elide: Text.ElideRight
                                font.pixelSize: Theme.sizeSmall
                            }
                        }

                        Text {
                            Layout.preferredWidth: 104
                            text: row.freqText
                            horizontalAlignment: Text.AlignRight
                            color: Theme.ink
                            font.family: Theme.monoFont
                            font.pixelSize: Theme.sizeBody
                        }

                        Text {
                            Layout.preferredWidth: 58
                            text: row.modeLabel
                            color: Theme.inkDim
                            font.pixelSize: Theme.sizeBody
                        }

                        Text {
                            Layout.preferredWidth: 170
                            text: row.tagsText
                            color: Theme.inkDim
                            elide: Text.ElideRight
                            font.pixelSize: Theme.sizeSmall
                        }

                        Text {
                            Layout.preferredWidth: 110
                            text: row.usedText
                            color: Theme.inkDim
                            font.family: Theme.monoFont
                            font.pixelSize: Theme.sizeSmall
                        }

                        Row {
                            Layout.preferredWidth: 96
                            spacing: 2

                            RButton {
                                flat: true
                                text: "recall"
                                enabled: engineLink.sourceOpen
                                implicitHeight: 22
                                onClicked: panel.manager.recall(row.key, false)
                            }

                            RButton {
                                flat: true
                                text: "+rx"
                                enabled: engineLink.sourceOpen && !engineLink.rackFull
                                implicitHeight: 22
                                onClicked: panel.manager.recall(row.key, true)
                            }

                            RButton {
                                flat: true
                                text: "×"
                                ink: Theme.inkDim
                                implicitHeight: 22
                                onClicked: panel.manager.remove(row.key)
                            }
                        }
                    }
                }
            }

            Text {
                visible: list.count === 0
                Layout.fillWidth: true
                Layout.preferredHeight: panel.rowHeight
                leftPadding: 8
                verticalAlignment: Text.AlignVCenter
                text: panel.manager.total === 0
                      ? "no memories yet: save the receiver, or import a file under \"files\""
                      : "nothing matches"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody
            }

            // Editing in place: the current memory's fields, each written when
            // it is left or Return is pressed in it.
            //
            // THE FIELDS ARE FILLED, NOT BOUND. A binding from a field's text
            // to the current row is broken by the first keystroke, and the
            // field then keeps showing that text on every row after. So load()
            // fills them whenever the row or the book changes, and each
            // remembers the memory it was filled from: a field left because
            // another row was clicked writes to the memory it was showing,
            // not to the one the click made current.
            GridLayout {
                id: editor

                readonly property var current: list.currentItem
                readonly property bool has: current ? true : false

                // The memory the fields were last filled from, zero with none.
                property var key: 0

                Layout.fillWidth: true
                visible: has
                columns: 6
                columnSpacing: 8
                rowSpacing: 6

                onCurrentChanged: editor.load()

                Connections {
                    target: panel.manager
                    function onBookChanged() { editor.load() }
                }

                // Writes one field if something was typed into it and not
                // written yet. Answers whether it wrote.
                function commit(field) {
                    if (!field.dirty || editor.key === 0)
                        return false
                    field.dirty = false
                    const key = editor.key
                    if (field === nameEdit)
                        panel.manager.setName(key, field.text)
                    else if (field === freqEdit)
                        panel.manager.setFrequency(key, field.text)
                    else if (field === tagsEdit)
                        panel.manager.setTags(key, field.text)
                    else if (field === groupEdit)
                        panel.manager.setGroup(key, field.text)
                    else if (field === notesEdit)
                        panel.manager.setNotes(key, field.text)
                    return true
                }

                // A field left by Return or by focus moving on. The list may
                // have re-sorted under the edit, so the row is found again.
                function finish(field) {
                    const key = editor.key
                    if (editor.commit(field))
                        panel.selectKey(key)
                }

                // Reads the list's current item directly rather than through
                // `has`, which is a binding and can still say true for the
                // moment a model reset has taken the item away.
                function load() {
                    for (const field of [nameEdit, freqEdit, tagsEdit, groupEdit, notesEdit])
                        editor.commit(field)
                    const row = list.currentItem
                    const here = row !== null && row !== undefined
                    editor.key = here ? row.key : 0
                    nameEdit.text = here ? row.name : ""
                    freqEdit.text = here ? row.freqText : ""
                    tagsEdit.text = here ? row.tagsText : ""
                    groupEdit.text = here ? row.group : ""
                    notesEdit.text = here ? row.notes : ""
                    modeEdit.currentIndex = here ? modeEdit.model.indexOf(row.mode) : -1
                }

                FieldLabel { text: "name" }
                EditField {
                    id: nameEdit
                    Layout.fillWidth: true
                    placeholderText: "unnamed"
                }

                FieldLabel { text: "MHz" }
                EditField {
                    id: freqEdit
                    Layout.preferredWidth: 130
                    mono: true
                }

                FieldLabel { text: "mode" }
                RComboBox {
                    id: modeEdit
                    Layout.preferredWidth: 96
                    model: panel.manager.modes()
                    onActivated: (index) => {
                        const key = editor.key
                        if (key !== 0) {
                            panel.manager.setMode(key, model[index])
                            panel.selectKey(key)
                        }
                    }
                }

                FieldLabel { text: "tags" }
                EditField {
                    id: tagsEdit
                    Layout.fillWidth: true
                    placeholderText: "comma separated"
                }

                FieldLabel { text: "group" }
                EditField {
                    id: groupEdit
                    Layout.preferredWidth: 130
                }

                FieldLabel { text: "notes" }
                EditField {
                    id: notesEdit
                    Layout.preferredWidth: 96
                    Layout.fillWidth: true
                }
            }
        }

        // --------------------------------------------------------------------
        // Files: import with a preview, and export
        // --------------------------------------------------------------------

        ColumnLayout {
            Layout.fillWidth: true
            visible: panel.view === "files"
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: "Import reads SDR#'s frequencies.xml, SDR++'s frequency_manager_config.json "
                      + "or a bookmark export from it, a CHIRP CSV export, and Revenant's own file. "
                      + "Type a path or drop the file on this panel. Nothing is added until you say so."
                wrapMode: Text.WordWrap
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                RTextField {
                    id: importPath
                    Layout.fillWidth: true
                    placeholderText: "path to a file to import"
                    onAccepted: panel.manager.previewImport(text)
                }

                RButton {
                    text: "preview"
                    enabled: importPath.text.length > 0
                    onClicked: panel.manager.previewImport(importPath.text)
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: panel.manager.importPending
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    text: panel.manager.importSummary
                    color: Theme.ink
                    font.pixelSize: Theme.sizeBody
                    font.bold: true
                    elide: Text.ElideRight
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    // What would be added.
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 180
                        clip: true
                        model: panel.manager.importAdds
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: RScrollBar {}

                        delegate: RowLayout {
                            required property var modelData

                            width: ListView.view.width
                            height: 20
                            spacing: 8

                            Text {
                                Layout.fillWidth: true
                                text: parent.modelData.label
                                      + (parent.modelData.group.length > 0 ? "   " + parent.modelData.group : "")
                                color: Theme.ink
                                elide: Text.ElideRight
                                font.pixelSize: Theme.sizeSmall
                            }
                            Text {
                                Layout.preferredWidth: 90
                                text: parent.modelData.freqText
                                horizontalAlignment: Text.AlignRight
                                color: Theme.ink
                                font.family: Theme.monoFont
                                font.pixelSize: Theme.sizeSmall
                            }
                            Text {
                                Layout.preferredWidth: 52
                                text: parent.modelData.mode
                                color: Theme.inkDim
                                font.pixelSize: Theme.sizeSmall
                            }
                        }
                    }

                    // Every entry that will not be added, with its line and why.
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 180
                        clip: true
                        model: panel.manager.importSkips
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: RScrollBar {}

                        delegate: RowLayout {
                            required property var modelData

                            width: ListView.view.width
                            height: 20
                            spacing: 8

                            Text {
                                Layout.preferredWidth: 44
                                text: parent.modelData.line > 0 ? "line " + parent.modelData.line : ""
                                color: Theme.inkDim
                                font.family: Theme.monoFont
                                font.pixelSize: Theme.sizeSmall
                            }
                            Text {
                                Layout.preferredWidth: 120
                                text: parent.modelData.what
                                color: Theme.ink
                                elide: Text.ElideRight
                                font.pixelSize: Theme.sizeSmall
                            }
                            Text {
                                Layout.fillWidth: true
                                text: parent.modelData.reason
                                color: Theme.inkWarn
                                elide: Text.ElideRight
                                font.pixelSize: Theme.sizeSmall
                            }
                        }
                    }
                }

                RowLayout {
                    spacing: 8

                    RButton {
                        text: "add them"
                        tint: Theme.accent
                        enabled: panel.manager.importAdds.length > 0
                        onClicked: panel.manager.commitImport()
                    }

                    RButton {
                        flat: true
                        text: "cancel"
                        onClicked: panel.manager.cancelImport()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: "export the " + panel.manager.listed + " listed as"
                    color: Theme.inkDim
                    font.pixelSize: Theme.sizeBody
                }

                RSegmented {
                    id: exportFormat
                    property string format: "json"
                    options: ["Revenant JSON", "CHIRP CSV"]
                    current: format === "chirp" ? "CHIRP CSV" : "Revenant JSON"
                    onPicked: (option) => {
                        format = option === "CHIRP CSV" ? "chirp" : "json"
                        exportPath.text = panel.manager.suggestedExportPath(format)
                    }
                }

                RTextField {
                    id: exportPath
                    Layout.fillWidth: true
                    text: panel.manager.suggestedExportPath("json")
                }

                RButton {
                    text: "export"
                    enabled: panel.manager.listed > 0 && exportPath.text.length > 0
                    onClicked: panel.manager.exportListed(exportPath.text, exportFormat.format)
                }
            }
        }

        // --------------------------------------------------------------------
        // Scan lists
        // --------------------------------------------------------------------

        ColumnLayout {
            Layout.fillWidth: true
            visible: panel.view === "scan"
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: "Scan lists are kept with the memories. Nothing scans them yet: the scanning "
                      + "engine is a later piece of work, and these are the lists it will read."
                wrapMode: Text.WordWrap
                color: Theme.inkWarn
                font.pixelSize: Theme.sizeSmall
            }

            Repeater {
                model: panel.manager.scanLists

                RowLayout {
                    id: scanRow

                    required property var modelData
                    required property int index

                    Layout.fillWidth: true
                    spacing: 10

                    Text {
                        Layout.preferredWidth: 160
                        text: scanRow.modelData.name
                        color: Theme.ink
                        elide: Text.ElideRight
                        font.pixelSize: Theme.sizeBody
                    }
                    Text {
                        Layout.fillWidth: true
                        text: scanRow.modelData.summary
                        color: Theme.inkDim
                        elide: Text.ElideRight
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }
                    RButton {
                        flat: true
                        text: "×"
                        ink: Theme.inkDim
                        implicitHeight: 22
                        onClicked: panel.manager.removeScanList(scanRow.index)
                    }
                }
            }

            Text {
                visible: panel.manager.scanLists.length === 0
                text: "no scan lists"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                RTextField {
                    id: listName
                    Layout.preferredWidth: 160
                    placeholderText: "list name"
                }

                RButton {
                    text: "from the " + panel.manager.listed + " listed"
                    onClicked: scanProblem.text = panel.manager.addScanListFromListed(listName.text)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                RTextField { id: rangeName; Layout.preferredWidth: 160; placeholderText: "range name" }
                RTextField { id: rangeLow; Layout.preferredWidth: 100; mono: true; placeholderText: "from MHz" }
                RTextField { id: rangeHigh; Layout.preferredWidth: 100; mono: true; placeholderText: "to MHz" }
                RTextField { id: rangeStep; Layout.preferredWidth: 90; mono: true; placeholderText: "step Hz" }
                RComboBox { id: rangeMode; Layout.preferredWidth: 90; model: panel.manager.modes() }

                RButton {
                    text: "add range"
                    onClicked: scanProblem.text = panel.manager.addScanRange(
                        rangeName.text, rangeLow.text, rangeHigh.text, rangeStep.text,
                        rangeMode.currentText)
                }
            }

            Text {
                id: scanProblem
                visible: text.length > 0
                color: Theme.inkWarn
                font.pixelSize: Theme.sizeSmall
            }
        }

        // --------------------------------------------------------------------
        // The foot: undo, the file, and what just happened
        // --------------------------------------------------------------------

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            RButton {
                flat: true
                visible: panel.manager.canUndo
                text: panel.manager.undoText
                onClicked: panel.manager.undo()
            }

            KeyChip {
                visible: panel.manager.canUndo
                keys: KeyMap.keysText("memories.undo")
            }

            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: panel.manager.status
                color: Theme.inkDim
                elide: Text.ElideRight
                font.pixelSize: Theme.sizeSmall
            }

            StatusChip {
                visible: panel.manager.fault.length > 0
                label: "file not saved"
                detail: panel.manager.fault
                ink: Theme.inkBad
            }

            Label {
                Layout.maximumWidth: 360
                text: panel.manager.filePath.length > 0 ? panel.manager.filePath : "not saved to a file"
                color: Theme.inkOff
                elide: Text.ElideMiddle
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
            }
        }

        Text {
            Layout.fillWidth: true
            text: KeyMap.keysText("memories.recall") + " recalls, "
                  + KeyMap.keysText("memories.recall_new") + " into a new receiver, "
                  + KeyMap.keysText("memories.edit") + " renames, "
                  + KeyMap.keysText("memories.delete") + " deletes, "
                  + KeyMap.keysText("memories.search") + " searches"
            color: Theme.inkDim
            elide: Text.ElideRight
            font.pixelSize: Theme.sizeSmall
        }
    }
}
