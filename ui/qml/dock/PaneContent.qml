// What a pane holds: the display or the controls, plus what the frame around
// them needs to know. Every pane's content is one of these, made once by
// whoever owns it and handed to a PaneHost, which re-parents it into a frame.

import QtQuick

Item {
    // The title on the pane's strip.
    property string title: ""

    // What the content needs to be drawn whole, at a scale of one, title
    // strip excluded. The store takes the largest it has seen this session,
    // so text that grows and shrinks does not move a border; see
    // models/pane_layout_store.h.
    property real needWidth: 0
    property real needHeight: 0

    // Whether there is anything to show. A pane with nothing steps aside and
    // its neighbours take the room, and it comes back where it was.
    property bool paneShown: true
}
