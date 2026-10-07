// The bands: a few one-click shortcuts, and every band in a grouped menu.
//
// The table is models/band_plan.h, with the regulations it was taken from and
// cases in ui/tests. A click puts the front end at the band's centre, which
// the table chooses as the part of the band worth landing on rather than the
// middle of the allocation.
//
// A band the open device cannot be put on is greyed rather than hidden, so
// the menu is the same list on every radio and says which parts of it this
// one reaches. On a source that cannot retune every row is grey, and the
// tuning row says why beside the dial.
//
// The table runs to several hundred rows, most of them channels, so channels
// fold under one entry per parent (CB channels, FRS/GMRS channels) that opens
// in place, and a sub-band sits indented under the allocation it lies within.
// Group headings fold too, and the groups flow into two columns that scroll
// as one view.
//
// Opening or closing an entry must not move anything above it. The rows live
// in two ListModels that are edited in place (rows inserted or removed after
// the entry) rather than reassigned, because handing a view a new model array
// resets it: scroll to the top, current row gone.

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
    readonly property int columnWidth: 420

    // Re-read when the region changes in the settings window. UiRules.bands()
    // reads the stored region, which BandPlanSettings writes before it
    // signals, and naming the property here is what makes the binding depend
    // on it.
    readonly property var bands: {
        BandPlanSettings.region
        return UiRules.bands()
    }

    // Every row the menu can show, in order, built once per region. The
    // models hold indices into it ("ri"), so a row's identity survives any
    // amount of opening, closing and filtering.
    property var allRows: []

    // Open parents and closed groups, by key. They live here rather than in
    // the popup so they outlast a close and reopen.
    property var expanded: ({})
    property var closedGroups: ({})

    // Typed while the menu is open; narrows the rows to matching names.
    property string filter: ""

    // The current row as column and position within that column's model.
    property int curCol: 0
    property int curPos: -1

    readonly property var columnModels: [leftRows, rightRows]

    ListModel { id: leftRows }
    ListModel { id: rightRows }

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
        engineLink.tuneSourceHz(band.centre)
        menu.close()
    }

    // Both edges in the larger edge's unit, so "7 to 7.3 MHz" reads as one span.
    function edges(low, high) {
        const unit = high >= 1e9 ? 1e9 : high >= 1e6 ? 1e6 : 1e3
        const name = unit === 1e9 ? " GHz" : unit === 1e6 ? " MHz" : " kHz"
        return +(low / unit).toFixed(6) + "–" + +(high / unit).toFixed(6) + name
    }

    function build(bands) {
        const out = []
        const parents = {}
        let group = null
        let groupRow = null
        let allocation = null

        for (let i = 0; i < bands.length; ++i) {
            const band = bands[i]
            if (band.group !== group) {
                group = band.group
                allocation = null
                groupRow = { type: "group", key: group, label: group, group: group,
                             col: 0, text: group.toLowerCase() }
                out.push(groupRow)
            }

            if (band.parent !== "") {
                const key = band.group + "\u0001" + band.parent
                let entry = parents[key]
                if (!entry) {
                    entry = { type: "parent", key: key, label: band.parent,
                              group: group, col: 0, low: band.low, high: band.high,
                              children: [], depth: 0,
                              text: band.parent.toLowerCase() }
                    parents[key] = entry
                    out.push(entry)
                }
                entry.low = Math.min(entry.low, band.low)
                entry.high = Math.max(entry.high, band.high)
                entry.children.push({ type: "band", band: band, depth: 1, group: group,
                                      col: 0, parentKey: key,
                                      text: (band.name + " " + band.parent).toLowerCase() })
                continue
            }

            let depth = 0
            if (band.kind === picker.kindAllocation) {
                allocation = band
            } else if (allocation && band.low >= allocation.low
                       && band.high <= allocation.high) {
                depth = 1
            }
            out.push({ type: "band", band: band, depth: depth, group: group, col: 0,
                       parentKey: "", text: band.name.toLowerCase() })
        }

        // Channels go straight after their parent, which is where they open.
        const rows = []
        for (const row of out) {
            rows.push(row)
            if (row.type === "parent")
                for (const child of row.children) rows.push(child)
        }

        // Split the groups between the columns by their folded height, keeping
        // reading order (left top to bottom, then right). The split is fixed
        // per region so opening an entry never moves a group across.
        const heights = {}
        let total = 0
        for (const row of rows) {
            const h = row.type === "group" ? picker.headingHeight
                    : row.type === "band" && row.parentKey !== "" ? 0 : picker.rowHeight
            heights[row.group] = (heights[row.group] || 0) + h
            total += h
        }
        let before = 0
        const colOf = {}
        for (const row of rows) {
            if (row.type !== "group") continue
            colOf[row.group] = before + heights[row.group] / 2 <= total / 2 ? 0 : 1
            before += heights[row.group]
        }
        for (const row of rows) row.col = colOf[row.group]
        return rows
    }

    // Which rows should be in the models now, as ascending indices into allRows.
    function wanted() {
        const rows = picker.allRows
        const out = []
        const f = picker.filter.toLowerCase()
        if (f === "") {
            for (let i = 0; i < rows.length; ++i) {
                const row = rows[i]
                if (row.type !== "group" && picker.closedGroups[row.group]) continue
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
        const m = picker.columnModels[picker.curCol]
        return picker.curPos >= 0 && picker.curPos < m.count ? m.get(picker.curPos).ri : -1
    }

    // Brings each model to the wanted rows with inserts and removes only, so
    // rows that stay keep their delegates and the view keeps its scroll.
    function sync() {
        const keepRi = picker.currentRi()
        const want = picker.wanted()
        const perCol = [[], []]
        for (const ri of want) perCol[picker.allRows[ri].col].push(ri)

        for (let c = 0; c < 2; ++c) {
            const m = picker.columnModels[c]
            const need = perCol[c]
            let i = 0
            let j = 0
            while (i < m.count || j < need.length) {
                const have = i < m.count ? m.get(i).ri : Infinity
                const next = j < need.length ? need[j] : Infinity
                if (have === next) {
                    ++i; ++j
                } else if (have < next) {
                    let n = 1
                    while (i + n < m.count && m.get(i + n).ri < next) ++n
                    m.remove(i, n)
                } else {
                    m.insert(i, { ri: next })
                    ++i; ++j
                }
            }
        }
        picker.restoreCurrent(keepRi)
    }

    // Puts the cursor back on the row it was on, or on the entry that row
    // folded into.
    function restoreCurrent(ri) {
        if (ri < 0) { picker.curPos = -1; return }
        const row = picker.allRows[ri]
        const owners = [ri]
        if (row.type === "band" && row.parentKey !== "")
            owners.push(picker.allRows.findIndex(r => r.key === row.parentKey
                                                      && r.type === "parent"))
        owners.push(picker.allRows.findIndex(r => r.type === "group" && r.group === row.group))
        const m = picker.columnModels[row.col]
        for (const target of owners) {
            for (let i = 0; i < m.count; ++i) {
                if (m.get(i).ri === target) {
                    picker.curCol = row.col
                    picker.curPos = i
                    return
                }
            }
        }
        picker.curPos = -1
    }

    function rebuild() {
        picker.allRows = picker.build(picker.bands)
        leftRows.clear()
        rightRows.clear()
        picker.curPos = -1
        picker.sync()
    }

    function rowEnabled(row) {
        if (row.type === "band") return picker.reachable(row.band)
        if (row.type === "parent")
            return row.children.some(child => picker.reachable(child))
        return true
    }

    function isOpen(row) {
        if (row.type === "parent") return picker.filter !== "" || !!picker.expanded[row.key]
        if (row.type === "group") return picker.filter !== "" || !picker.closedGroups[row.group]
        return false
    }

    function toggle(row) {
        // Folding is meaningless while a filter has everything open.
        if (picker.filter !== "") return
        if (row.type === "parent") {
            const next = Object.assign({}, picker.expanded)
            if (next[row.key]) delete next[row.key]
            else next[row.key] = true
            picker.expanded = next
        } else if (row.type === "group") {
            const next = Object.assign({}, picker.closedGroups)
            if (next[row.group]) delete next[row.group]
            else next[row.group] = true
            picker.closedGroups = next
        }
        picker.sync()
    }

    function activate(row) {
        if (row.type === "parent" || row.type === "group") picker.toggle(row)
        else if (picker.reachable(row.band)) picker.tuneTo(row.band)
    }

    function setFilter(text) {
        picker.filter = text
        picker.sync()
        if (picker.curPos < 0) {
            picker.curCol = leftRows.count > 0 ? 0 : 1
            picker.curPos = picker.columnModels[picker.curCol].count > 0 ? 0 : -1
        }
        picker.reveal()
    }

    // Scrolls just enough to show the current row.
    function reveal() {
        const item = (picker.curCol === 0 ? leftRepeater : rightRepeater).itemAt(picker.curPos)
        if (!item) return
        if (item.y < flick.contentY) flick.contentY = item.y
        else if (item.y + item.height > flick.contentY + flick.height)
            flick.contentY = item.y + item.height - flick.height
    }

    function moveTo(col, pos) {
        const m = picker.columnModels[col]
        if (m.count === 0) return
        picker.curCol = col
        picker.curPos = Math.max(0, Math.min(m.count - 1, pos))
        picker.reveal()
    }

    // Across to the row level with the current one in the other column.
    function crossTo(col) {
        if (col === picker.curCol || picker.columnModels[col].count === 0) return
        const from = (picker.curCol === 0 ? leftRepeater : rightRepeater).itemAt(picker.curPos)
        const y = from ? from.y : flick.contentY
        const rep = col === 0 ? leftRepeater : rightRepeater
        let best = 0
        for (let i = 0; i < rep.count; ++i) {
            const it = rep.itemAt(i)
            if (it && it.y <= y) best = i
        }
        picker.moveTo(col, best)
    }

    spacing: 2

    Repeater {
        model: picker.bands

        RButton {
            required property var modelData

            visible: modelData.favourite
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
            property int col: 0

            readonly property var row: picker.allRows[ri] ?? null
            readonly property bool heading: row !== null && row.type === "group"
            readonly property bool parentEntry: row !== null && row.type === "parent"
            readonly property bool open: row !== null && picker.isOpen(row)
            readonly property bool live: row !== null && picker.rowEnabled(row)
            readonly property bool current: picker.curCol === col && picker.curPos === index
            readonly property var band: row !== null && row.type === "band" ? row.band : null

            width: picker.columnWidth
            height: heading ? picker.headingHeight : picker.rowHeight

            Rectangle {
                anchors.fill: parent
                color: (hover.containsMouse || entry.current) && (entry.live || entry.heading)
                       ? Theme.controlHover : "transparent"
                // The cursor on an out-of-range row has no fill to show it,
                // so it gets an outline instead.
                border.width: entry.current && !entry.live ? 1 : 0
                border.color: Theme.border
            }

            MouseArea {
                id: hover

                anchors.fill: parent
                hoverEnabled: true
                cursorShape: entry.live ? Qt.PointingHandCursor : Qt.ArrowCursor
                onEntered: {
                    picker.curCol = entry.col
                    picker.curPos = entry.index
                }
                onClicked: picker.activate(entry.row)
            }

            Text {
                visible: entry.heading
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 3
                text: entry.heading ? (entry.open ? "▾ " : "▸ ") + entry.row.label : ""
                color: Theme.inkDim
                font.family: Theme.uiFont
                font.pixelSize: Theme.sizeSmall
                font.bold: true
            }

            RowLayout {
                visible: !entry.heading
                anchors.fill: parent
                anchors.leftMargin: 16 + (entry.row && !entry.heading ? entry.row.depth * 14 : 0)
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
        width: picker.columnWidth * 2 + 1 + 2
        height: Math.min(body.implicitHeight + 2, 600)
        padding: 1
        focus: true
        // Escape is handled below: first it clears a filter, then it closes.
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnPressOutsideParent

        // Scroll and expansion are kept from last time; only the filter is
        // dropped, and the current row brought back into view.
        onOpened: {
            flick.forceActiveFocus()
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
            id: body

            spacing: 0

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

            Flickable {
                id: flick

                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: contentHeight
                clip: true
                contentWidth: width
                contentHeight: Math.max(leftColumn.height, rightColumn.height)
                boundsBehavior: Flickable.StopAtBounds
                focus: true
                ScrollBar.vertical: RScrollBar {}

                Keys.onUpPressed: picker.moveTo(picker.curCol, picker.curPos - 1)
                Keys.onDownPressed: picker.moveTo(picker.curCol, picker.curPos + 1)
                Keys.onPressed: event => {
                    const m = picker.columnModels[picker.curCol]
                    const row = picker.curPos >= 0 && picker.curPos < m.count
                                ? picker.allRows[m.get(picker.curPos).ri] : null
                    const foldable = row !== null
                                     && (row.type === "parent" || row.type === "group")
                                     && picker.filter === ""
                    if (event.key === Qt.Key_Escape) {
                        if (picker.filter !== "") picker.setFilter("")
                        else menu.close()
                        event.accepted = true
                    } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                        const page = Math.max(1, Math.floor(flick.height / picker.rowHeight) - 1)
                        picker.moveTo(picker.curCol, picker.curPos
                                      + (event.key === Qt.Key_PageDown ? page : -page))
                        event.accepted = true
                    } else if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                        picker.moveTo(picker.curCol, event.key === Qt.Key_Home ? 0 : m.count - 1)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Backspace) {
                        if (picker.filter !== "") picker.setFilter(picker.filter.slice(0, -1))
                        event.accepted = true
                    } else if (event.key === Qt.Key_Right) {
                        if (foldable && !picker.isOpen(row)) picker.toggle(row)
                        else picker.crossTo(1)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Left) {
                        if (foldable && picker.isOpen(row)) picker.toggle(row)
                        else picker.crossTo(0)
                        event.accepted = true
                    } else if (row && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                       || (event.key === Qt.Key_Space && picker.filter === ""))) {
                        picker.activate(row)
                        event.accepted = true
                    } else if (event.text.length === 1 && event.text >= " "
                               && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier))) {
                        if (picker.curPos < 0 && m.count > 0) picker.curPos = 0
                        picker.setFilter(picker.filter + event.text)
                        event.accepted = true
                    }
                }

                Row {
                    spacing: 1

                    Column {
                        id: leftColumn

                        Repeater {
                            id: leftRepeater

                            model: leftRows
                            delegate: rowDelegate
                        }
                    }

                    Rectangle {
                        width: 1
                        height: flick.contentHeight
                        color: Theme.border
                    }

                    Column {
                        id: rightColumn

                        Repeater {
                            id: rightRepeater

                            model: rightRows
                            delegate: rowDelegate
                            onItemAdded: (i, item) => item.col = 1
                        }
                    }
                }
            }
        }
    }
}
