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

    // Re-read when the region changes in the settings window. UiRules.bands()
    // reads the stored region, which BandPlanSettings writes before it
    // signals, and naming the property here is what makes the binding depend
    // on it.
    readonly property var bands: {
        BandPlanSettings.region
        return UiRules.bands()
    }

    // Parent entries opened in the menu, keyed group + parent.
    property var expanded: ({})

    // The menu's flat list: group headings, bands, and parent entries, with
    // the open parents' channels spliced in after them.
    readonly property var rows: picker.flatten(picker.bands, picker.expanded)

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

    function flatten(bands, open) {
        const out = []
        const parents = {}
        let group = null
        let allocation = null

        for (let i = 0; i < bands.length; ++i) {
            const band = bands[i]
            if (band.group !== group) {
                group = band.group
                allocation = null
                out.push({ type: "group", label: group })
            }

            if (band.parent !== "") {
                const key = band.group + "\u0001" + band.parent
                let entry = parents[key]
                if (!entry) {
                    entry = { type: "parent", key: key, label: band.parent,
                              low: band.low, high: band.high, children: [],
                              depth: 0 }
                    parents[key] = entry
                    out.push(entry)
                }
                entry.low = Math.min(entry.low, band.low)
                entry.high = Math.max(entry.high, band.high)
                entry.children.push(band)
                continue
            }

            let depth = 0
            if (band.kind === picker.kindAllocation) {
                allocation = band
            } else if (allocation && band.low >= allocation.low
                       && band.high <= allocation.high) {
                depth = 1
            }
            out.push({ type: "band", band: band, depth: depth })
        }

        // Splice each open parent's channels in after it.
        const result = []
        for (const row of out) {
            result.push(row)
            if (row.type === "parent" && open[row.key]) {
                for (const child of row.children)
                    result.push({ type: "band", band: child, depth: 1 })
            }
        }
        return result
    }

    function rowEnabled(row) {
        if (row.type === "band") return picker.reachable(row.band)
        if (row.type === "parent")
            return row.children.some(child => picker.reachable(child))
        return false
    }

    function toggle(key) {
        const next = Object.assign({}, picker.expanded)
        if (next[key]) delete next[key]
        else next[key] = true
        picker.expanded = next
    }

    function activate(row) {
        if (row.type === "parent") picker.toggle(row.key)
        else if (row.type === "band" && picker.reachable(row.band)) picker.tuneTo(row.band)
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

    Popup {
        id: menu

        y: opener.y + opener.height + 4
        x: opener.x
        width: 380
        height: Math.min(list.contentHeight + 2, 560)
        padding: 1
        focus: true

        onOpened: list.forceActiveFocus()

        background: Rectangle {
            radius: Theme.radius
            color: Theme.panelSolid
            border.width: 1
            border.color: Theme.border
        }

        contentItem: ListView {
            id: list

            clip: true
            model: picker.rows
            currentIndex: -1
            keyNavigationEnabled: false
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            ScrollBar.vertical: RScrollBar {}

            // Step over headings, which take no action.
            function step(delta) {
                let i = list.currentIndex
                for (;;) {
                    i += delta
                    if (i < 0 || i >= picker.rows.length) return
                    if (picker.rows[i].type !== "group") break
                }
                list.currentIndex = i
                list.positionViewAtIndex(i, ListView.Contain)
            }

            Keys.onUpPressed: step(-1)
            Keys.onDownPressed: step(1)
            Keys.onPressed: event => {
                const row = picker.rows[list.currentIndex]
                if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                    const page = Math.max(1, Math.floor(list.height / 24) - 1)
                    for (let n = 0; n < page; ++n)
                        list.step(event.key === Qt.Key_PageDown ? 1 : -1)
                    event.accepted = true
                } else if (!row) {
                    return
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                           || event.key === Qt.Key_Space) {
                    picker.activate(row)
                    event.accepted = true
                } else if (event.key === Qt.Key_Right && row.type === "parent"
                           && !picker.expanded[row.key]) {
                    picker.toggle(row.key)
                    event.accepted = true
                } else if (event.key === Qt.Key_Left) {
                    // On an open parent, close it; on one of its channels,
                    // go back up to it.
                    if (row.type === "parent" && picker.expanded[row.key]) {
                        picker.toggle(row.key)
                    } else if (row.type === "band" && row.band.parent !== "") {
                        for (let i = list.currentIndex; i >= 0; --i) {
                            if (picker.rows[i].type === "parent") {
                                list.currentIndex = i
                                break
                            }
                        }
                    }
                    event.accepted = true
                }
            }

            delegate: Item {
                id: entry

                required property var modelData
                required property int index

                readonly property bool heading: modelData.type === "group"
                readonly property bool parentEntry: modelData.type === "parent"
                readonly property bool open: parentEntry && !!picker.expanded[modelData.key]
                readonly property bool live: picker.rowEnabled(modelData)
                readonly property bool current: ListView.isCurrentItem
                readonly property var band: modelData.type === "band" ? modelData.band : null

                width: ListView.view.width
                height: heading ? 26 : 24

                Text {
                    visible: entry.heading
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 3
                    text: entry.heading ? entry.modelData.label : ""
                    color: Theme.inkDim
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.sizeSmall
                    font.bold: true
                }

                Rectangle {
                    visible: !entry.heading
                    anchors.fill: parent
                    color: (hover.containsMouse || entry.current) && entry.live
                           ? Theme.controlHover : "transparent"
                }

                MouseArea {
                    id: hover

                    enabled: !entry.heading
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: entry.live ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onEntered: list.currentIndex = entry.index
                    onClicked: picker.activate(entry.modelData)
                }

                RowLayout {
                    visible: !entry.heading
                    anchors.fill: parent
                    anchors.leftMargin: 16 + (entry.heading ? 0 : entry.modelData.depth * 14)
                    anchors.rightMargin: 10
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: entry.heading ? ""
                              : entry.parentEntry
                                ? (entry.open ? "▾ " : "▸ ") + entry.modelData.label
                                  + "  (" + entry.modelData.children.length + ")"
                                : entry.band.name
                        color: entry.live ? Theme.ink : Theme.inkOff
                        font.family: Theme.uiFont
                        font.pixelSize: entry.band && entry.band.kind === picker.kindAllocation
                                        ? Theme.sizeBody : Theme.sizeSmall
                    }

                    Text {
                        text: entry.heading ? ""
                              : entry.parentEntry
                                ? picker.edges(entry.modelData.low, entry.modelData.high)
                                : picker.edges(entry.band.low, entry.band.high)
                        color: entry.live ? Theme.inkDim : Theme.inkOff
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }

                    Text {
                        Layout.preferredWidth: 34
                        text: entry.band ? entry.band.mode : ""
                        color: entry.live ? Theme.inkDim : Theme.inkOff
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
    }
}
