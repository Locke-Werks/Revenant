// The receivers: the rack of receiver strips, the focused receiver's controls
// and fine-tuning display, and its RDS, decoding and audio.
//
// ONE PANEL, DRAWN IN ONE OF TWO PLACES. Docked under the span in the main
// window, which is where it starts, or popped out into the receiver window
// for a second screen. Main.qml moves this one item between the two, so the
// fine-tuning waterfall keeps its history and the rack its scroll position
// across a pop; models/receiver_placement.h has the owner's call behind it.
//
// TILED, SINCE 2026-11-09. The rack, the receiver and its sections are three
// panes of their own tree, models/pane_layout.h, which the operator resizes
// and rearranges and which travels with the panel between the dock and the
// receiver window. They replaced three fixed arrangements chosen from the
// panel's shape (narrow, stacked, wide). Each pane reports what its content
// needs as its minimum, so no pane is ever drawn smaller than its content and
// nothing is clipped; the window holding them refuses to shrink below the sum.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Item {
    id: panel

    // In the receiver window rather than docked, which the rack's button reads.
    property bool poppedOut: false

    // The rack's pop-out button was pressed.
    signal popToggled()

    // What the keys reach into here: the receiver's dial and its filter
    // display. See Commands.qml.
    readonly property alias dial: detail.dial
    readonly property alias passband: detail.passband

    // Inside the frames, the same breathing room the panel always had.
    readonly property real inset: panel.poppedOut ? 12 : Theme.gap

    // The smallest the panel can be, its margins included. Main.qml gives it
    // to the main window's tree, and the receiver window takes it as its own.
    readonly property real minimumWidth: receiverLayout.minimumWidth + 2 * host.anchors.margins
    readonly property real minimumHeight: receiverLayout.minimumHeight + 2 * host.anchors.margins

    // What each pane's content needs, which raises that pane's floor.
    component PaneMinimum: QtObject {
        required property string pane
        property real needWidth: 0
        property real needHeight: 0
        readonly property real strip: host ? host.stripHeight : 0
        function report() {
            receiverLayout.setMinimum(pane, needWidth + 2 * panel.inset,
                                      needHeight + strip + 2 * panel.inset)
        }
        onNeedWidthChanged: report()
        onNeedHeightChanged: report()
        onStripChanged: report()
        Component.onCompleted: report()
    }

    PaneMinimum { pane: "rack"; needWidth: 240; needHeight: 150 }
    PaneMinimum { pane: "detail"; needWidth: 300; needHeight: detail.implicitHeight }
    PaneMinimum { pane: "sections"; needWidth: panel.sectionsNeed; needHeight: sections.implicitHeight }

    // The sections' width when their rows have folded onto two lines each,
    // which is the narrowest they read at. AudioPane and RdsPane fold under
    // 700, so their width is measured only while folded: measured wide, it
    // would be a minimum that kept them from ever folding.
    property real sectionsNeed: 340
    function measureSections() {
        if (sections.width > 0 && sections.width < 700)
            sectionsNeed = Math.max(340, Math.ceil(sections.implicitWidth))
    }

    PaneHost {
        id: host
        anchors.fill: parent
        anchors.margins: panel.poppedOut ? 4 : 0
        store: receiverLayout
        contents: ({ "rack": rackPane, "detail": detailPane, "sections": sectionsPane })
        titles: ({ "rack": "Receivers", "detail": "Receiver", "sections": "RDS, decoding, audio" })
    }

    // Where the three displays wait until a frame takes them.
    Item {
        id: stash
        visible: false

        // The rack, scrolled inside, so it is usable at any height.
        Item {
            id: rackPane
            anchors.fill: parent

            ReceiverRack {
                anchors.fill: parent
                anchors.margins: panel.inset
                poppedOut: panel.poppedOut
                onPopToggled: panel.popToggled()
            }
        }

        Item {
            id: detailPane
            anchors.fill: parent

            ReceiverDetail {
                id: detail
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: panel.inset
                height: Math.max(implicitHeight, parent.height - 2 * panel.inset)
                narrow: width < 520
            }
        }

        Item {
            id: sectionsPane
            anchors.fill: parent

            // RDS, decoding and audio. The decode log takes what the column
            // has spare, so audio stays at the foot of it.
            ColumnLayout {
                id: sections

                onImplicitWidthChanged: panel.measureSections()
                onWidthChanged: panel.measureSections()
                anchors.fill: parent
                anchors.margins: panel.inset
                spacing: 10

                RdsPane {
                    Layout.minimumWidth: 0
                }

                DecodePane {
                    id: decode
                    Layout.minimumWidth: 0
                    fills: true
                }

                ImagePane {
                    Layout.minimumWidth: 0
                }

                CallsPane {
                    Layout.minimumWidth: 0
                }

                // P25 trunk following.
                TrunkPanel {
                    Layout.minimumWidth: 0
                }

                Item {
                    visible: !(decode.visible && decode.growing)
                    Layout.fillHeight: true
                }

                AudioPane {
                    Layout.minimumWidth: 0
                }
            }
        }
    }
}
