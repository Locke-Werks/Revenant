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
        pivotRow.slider.value = ScaleSettings.waterfallPivot
        fitRow.slider.value = ScaleSettings.spectrumRangeFit
        smoothSlider.value = ScaleSettings.spectrumSmoothing
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
        property real minimum: 0
        property string lowLabel: "padded"
        property string highLabel: "tight"
        property bool percent: true
        signal moved(real value)

        spacing: 8

        Label {
            text: fit.label
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            Layout.minimumWidth: 150
        }

        Label {
            text: fit.lowLabel
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        RSlider {
            id: fitSlider
            Layout.preferredWidth: 180
            from: fit.minimum
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
            text: fit.highLabel
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
             + "waterfall's history across the whole colour map. Past that the map bends into an "
             + "S about the pivot: below it goes darker, above it brighter, so the noise sinks "
             + "and weak signals lift. Pins still win."
        onMoved: (value) => ScaleSettings.waterfallContrast = value
    }

    FitRow {
        id: pivotRow
        label: "contrast pivot"
        minimum: 0.05
        maximum: 0.95
        lowLabel: "low"
        highLabel: "high"
        tip: "Where the S turns, as a height on the colour map. Levels below it go darker and "
             + "above it brighter. Set it just above the noise. Only acts past 100% contrast."
        onMoved: (value) => ScaleSettings.waterfallPivot = value
    }

    // The contrast curve, input across and output up, each point and the
    // bars under it in the colour the waterfall gives that output. The
    // dim diagonal is the straight map; the tick is the pivot.
    RowLayout {
        spacing: 8

        Item { Layout.minimumWidth: 150 }

        Canvas {
            id: curveCanvas
            Layout.preferredWidth: 240
            Layout.preferredHeight: 120

            Connections {
                target: ScaleSettings
                function onWaterfallContrastChanged() { curveCanvas.requestPaint() }
                function onWaterfallPivotChanged() { curveCanvas.requestPaint() }
            }

            onPaint: {
                const ctx = getContext("2d")
                const w = width, h = height, pad = 4
                const iw = w - 2 * pad, ih = h - 2 * pad
                ctx.reset()
                ctx.fillStyle = Theme.panelSolid
                ctx.fillRect(0, 0, w, h)
                ctx.strokeStyle = Theme.border
                ctx.lineWidth = 1
                ctx.strokeRect(0.5, 0.5, w - 1, h - 1)

                // The output colour along the bottom strip, so the input
                // band below the pivot reads as how dark it ends up.
                const steps = 120
                const strip = 6
                for (let i = 0; i < steps; ++i) {
                    const x = (i + 0.5) / steps
                    ctx.fillStyle = ScaleSettings.paletteAt(ScaleSettings.curveAt(x))
                    ctx.fillRect(pad + i * iw / steps, h - pad - strip, iw / steps + 1, strip)
                }

                ctx.strokeStyle = Theme.inkOff
                ctx.beginPath()
                ctx.moveTo(pad, h - pad)
                ctx.lineTo(w - pad, pad)
                ctx.stroke()

                const p = ScaleSettings.waterfallPivot
                ctx.strokeStyle = Theme.inkDim
                ctx.setLineDash([2, 3])
                ctx.beginPath()
                ctx.moveTo(pad + p * iw, pad)
                ctx.lineTo(pad + p * iw, h - pad - strip)
                ctx.stroke()
                ctx.setLineDash([])

                ctx.lineWidth = 2.5
                let lx = pad, ly = h - pad - ScaleSettings.curveAt(0) * ih
                for (let i = 1; i <= steps; ++i) {
                    const x = i / steps
                    const y = ScaleSettings.curveAt(x)
                    const px = pad + x * iw, py = h - pad - y * ih
                    ctx.strokeStyle = ScaleSettings.paletteAt(Math.max(y, 0.08))
                    ctx.beginPath()
                    ctx.moveTo(lx, ly)
                    ctx.lineTo(px, py)
                    ctx.stroke()
                    lx = px
                    ly = py
                }
            }

            Component.onCompleted: requestPaint()
        }
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

    // Release time for the spectrum trace. A signal still appears in the
    // frame it arrives; this only sets how long noise is averaged over and
    // how slowly a signal that left fades. See render/spectrum_scale.h.
    RowLayout {
        spacing: 8

        Label {
            text: "spectrum smoothing"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            Layout.minimumWidth: 150
        }

        Label {
            text: "off"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        RSlider {
            id: smoothSlider
            Layout.preferredWidth: 180
            from: 0
            to: 1
            stepSize: 0.01
            onMoved: ScaleSettings.spectrumSmoothing = value

            Tip {
                visible: parent.hovered
                delay: 400
                text: "Averages the noise so the floor reads flat. Signals still appear at "
                      + "once; this is how slowly they fade after they go."
            }
        }

        Label {
            text: "slow"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
        }

        Readout {
            widest: "1000 ms"
            horizontalAlignment: Text.AlignLeft
            text: smoothSlider.value > 0 ? Math.round(smoothSlider.value * 1000) + " ms" : "off"
            color: Theme.ink
        }
    }

    RCheckBox {
        text: "also smooth the noise floor across frequency"
        checked: ScaleSettings.spectrumFloorSmoothing
        onToggled: {
            ScaleSettings.spectrumFloorSmoothing = checked
            checked = Qt.binding(() => ScaleSettings.spectrumFloorSmoothing)
        }
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

    SectionTitle { text: "radio" }

    // Engine-wide: cuts impulse noise out of the whole span before the
    // channelizer. Remembered and re-sent on each connection;
    // models/settings.h, kImpulseExcision.
    RCheckBox {
        text: "remove impulse noise across the span"
        checked: engineLink.impulseExcision
        onToggled: {
            engineLink.impulseExcision = checked
            checked = Qt.binding(() => engineLink.impulseExcision)
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
