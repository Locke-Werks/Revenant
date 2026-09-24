// The row under the top bar: the front end's gain and the detector's three
// settings, always there, on one line.
//
// WHAT THIS ROW USED TO BE. A strip, ui/qml/NoticeBanner.qml, that existed
// only while a fault or a refusal held, as chips naming each with its sentence
// on hover. The owner, using it live on 2026-09-23: the chips were already up
// top beside the receivers button, since the status pill names the first of
// them, and the row would serve better holding a gain slider and the
// detector's sliders. The chips moved into the top bar beside the pill, once
// each; models/status_summary.h decides which is the pill and which are
// chips. This row holds the controls an operator adjusts while watching the
// band, so it does not come and go and the span under it stays put.
//
// ONE LINE AT THE WIDTHS THE CLIENT IS USED AT. Short labels, sliders of a
// fixed width and readouts that reserve their widest text, so the numbers
// neither jump nor push each other along as they change. The whole row is
// about 900 pixels, inside the 1280 the window opens at. Narrower than that
// the right-hand end is clipped rather than the row wrapping to two lines and
// moving the span down.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

Rectangle {
    id: row

    // For the detections key, which puts the arrow keys on the threshold.
    function focusThreshold() {
        detector.focusThreshold()
    }

    implicitHeight: 30
    color: Theme.panelSolid
    clip: true

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }

    RowLayout {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 12
        spacing: 14

        // Drawn with no engine as well, disabled, so the row and the span
        // under it do not move when one comes or goes.
        GainControl {}

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 16
            color: Theme.border
        }

        DetectionControls {
            id: detector
        }
    }
}
