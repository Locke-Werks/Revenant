// The receivers: the rack of receiver strips, the focused receiver's controls
// and fine-tuning display, and its RDS, decoding and audio.
//
// ONE PANEL, DRAWN IN ONE OF TWO PLACES. Docked under the span in the main
// window, which is where it starts, or popped out into the receiver window
// for a second screen. Main.qml moves this one item between the two, so the
// fine-tuning waterfall keeps its history and the rack its scroll position
// across a pop; models/receiver_placement.h has the owner's call behind it.
//
// TILED, SINCE 2026-11-09. Each part is a pane of its own in the tiled layout
// (models/pane_layout.h): the rack, the receiver, and RDS, decoding, images,
// calls, trunking and audio each, any of which can go anywhere in the window
// holding them. This item draws nothing. It makes the contents once and hands
// them to whichever window has the receivers, so a pop moves them rather
// than rebuilding them. A section with nothing to show steps out of the
// layout until it has.
//
// WHAT EACH NEEDS. A pane's width is measured while its content is in its
// folded arrangement, the narrowest it reads at; measured wide, it would be a
// minimum that kept the content from ever folding. The store keeps the
// largest it has seen, so nothing moves as the words in it change.

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

    // Pane name to its PaneContent, for a PaneHost.
    readonly property var contents: ({
        "rack": rackPane, "detail": detailPane, "rds": rdsPane, "decode": decodePane,
        "image": imagePane, "calls": callsPane, "trunk": trunkPane, "audio": audioPane
    })

    // The panes with nothing to show right now.
    readonly property var empty: {
        const out = []
        for (const name in contents) {
            if (!contents[name].paneShown)
                out.push(name)
        }
        return out
    }

    // Breathing room inside each pane.
    readonly property real inset: Theme.gap

    visible: false

    // A width taken while the content is folded. See WHAT EACH NEEDS above.
    component Measured: PaneContent {
        id: measured
        default property alias body: inner.data
        // True for content with no folded arrangement, whose width is always
        // what it needs.
        property bool folded: true
        property real foldedWidth: 0
        property Item measure: null
        property real fallbackWidth: 240

        function take() {
            if (folded && measure)
                foldedWidth = Math.max(foldedWidth, Math.ceil(measure.implicitWidth))
        }

        anchors.fill: parent
        needWidth: Math.max(fallbackWidth, foldedWidth) + 2 * panel.inset
        needHeight: (measure ? measure.implicitHeight : 0) + 2 * panel.inset
        onFoldedChanged: take()

        Connections {
            target: measured.measure
            function onImplicitWidthChanged() { measured.take() }
        }

        Item {
            id: inner
            anchors.fill: parent
            anchors.margins: panel.inset
        }
    }

    PaneContent {
        id: rackPane
        anchors.fill: parent
        title: "Receivers"
        needWidth: 250 + 2 * panel.inset
        needHeight: (rack.pills ? 80 : 150) + 2 * panel.inset

        // Scrolled inside, so it is usable at any height.
        ReceiverRack {
            id: rack
            anchors.fill: parent
            anchors.margins: panel.inset
            poppedOut: panel.poppedOut
            onPopToggled: panel.popToggled()
        }
    }

    Measured {
        id: detailPane
        title: "Receiver"
        paneShown: engineLink.receiverId > 0
        measure: detail
        folded: detail.narrow
        fallbackWidth: 300

        ReceiverDetail {
            id: detail
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Math.max(implicitHeight, parent.height)
            narrow: width < 520
        }
    }

    Measured {
        id: rdsPane
        title: "RDS"
        paneShown: rds.shown
        measure: rds
        folded: rds.compact
        RdsPane { id: rds; anchors.fill: parent }
    }

    Measured {
        id: decodePane
        title: "Decoding"
        paneShown: decode.shown
        measure: decode
        folded: decode.compact
        DecodePane { id: decode; anchors.fill: parent; fills: true }
    }

    Measured {
        id: imagePane
        title: "Images"
        paneShown: image.hasImages
        measure: image
        ImagePane { id: image; anchors.fill: parent }
    }

    Measured {
        id: callsPane
        title: "Calls"
        paneShown: calls.shown
        measure: calls
        CallsPane { id: calls; anchors.fill: parent }
    }

    Measured {
        id: trunkPane
        title: "Trunking"
        paneShown: trunk.shown
        measure: trunk
        TrunkPanel { id: trunk; anchors.fill: parent }
    }

    Measured {
        id: audioPane
        title: "Audio"
        paneShown: audio.shown
        measure: audio
        folded: audio.compact
        AudioPane { id: audio; anchors.fill: parent }
    }
}
