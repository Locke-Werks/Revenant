// Who is talking: digital voice calls, and the talkgroups and stations they
// were on, from every digital voice receiver this client knows and not only
// the focused one. The trunk tracker's voice receivers feed it as well.
//
// TWO VIEWS OF ONE TABLE. Calls is a row per transmission, newest first; Groups
// is a row per talkgroup, or per station where a protocol has no talkgroups,
// with how many calls it has carried. A live row is lit. A grant the control
// channel sent and no receiver followed stays as "granted", so a talkgroup the
// tracker is not following is still seen to be busy.
//
// ENCRYPTED IS A DIM CHIP, as in the decode log: the decoder read the header
// and says so, and nothing decrypts. models/call_log.h has every rule and
// ui/tests/test_call_log.cpp its cases.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: pane

    readonly property var table: engineLink.callLog
    property string view: "calls"
    readonly property var rows: view === "calls" ? table.calls : table.groups

    readonly property bool trunking: {
        const files = engineLink.pluginFiles;
        for (let i = 0; i < files.length; ++i) {
            if (files[i].running && files[i].name.indexOf("p25") === 0) {
                return true;
            }
        }
        return false;
    }

    Layout.fillWidth: true
    spacing: 4
    visible: table.feeds > 0 || trunking || table.calls.count > 0

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 8

        Label {
            text: "calls"
            color: Theme.inkDim
            font.bold: true
            font.pixelSize: Theme.sizeBody
        }

        RSegmented {
            options: ["calls", "groups"]
            current: pane.view
            onPicked: (option) => pane.view = option
        }

        Item { Layout.fillWidth: true }

        Readout {
            widest: "000 groups"
            text: pane.rows.count + " " + pane.view
            font.pixelSize: Theme.sizeSmall
        }

        RButton {
            flat: true
            text: "clear"
            ink: Theme.inkDim
            enabled: pane.table.calls.count > 0
            onClicked: pane.table.clear()
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.minimumHeight: 72
        Layout.preferredHeight: 160
        radius: Theme.radius
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border

        Label {
            anchors.centerIn: parent
            visible: pane.rows.count === 0
            text: "No calls heard yet."
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        TextMetrics {
            id: timeWidth
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeSmall
            text: "00:00:00"
        }

        TextMetrics {
            id: protocolWidth
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeSmall
            text: "D-STAR"
        }

        TextMetrics {
            id: idWidth
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeSmall
            text: "KF4FIC/ID51"
        }

        TextMetrics {
            id: frequencyWidth
            font.family: Theme.monoFont
            font.pixelSize: Theme.sizeSmall
            text: "0000.00000"
        }

        ListView {
            anchors.fill: parent
            anchors.margins: 4
            clip: true
            model: pane.rows
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            ScrollBar.vertical: RScrollBar {}

            delegate: Rectangle {
                id: entry

                required property string protocol
                required property string system
                required property string target
                required property string source
                required property bool group
                required property bool encrypted
                required property bool emergency
                required property string frequency
                required property string owner
                required property string lastHeard
                required property string duration
                required property bool active
                required property bool granted
                required property var count

                width: ListView.view.width
                height: 20
                radius: 2
                color: entry.active ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.14)
                                    : (hover.hovered ? Theme.controlHover : "transparent")

                HoverHandler { id: hover }

                RowLayout {
                    x: 4
                    width: parent.width - 8
                    height: parent.height
                    spacing: 10

                    Text {
                        Layout.preferredWidth: timeWidth.advanceWidth
                        text: entry.lastHeard
                        color: entry.active ? Theme.ink : Theme.inkDim
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }

                    Text {
                        Layout.preferredWidth: protocolWidth.advanceWidth
                        text: entry.protocol
                        color: entry.active ? Theme.accent : Theme.inkDim
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }

                    // Talkgroup, UR or destination. "TG" only where it is one.
                    Text {
                        Layout.preferredWidth: idWidth.advanceWidth
                        text: entry.target.length === 0 ? ""
                              : (entry.group && (entry.protocol === "P25" || entry.protocol === "DMR")
                                 ? "TG " + entry.target : entry.target)
                        color: entry.encrypted ? Theme.inkDim : Theme.ink
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.preferredWidth: idWidth.advanceWidth
                        text: entry.source
                        color: entry.encrypted ? Theme.inkDim : Theme.ink
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.preferredWidth: frequencyWidth.advanceWidth
                        text: entry.frequency
                        color: Theme.inkDim
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }

                    StatusChip {
                        visible: entry.emergency
                        label: "emergency"
                        ink: Theme.inkBad
                    }

                    StatusChip {
                        visible: entry.encrypted
                        label: "encrypted"
                        ink: Theme.inkDim
                    }

                    StatusChip {
                        visible: entry.granted
                        label: "granted"
                        detail: "The control channel granted this talkgroup a voice channel and no receiver followed it."
                        ink: Theme.inkDim
                    }

                    // The system, then whose receiver heard it, then how much.
                    Text {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: {
                            const parts = [];
                            if (entry.system.length > 0) {
                                parts.push(entry.system);
                            }
                            if (entry.owner.length > 0 && entry.owner !== "user") {
                                parts.push(entry.owner);
                            }
                            parts.push(pane.view === "calls"
                                       ? entry.duration
                                       : entry.count + (Number(entry.count) === 1 ? " call" : " calls"));
                            return parts.join("  ");
                        }
                        color: Theme.inkDim
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}
