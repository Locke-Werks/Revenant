// The window. Layout and binding only.
//
// Every number shown here is read from a property on EngineLink or on one of
// the two render items. Nothing is computed in this file: the reduction
// correction in particular belongs to the item that applied it, because it
// depends on that item's width, and a copy of the arithmetic here would be a
// second answer to the same question.
//
// The one call that looks like an exception is not one. The axis asks
// EngineLink for the frequency at a fraction of the span, and that is a
// method rather than a property because only a display knows where its ticks
// are. The arithmetic behind it is still in one place, on the far side of
// that call, working from the rationals the wire carries.
//
// ONE FILE PER SECTION, AND THIS ONE ONLY STACKS THEM
//
// This was the whole interface in 2792 lines until 2026-09-22. Each section
// is now its own file beside this one and keeps the comments that explain
// its wording, so a reader looking for why a row says what it says opens the
// file named for the row. The palette and the type sizes are Theme.qml, and
// the selection the two span displays share is TuneSelection.qml, which also
// carries the note that used to open this file about why the selection lives
// outside both items.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ApplicationWindow {
    id: window

    width: 1280
    height: 800
    visible: true
    color: Theme.background
    title: engineLink.connected
           ? "Revenant  ·  " + engineLink.endpoint
           : "Revenant  ·  waiting for " + engineLink.endpoint

    TuneSelection {
        id: tuneSelection
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        StatusRow {}
        SourcePicker {}
        TuningPanel {}
        ReceiverNotices {}
        BookmarkRow {}
        EngineNotices {}
        DetectionControls {}

        // The spectrum, the waterfall and the axis under both, which is one
        // section because the axis labels those two and nothing else.
        SpanView {
            selection: tuneSelection
        }

        VfoPane {}
        RdsPane {}
        AudioPane {}

        ClickReadout {
            selection: tuneSelection
        }

        Label {
            Layout.fillWidth: true
            visible: engineLink.connected && !engineLink.spectrumEnabled
            text: "This engine was built with no spectrum stage, so there are no frames to draw."
            color: Theme.inkWarn
            font.pixelSize: Theme.sizeBody
        }
    }
}
