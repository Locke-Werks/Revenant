// What the engine is.
//
// Every label in this row sets Layout.minimumWidth and elides.
// Without that a RowLayout's minimum width is the sum of what its
// children want, the ColumnLayout inherits it, and a window narrower
// than that gets a layout laid out at its minimum instead: the
// waterfall is then drawn wider than the window and its right-hand
// end is off-screen, while the axis underneath still claims the
// whole span. Part of the band silently missing while the labels say
// otherwise is the one failure a spectrum display must not have, and
// it turned up at 640 pixels wide.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

RowLayout {
    Layout.fillWidth: true
    spacing: 18

    Label {
        Layout.minimumWidth: 0
        text: engineLink.connected
              ? engineLink.deviceName + "  (" + engineLink.deviceVendor + ")"
              : "waiting for an engine at " + engineLink.endpoint
        color: engineLink.connected ? Theme.ink : Theme.inkWarn
        font.pixelSize: Theme.sizeTitle
        font.bold: true
        elide: Text.ElideRight
    }

    Label {
        Layout.minimumWidth: 0
        visible: engineLink.connected
        text: engineLink.sourceRate + " S/s source  ·  "
              + engineLink.channelRate + " S/s channel  ·  "
              + engineLink.gridChannels + " channels  ·  "
              + engineLink.bins + " bins"
        color: Theme.inkDim
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }

    Item { Layout.fillWidth: true }

    // WHETHER THE ENGINE IS RUNNING, WHICH IS NOT WHETHER IT IS
    // REACHABLE
    //
    // An engine answers RPC calls from the moment it binds its port,
    // which core/engine/engine.cpp does before run() and leaves true
    // after the source ends. So an engine that is up with its graph
    // stopped is connected, has geometry, and draws 0.0 rows/s, and
    // without this it presents as a healthy engine on a dead band.
    // Those two need different actions and the row has to say which
    // it is.
    //
    // Paired with the rate rather than given a row of its own,
    // because the pair is the diagnosis: running with 0.0 rows/s is
    // a source that has stopped delivering, stopped with 0.0 is an
    // engine waiting to be started, and both used to look the same.
    Label {
        Layout.minimumWidth: 0
        visible: engineLink.connected
        text: engineLink.engineRunning
              ? engineLink.frameRate.toFixed(1) + " rows/s"
              : "engine stopped  ·  " + engineLink.frameRate.toFixed(1) + " rows/s"
        color: engineLink.engineRunning ? Theme.inkDim : Theme.inkWarn
        font.pixelSize: Theme.sizeBody
        font.bold: !engineLink.engineRunning
        elide: Text.ElideRight
    }

    // The two losses named apart, because they have different
    // fixes and the sum on its own points at neither. "not drawn" is
    // this client replacing a frame in the hand-off slot before the
    // GUI thread came for it, which means the GUI thread is the
    // limit; "engine dropped" means the engine had a frame at the
    // rate asked for and threw it away because this client had not
    // answered for the previous one.
    Label {
        Layout.minimumWidth: 0
        visible: engineLink.connected
        text: engineLink.framesReceived + " frames  ·  "
              + engineLink.framesDroppedByEngine + " engine dropped  ·  "
              + engineLink.framesDroppedByUi + " not drawn"
        color: engineLink.framesDropped > 0 ? Theme.inkWarn : Theme.inkDim
        font.pixelSize: Theme.sizeBody
        elide: Text.ElideRight
    }
}
