// The settings window: the preferences that hold for the whole application
// rather than for one receiver, in one place. Opened from the top bar's
// settings button, the command palette, or Ctrl+Comma.
//
// DETECTION is the detector's three settings again, with what the control row
// cannot show: whether each value is this radio's own, every radio's, or the
// default, and a way back to the default. The threshold is kept per radio
// because a threshold is a statement about one front end's noise; see
// models/detector_scope.h.
//
// DISPLAY is the two fit sliders, live through ScaleSettings. Auto-scaling
// itself is always on; these only set how close it pulls the measured levels
// to the screen's edges. render/spectrum_scale.h, blend_ends.
//
// BAND PLAN is the bar over the span and which administrations' rows it and
// the band menu list, both live through BandPlanSettings.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: panel

    spacing: 10

    // Every slider takes its value from the link rather than binding to it,
    // for the reason DetectionControls.qml gives: a binding fights the drag.
    // So each is put back when the panel opens and whenever a reset or a share
    // moved the value under it.
    function sync() {
        thresholdRow.slider.value = engineLink.detectionThresholdWanted
        heldRow.slider.value = engineLink.confidenceBar
        marginRow.slider.value = engineLink.marginBar
        contrastRow.slider.value = ScaleSettings.waterfallContrast
        fitRow.slider.value = ScaleSettings.spectrumRangeFit
    }

    Component.onCompleted: sync()

    Connections {
        target: engineLink
        function onDetectorScopeChanged() { panel.sync() }
    }

    component SectionTitle: Label {
        font.pixelSize: Theme.sizeTitle
        font.bold: true
        color: Theme.ink
        Layout.topMargin: 2
    }

    component Rule: Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: Theme.border
    }

    // One detector setting: its name, a slider, the value, where the value
    // comes from, and the two actions.
    component DetectorRow: RowLayout {
        id: row

        required property string field
        required property string label
        required property string scope
        required property string tip
        property alias slider: rowSlider
        property real from: 0
        property real to: 1
        property real step: 0.01
        property var format: (v) => v.toFixed(2)
        signal moved(real value)

        spacing: 8
        Layout.fillWidth: true

        Label {
            text: row.label
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            Layout.minimumWidth: 70
        }

        RSlider {
            id: rowSlider
            Layout.preferredWidth: 180
            from: row.from
            to: row.to
            stepSize: row.step
            onMoved: row.moved(value)

            Tip {
                visible: parent.hovered
                delay: 400
                text: row.tip
            }
        }

        Readout {
            widest: "saturated"
            horizontalAlignment: Text.AlignLeft
            text: row.format(rowSlider.value)
            color: Theme.ink
        }

        // Where the value in use is kept. Warm when it is this radio's own,
        // since that is the one a second radio will not share.
        Label {
            text: row.scope
            color: row.scope === "this radio" ? Theme.accent
                   : row.scope === "all radios" ? Theme.ink
                   : Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            Layout.minimumWidth: 64
        }

        RButton {
            flat: true
            text: "use for all radios"
            ink: Theme.inkDim
            visible: row.scope === "this radio"
            onClicked: engineLink.shareDetectorSetting(row.field)
        }

        RButton {
            flat: true
            text: "reset to default"
            ink: Theme.inkDim
            enabled: row.scope !== "default"
            onClicked: engineLink.resetDetectorSetting(row.field)

            Tip {
                visible: parent.hovered
                text: row.scope === "this radio"
                      ? "Forget this radio's own value. It goes back to the value for all "
                        + "radios, or to the default when there is none."
                      : "Forget the value for all radios and go back to the default."
            }
        }

        Item { Layout.fillWidth: true }
    }

    SectionTitle { text: "detection" }

    Label {
        Layout.fillWidth: true
        Layout.maximumWidth: 640
        wrapMode: Text.WordWrap
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        text: engineLink.detectorRadio.length > 0
              ? "Values set now are kept for " + engineLink.detectorRadio
                + " and put back whenever it is opened. A radio with no value of its own "
                + "uses the value for all radios, or the default."
              : "The open source has no serial, so values set now are kept for all radios."
    }

    DetectorRow {
        id: thresholdRow
        field: "threshold"
        label: "threshold"
        scope: engineLink.thresholdScope
        from: -3.0
        to: 40.0
        step: 0.5
        format: (v) => v.toFixed(1) + " dB"
        tip: "The engine's detection threshold, in dB of SNR in 2500 Hz. Raise it when the "
             + "detector marks noise as signals. Shared by every client of the engine, and "
             + "the last writer wins."
        onMoved: (value) => engineLink.detectionThresholdDb = value
    }

    DetectorRow {
        id: heldRow
        field: "held"
        label: "held for"
        scope: engineLink.confidenceScope
        to: engineLink.maxConfidenceBar
        format: (v) => v >= engineLink.maxConfidenceBar ? "saturated" : v.toFixed(2)
        tip: "This window only. Hides tracks that have not been detected for long without a break."
        onMoved: (value) => engineLink.confidenceBar = Math.min(value, engineLink.maxConfidenceBar)
    }

    DetectorRow {
        id: marginRow
        field: "margin"
        label: "stronger"
        scope: engineLink.marginScope
        to: engineLink.maxConfidenceBar
        format: (v) => v < 0.5 ? "all" : v.toFixed(2)
        tip: "This window only. Hides detections that stood only a little above the threshold. "
             + "Below a half passes everything."
        onMoved: (value) => engineLink.marginBar = Math.min(value, engineLink.maxConfidenceBar)
    }

    Rule {}

    SectionTitle { text: "display" }

    // One fit slider: a name, the padded end, the slider, the tight end and
    // the value. Applied as it moves, since the point is to watch the picture.
    component FitRow: RowLayout {
        id: fit

        required property string label
        required property string tip
        property alias slider: fitSlider
        property real maximum: 1
        signal moved(real value)

        spacing: 8

        Label {
            text: fit.label
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            Layout.minimumWidth: 150
        }

        Label {
            text: "padded"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        RSlider {
            id: fitSlider
            Layout.preferredWidth: 180
            from: 0
            to: fit.maximum
            stepSize: 0.01
            onMoved: fit.moved(value)

            Tip {
                visible: parent.hovered
                delay: 400
                text: fit.tip
            }
        }

        Label {
            text: "tight"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        Readout {
            widest: "200%"
            horizontalAlignment: Text.AlignLeft
            text: Math.round(fitSlider.value * 100) + "%"
            color: Theme.ink
        }
    }

    FitRow {
        id: contrastRow
        label: "waterfall contrast"
        maximum: 2
        tip: "Padded is the usual look. 100% spreads the weakest to the strongest level in the "
             + "waterfall's history across the whole colour map. Past that the noise goes black "
             + "and strong signals saturate, so weak ones stand out. Pins still win."
        onMoved: (value) => ScaleSettings.waterfallContrast = value
    }

    FitRow {
        id: fitRow
        label: "spectrum range fit"
        tip: "Padded is the usual look. Pins still win."
        onMoved: (value) => ScaleSettings.spectrumRangeFit = value
    }

    Label {
        Layout.fillWidth: true
        Layout.maximumWidth: 640
        wrapMode: Text.WordWrap
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        text: "At tight, the spectrum's noise floor sits on the bottom edge and its strongest "
              + "peak on the top edge."
    }

    Rule {}

    SectionTitle { text: "band plan" }

    RCheckBox {
        text: "show the band bar over the span"
        checked: BandPlanSettings.showBar
        onToggled: {
            BandPlanSettings.showBar = checked
            checked = Qt.binding(() => BandPlanSettings.showBar)
        }
    }

    RowLayout {
        spacing: 16

        Label {
            text: "regions"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            Layout.minimumWidth: 70
        }

        // At least one stays checked: BandPlanSettings refuses to clear the
        // last, and the checkbox follows it back.
        RCheckBox {
            text: "United States"
            checked: BandPlanSettings.us
            onToggled: {
                BandPlanSettings.us = checked
                checked = Qt.binding(() => BandPlanSettings.us)
            }
        }

        RCheckBox {
            text: "Canada"
            checked: BandPlanSettings.canada
            onToggled: {
                BandPlanSettings.canada = checked
                checked = Qt.binding(() => BandPlanSettings.canada)
            }
        }
    }

    Rule {}

    SectionTitle { text: "speech" }

    // The same engine-wide switch as the top bar's speech button, here so the
    // window holds every global preference. Remembered and re-sent on each
    // connection; models/settings.h, kTranscription.
    RCheckBox {
        text: "speech to text on every receiver"
        enabled: engineLink.connected && engineLink.transcriptionOffered
        checked: engineLink.transcriptionOn
        onToggled: {
            if (checked !== engineLink.transcriptionOn)
                engineLink.toggleTranscription()
            checked = Qt.binding(() => engineLink.transcriptionOn)
        }
    }
}
