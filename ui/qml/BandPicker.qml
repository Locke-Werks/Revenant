// The bands: a few one-click shortcuts, and every band in a two-pane menu.
//
// The table is models/band_plan.h, with the regulations it was taken from and
// cases in ui/tests. A click puts the front end at the band's centre, which
// the table chooses as the part of the band worth landing on rather than the
// middle of the allocation, moved only as far as keeps the whole band in the
// span.
//
// A band the open device cannot be put on is greyed rather than hidden, so
// the menu is the same list on every radio and says which parts of it this
// one reaches. On a source that cannot retune every row is grey, and the
// tuning row says why beside the dial.
//
// A click puts the band on screen: when it fits the span the spectrum and
// waterfall narrow to it, and when it does not the band's low edge goes to
// the left of the span. EngineLink::pickBand.
//
// The left pane lists each category once; clicking one shows its bands in
// the right pane. The table interleaves a category's allocations, channels
// and sub-bands, so rows are gathered per category here and each sub-band is
// placed under the allocation it lies within. Channels fold under one entry
// per parent (CB channels, FRS/GMRS channels) that opens in place.
//
// Typing filters across every category: the right pane shows the matches
// under their category headings, and the left pane greys categories with
// none.
//
// Opening or closing an entry must not move anything above it. The right
// pane's rows live in a ListModel edited in place (rows inserted or removed)
// rather than reassigned, because handing a view a new model array resets it:
// scroll to the top, current row gone.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

RowLayout {
    id: picker

    // BandKind in band_plan.h.
    readonly property int kindAllocation: 0
    readonly property int kindSubBand: 1
    readonly property int kindChannel: 2

    // Unreachable rows. Theme.inkOff is about 1.9:1 on panelSolid and
    // Theme.inkDim about 4.5:1, a hair under WCAG AA; this is about 5.5:1, so
    // the name stays readable while still sitting below Theme.ink.
    readonly property color inkOut: "#7f8a9a"

    readonly property int rowHeight: 24
    readonly property int headingHeight: 26
    readonly property int groupWidth: 190
    readonly property int columnWidth: 440

    // Re-read when the region changes in the settings window. UiRules.bands()
    // reads the stored region, which BandPlanSettings writes before it
    // signals, and naming the property here is what makes the binding depend
    // on it.
    readonly property var bands: {
        BandPlanSettings.region
        return UiRules.bands()
    }

    // Every row the menu can show, category by category, built once per
    // region. The model holds indices into it ("ri"), so a row's identity
    // survives any amount of opening, closing and filtering.
    property var allRows: []

    // Category names in table order, each once.
    property var groups: []

    // Kept here rather than in the popup so they outlast a close and reopen.
    property string selectedGroup: ""
    property var expanded: ({})

    // Typed while the menu is open; narrows the rows to matching names.
    property string filter: ""

    // Categories with at least one row matching the filter.
    property var matchedGroups: ({})

    // The cursor: column 0 is the category list, column 1 the band pane.
    property int curCol: 0
    property int curPos: -1

    ListModel { id: bandRows }

    onBandsChanged: picker.rebuild()
    Component.onCompleted: picker.rebuild()

    // A map's "index" is its row in kBands, which is what bandReachable takes;
    // its position in this list is not, because the region filter skips rows.
    function reachable(band) {
        return engineLink.sourceCanRetune
               && UiRules.bandReachable(band.index, engineLink.sourceTuneLowHz,
                                        engineLink.sourceTuneHighHz)
    }

    function tuneTo(band) {
        // Tunes and, when the band fits the span, narrows the view to it.
        // models/span_view.h has the rule; Ctrl+Home or the span's full
        // button puts the whole span back.
        engineLink.pickBand(band.low, band.high, band.centre)
        menu.close()
    }

    // Both edges in the larger edge's unit, so "7 to 7.3 MHz" reads as one span.
    function edges(low, high) {
        const unit = high >= 1e9 ? 1e9 : high >= 1e6 ? 1e6 : 1e3
        const name = unit === 1e9 ? " GHz" : unit === 1e6 ? " MHz" : " kHz"
        return +(low / unit).toFixed(6) + "–" + +(high / unit).toFixed(6) + name
    }

    // One category's rows: allocations in table order, each followed by the
    // sub-bands inside it; parents (channel folds) and anything that fits no
    // allocation keep their table position relative to the allocations.
    function buildGroup(group, members) {
        const allocations = members.filter(b => b.parent === ""
                                                 && b.kind === picker.kindAllocation)
        const under = new Map()
        for (const a of allocations) under.set(a, [])

        const top = []
        const parents = {}
        for (const band of members) {
            if (band.parent !== "") {
                const key = group + "\u0001" + band.parent
                let entry = parents[key]
                if (!entry) {
                    entry = { type: "parent", key: key, label: band.parent,
                              group: group, low: band.low, high: band.high,
                              children: [], depth: 0,
                              text: band.parent.toLowerCase() }
                    parents[key] = entry
                    top.push(entry)
                }
                entry.low = Math.min(entry.low, band.low)
                entry.high = Math.max(entry.high, band.high)
                entry.children.push({ type: "band", band: band, depth: 1, group: group,
                                      parentKey: key,
                                      text: (band.name + " " + band.parent).toLowerCase() })
                continue
            }
            if (band.kind === picker.kindAllocation) {
                top.push({ type: "band", band: band, depth: 0, group: group,
                           parentKey: "", text: band.name.toLowerCase() })
                continue
            }
            // The narrowest allocation that holds it, so a sub-band of a
            // sub-allocation lands under the tighter one.
            let home = null
            for (const a of allocations)
                if (band.low >= a.low && band.high <= a.high
                        && (!home || a.high - a.low < home.high - home.low))
                    home = a
            const row = { type: "band", band: band, depth: home ? 1 : 0, group: group,
                          parentKey: "", text: band.name.toLowerCase() }
            if (home) under.get(home).push(row)
            else top.push(row)
        }

        const rows = [{ type: "group", key: group, label: group, group: group,
                        text: group.toLowerCase() }]
        for (const row of top) {
            rows.push(row)
            if (row.type === "parent")
                for (const child of row.children) rows.push(child)
            else if (row.band.kind === picker.kindAllocation)
                for (const sub of under.get(row.band)) rows.push(sub)
        }
        return rows
    }

    function build(bands) {
        const order = []
        const byGroup = {}
        for (const band of bands) {
            if (!byGroup[band.group]) {
                byGroup[band.group] = []
                order.push(band.group)
            }
            byGroup[band.group].push(band)
        }
        let rows = []
        for (const group of order) rows = rows.concat(picker.buildGroup(group, byGroup[group]))
        return { rows: rows, groups: order }
    }

    // Which rows the band pane should hold now, as ascending indices into
    // allRows.
    function wanted() {
        const rows = picker.allRows
        const out = []
        const f = picker.filter.toLowerCase()
        if (f === "") {
            for (let i = 0; i < rows.length; ++i) {
                const row = rows[i]
                if (row.group !== picker.selectedGroup || row.type === "group") continue
                if (row.type === "band" && row.parentKey !== ""
                        && !picker.expanded[row.parentKey]) continue
                out.push(i)
            }
            return out
        }

        // Filtering ignores folding: a match is shown with its parent and
        // heading so it still reads in context.
        const keep = {}
        let group = -1
        let parent = -1
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (row.type === "group") { group = i; parent = -1; continue }
            if (row.type === "parent") {
                parent = i
                if (row.text.includes(f)) { keep[group] = true; keep[i] = true }
                continue
            }
            if (row.parentKey === "") parent = -1
            if (row.text.includes(f)) {
                keep[group] = true
                keep[i] = true
                if (parent >= 0) keep[parent] = true
            }
        }
        for (let i = 0; i < rows.length; ++i)
            if (keep[i]) out.push(i)
        return out
    }

    function currentRi() {
        return picker.curCol === 1 && picker.curPos >= 0 && picker.curPos < bandRows.count
               ? bandRows.get(picker.curPos).ri : -1
    }

    // Brings the model to the wanted rows with inserts and removes only, so
    // rows that stay keep their delegates and the view keeps its scroll.
    function sync() {
        const keepRi = picker.currentRi()
        const need = picker.wanted()

        const matched = {}
        if (picker.filter !== "")
            for (const ri of need) matched[picker.allRows[ri].group] = true
        picker.matchedGroups = matched

        let i = 0
        let j = 0
        while (i < bandRows.count || j < need.length) {
            const have = i < bandRows.count ? bandRows.get(i).ri : Infinity
            const next = j < need.length ? need[j] : Infinity
            if (have === next) {
                ++i; ++j
            } else if (have < next) {
                let n = 1
                while (i + n < bandRows.count && bandRows.get(i + n).ri < next) ++n
                bandRows.remove(i, n)
            } else {
                bandRows.insert(i, { ri: next })
                ++i; ++j
            }
        }
        if (picker.curCol === 1) picker.restoreCurrent(keepRi)
    }

    // Puts the cursor back on the row it was on, or on the entry that row
    // folded into.
    function restoreCurrent(ri) {
        if (ri < 0) { picker.curPos = bandRows.count > 0 ? 0 : -1; return }
        const row = picker.allRows[ri]
        const owners = [ri]
        if (row.type === "band" && row.parentKey !== "")
            owners.push(picker.allRows.findIndex(r => r.key === row.parentKey
                                                      && r.type === "parent"))
        for (const target of owners) {
            for (let i = 0; i < bandRows.count; ++i) {
                if (bandRows.get(i).ri === target) {
                    picker.curPos = i
                    return
                }
            }
        }
        picker.curPos = bandRows.count > 0 ? 0 : -1
    }

    function rebuild() {
        const built = picker.build(picker.bands)
        picker.allRows = built.rows
        picker.groups = built.groups
        if (!picker.groups.includes(picker.selectedGroup))
            picker.selectedGroup = picker.groups.length > 0 ? picker.groups[0] : ""
        bandRows.clear()
        picker.curCol = 0
        picker.curPos = picker.groups.indexOf(picker.selectedGroup)
        picker.sync()
    }

    function selectGroup(group) {
        if (picker.filter !== "") picker.filter = ""
        if (group !== picker.selectedGroup) {
            picker.selectedGroup = group
            bandRows.clear()
            flick.contentY = 0
        }
        picker.curCol = 0
        picker.curPos = picker.groups.indexOf(group)
        picker.sync()
    }

    function rowEnabled(row) {
        if (row.type === "band") return picker.reachable(row.band)
        if (row.type === "parent")
            return row.children.some(child => picker.reachable(child.band))
        return true
    }

    function isOpen(row) {
        return row.type === "parent" && (picker.filter !== "" || !!picker.expanded[row.key])
    }

    function toggle(row) {
        // Folding is meaningless while a filter has everything open.
        if (picker.filter !== "" || row.type !== "parent") return
        const next = Object.assign({}, picker.expanded)
        if (next[row.key]) delete next[row.key]
        else next[row.key] = true
        picker.expanded = next
        picker.sync()
    }

    function activate(row) {
        if (row.type === "group") picker.selectGroup(row.group)
        else if (row.type === "parent") picker.toggle(row)
        else if (picker.reachable(row.band)) picker.tuneTo(row.band)
    }

    function setFilter(text) {
        picker.filter = text
        picker.sync()
        if (text !== "" && bandRows.count > 0) {
            picker.curCol = 1
            if (picker.curPos < 0 || picker.curPos >= bandRows.count) picker.curPos = 0
        } else if (text === "") {
            picker.curCol = 0
            picker.curPos = picker.groups.indexOf(picker.selectedGroup)
        }
        picker.reveal()
    }

    // Scrolls the band pane just enough to show the current row.
    function reveal() {
        if (picker.curCol !== 1) return
        const item = bandRepeater.itemAt(picker.curPos)
        if (!item) return
        if (item.y < flick.contentY) flick.contentY = item.y
        else if (item.y + item.height > flick.contentY + flick.height)
            flick.contentY = item.y + item.height - flick.height
    }

    function moveTo(pos) {
        if (picker.curCol === 0) {
            if (picker.groups.length === 0) return
            const p = Math.max(0, Math.min(picker.groups.length - 1, pos))
            picker.selectGroup(picker.groups[p])
            return
        }
        if (bandRows.count === 0) return
        picker.curPos = Math.max(0, Math.min(bandRows.count - 1, pos))
        picker.reveal()
    }

    // Off when the top bar is too narrow for the shortcuts; the menu still
    // has every band. TuneBar decides from the two widths below, which do
    // not depend on whether the shortcuts are shown.
    property bool shortcuts: true
    readonly property real openerWidth: opener.implicitWidth
    readonly property real favouritesWidth: {
        let width = 0
        for (let i = 0; i < favourites.count; ++i) {
            const item = favourites.itemAt(i)
            if (item && item.modelData.favourite)
                width += item.implicitWidth + picker.spacing
        }
        return width
    }

    spacing: 2

    Repeater {
        id: favourites
        model: picker.bands

        RButton {
            required property var modelData

            visible: modelData.favourite && picker.shortcuts
            flat: true
            text: modelData.name
            enabled: picker.reachable(modelData)
            ink: Theme.inkDim
            onClicked: picker.tuneTo(modelData)

            Tip {
                visible: parent.hovered
                text: picker.edges(modelData.low, modelData.high)
                      + (modelData.mode !== "" ? ", " + modelData.mode : "")
                      + (modelData.source !== "" ? "\n" + modelData.source : "")
            }
        }
    }

    RButton {
        id: opener

        flat: true
        text: "bands ▾"
        ink: Theme.inkDim

        // Open on any source, so the list can be read on a recording too;
        // its rows are what is greyed.
        onClicked: menu.opened ? menu.close() : menu.open()
    }

    Component {
        id: rowDelegate

        Item {
            id: entry

            required property int ri
            required property int index

            readonly property var row: picker.allRows[ri] ?? null
            readonly property bool heading: row !== null && row.type === "group"
            readonly property bool parentEntry: row !== null && row.type === "parent"
            readonly property bool open: row !== null && picker.isOpen(row)
            readonly property bool live: row !== null && picker.rowEnabled(row)
            readonly property bool current: picker.curCol === 1 && picker.curPos === index
            readonly property var band: row !== null && row.type === "band" ? row.band : null

            width: picker.columnWidth
            height: heading ? picker.headingHeight : picker.rowHeight

            Rectangle {
                anchors.fill: parent
                visible: !entry.heading
                color: (hover.containsMouse || entry.current) && entry.live
                       ? Theme.controlHover : "transparent"
                // The cursor on an out-of-range row has no fill to show it,
                // so it gets an outline instead.
                border.width: entry.current && !entry.live ? 1 : 0
                border.color: Theme.border
            }

            MouseArea {
                id: hover

                anchors.fill: parent
                enabled: !entry.heading
                hoverEnabled: true
                cursorShape: entry.live ? Qt.PointingHandCursor : Qt.ArrowCursor
                onEntered: {
                    picker.curCol = 1
                    picker.curPos = entry.index
                }
                onClicked: picker.activate(entry.row)
            }

            // Only shown while filtering, to say which category a match is in.
            Text {
                visible: entry.heading
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 3
                text: entry.heading ? entry.row.label : ""
                color: Theme.inkDim
                font.family: Theme.uiFont
                font.pixelSize: Theme.sizeSmall
                font.bold: true
            }

            RowLayout {
                visible: !entry.heading
                anchors.fill: parent
                anchors.leftMargin: 12 + (entry.row && !entry.heading ? entry.row.depth * 14 : 0)
                anchors.rightMargin: 10
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: !entry.row || entry.heading ? ""
                          : entry.parentEntry
                            ? (entry.open ? "▾ " : "▸ ") + entry.row.label
                              + "  (" + entry.row.children.length + ")"
                            : entry.band.name
                    color: entry.live ? Theme.ink : picker.inkOut
                    font.family: Theme.uiFont
                    font.pixelSize: entry.band && entry.band.kind === picker.kindAllocation
                                    ? Theme.sizeBody : Theme.sizeSmall
                }

                // Says why the row is grey rather than leaving it to the colour.
                Text {
                    visible: !entry.live
                    text: engineLink.sourceCanRetune ? "out of range" : "fixed"
                    color: Theme.inkDim
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.sizeSmall
                    font.italic: true
                }

                Text {
                    text: !entry.row || entry.heading ? ""
                          : picker.edges(entry.row.type === "parent" ? entry.row.low : entry.band.low,
                                         entry.row.type === "parent" ? entry.row.high : entry.band.high)
                    color: entry.live ? Theme.inkDim : picker.inkOut
                    font.family: Theme.monoFont
                    font.pixelSize: Theme.sizeSmall
                }

                Text {
                    Layout.preferredWidth: 34
                    text: entry.band ? entry.band.mode : ""
                    color: entry.live ? Theme.inkDim : picker.inkOut
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.sizeSmall
                }
            }

            Tip {
                visible: hover.containsMouse && entry.band !== null
                         && entry.band.source !== ""
                text: entry.band ? entry.band.source : ""
            }
        }
    }

    Popup {
        id: menu

        y: opener.y + opener.height + 4
        x: opener.x
        width: picker.groupWidth + 1 + picker.columnWidth + 2
        // Fixed rather than fitted to the right pane, so switching category
        // does not resize the menu under the pointer.
        height: 560
        padding: 1
        focus: true
        // Escape is handled below: first it clears a filter, then it closes.
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnPressOutsideParent

        // Category, scroll and expansion are kept from last time; only the
        // filter is dropped.
        onOpened: {
            keys.forceActiveFocus()
            picker.reveal()
        }
        onClosed: if (picker.filter !== "") picker.setFilter("")

        background: Rectangle {
            radius: Theme.radius
            color: Theme.panelSolid
            border.width: 1
            border.color: Theme.border
        }

        contentItem: ColumnLayout {
            id: keys

            spacing: 0
            focus: true

            Keys.onUpPressed: picker.moveTo(picker.curPos - 1)
            Keys.onDownPressed: picker.moveTo(picker.curPos + 1)
            Keys.onPressed: event => {
                const row = picker.curCol === 1 && picker.curPos >= 0
                            && picker.curPos < bandRows.count
                            ? picker.allRows[bandRows.get(picker.curPos).ri] : null
                if (event.key === Qt.Key_Escape) {
                    if (picker.filter !== "") picker.setFilter("")
                    else menu.close()
                    event.accepted = true
                } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                    const page = Math.max(1, Math.floor(flick.height / picker.rowHeight) - 1)
                    picker.moveTo(picker.curPos + (event.key === Qt.Key_PageDown ? page : -page))
                    event.accepted = true
                } else if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                    picker.moveTo(event.key === Qt.Key_Home ? 0 : 1e9)
                    event.accepted = true
                } else if (event.key === Qt.Key_Backspace) {
                    if (picker.filter !== "") picker.setFilter(picker.filter.slice(0, -1))
                    event.accepted = true
                } else if (event.key === Qt.Key_Right) {
                    if (picker.curCol === 0) {
                        if (bandRows.count > 0) { picker.curCol = 1; picker.curPos = 0; picker.reveal() }
                    } else if (row && row.type === "parent" && !picker.isOpen(row)) {
                        picker.toggle(row)
                    }
                    event.accepted = true
                } else if (event.key === Qt.Key_Left) {
                    if (row && row.type === "parent" && picker.isOpen(row) && picker.filter === "")
                        picker.toggle(row)
                    else if (picker.curCol === 1 && picker.filter === "") {
                        picker.curCol = 0
                        picker.curPos = picker.groups.indexOf(picker.selectedGroup)
                    }
                    event.accepted = true
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                           || (event.key === Qt.Key_Space && picker.filter === "")) {
                    if (picker.curCol === 0) {
                        if (bandRows.count > 0) { picker.curCol = 1; picker.curPos = 0; picker.reveal() }
                    } else if (row) {
                        picker.activate(row)
                    }
                    event.accepted = true
                } else if (event.text.length === 1 && event.text >= " "
                           && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier))) {
                    picker.setFilter(picker.filter + event.text)
                    event.accepted = true
                }
            }

            Text {
                visible: picker.filter !== ""
                Layout.fillWidth: true
                Layout.preferredHeight: 22
                leftPadding: 10
                verticalAlignment: Text.AlignVCenter
                text: "filter: " + picker.filter
                color: Theme.ink
                font.family: Theme.monoFont
                font.pixelSize: Theme.sizeSmall
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Column {
                    id: groupColumn

                    Layout.preferredWidth: picker.groupWidth
                    Layout.alignment: Qt.AlignTop
                    topPadding: 4

                    Repeater {
                        model: picker.groups

                        Item {
                            id: groupEntry

                            required property string modelData
                            required property int index

                            readonly property bool selected: picker.filter === ""
                                                             && modelData === picker.selectedGroup
                            readonly property bool dimmed: picker.filter !== ""
                                                           && !picker.matchedGroups[modelData]

                            width: picker.groupWidth
                            height: picker.rowHeight + 4

                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                radius: Theme.radius
                                color: groupEntry.selected || groupHover.containsMouse
                                       ? Theme.controlHover : "transparent"
                                border.width: groupEntry.selected && picker.curCol === 0 ? 1 : 0
                                border.color: Theme.border
                            }

                            // The selected category's marker, so it reads as
                            // the one feeding the right pane.
                            Rectangle {
                                visible: groupEntry.selected
                                x: 4
                                width: 3
                                height: parent.height - 8
                                anchors.verticalCenter: parent.verticalCenter
                                radius: 1
                                color: Theme.ink
                            }

                            Text {
                                anchors.fill: parent
                                anchors.leftMargin: 14
                                anchors.rightMargin: 8
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                text: groupEntry.modelData
                                color: groupEntry.dimmed ? picker.inkOut
                                       : groupEntry.selected ? Theme.ink : Theme.inkDim
                                font.family: Theme.uiFont
                                font.pixelSize: Theme.sizeBody
                                font.bold: groupEntry.selected
                            }

                            MouseArea {
                                id: groupHover

                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: picker.selectGroup(groupEntry.modelData)
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.fillHeight: true
                    color: Theme.border
                }

                Flickable {
                    id: flick

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: width
                    contentHeight: bandColumn.height
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: RScrollBar {}

                    Column {
                        id: bandColumn

                        Repeater {
                            id: bandRepeater

                            model: bandRows
                            delegate: rowDelegate
                        }
                    }
                }
            }
        }
    }
}
