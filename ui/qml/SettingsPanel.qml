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
        captionZoomRow.slider.value = ScaleSettings.captionZoom
    }

    Component.onCompleted: sync()

    // The pivot also moves from the strip on the waterfall's edge.
    Connections {
        target: ScaleSettings
        function onWaterfallPivotChanged() {
            if (!pivotRow.slider.pressed)
                pivotRow.slider.value = ScaleSettings.waterfallPivot
        }
    }

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
        slider.enabled: !ScaleSettings.waterfallAutoPivot
    }

    // While on, the slider shows where the automatic pivot is and the hand-set
    // one waits for it to go off; models/scale_settings.h.
    RCheckBox {
        text: "automatic pivot"
        checked: ScaleSettings.waterfallAutoPivot
        onToggled: {
            ScaleSettings.waterfallAutoPivot = checked
            checked = Qt.binding(() => ScaleSettings.waterfallAutoPivot)
        }

        Tip {
            visible: parent.hovered
            delay: 400
            text: "Sets the pivot from the waterfall itself: the split between the noise and "
                  + "the signals in its history, eased over a couple of seconds."
        }
    }

    // A tick and the clock time down the edge of both waterfalls.
    RowLayout {
        spacing: 8

        Label {
            text: "time markers"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        RSegmented {
            readonly property var seconds: [0, 5, 10, 30, 60]
            options: ["off", "5 s", "10 s", "30 s", "60 s"]
            current: options[Math.max(0, seconds.indexOf(ScaleSettings.timeMarkerSeconds))]
            onPicked: (choice) => ScaleSettings.timeMarkerSeconds =
                                      seconds[options.indexOf(choice)]
        }
    }

    // How long the span's persistence view remembers a hit.
    RowLayout {
        spacing: 8

        Label {
            text: "persistence decay"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }

        RSegmented {
            readonly property var seconds: [0.5, 2, 5, 15]
            options: ["0.5 s", "2 s", "5 s", "15 s"]
            current: options[Math.max(0, seconds.indexOf(
                                          SpanDisplaySettings.persistenceDecaySeconds))]
            onPicked: (choice) => SpanDisplaySettings.persistenceDecaySeconds =
                                      seconds[options.indexOf(choice)]
        }
    }

    RCheckBox {
        text: "remember notch and filter per band"
        checked: frequencyManager.rememberPerBand
        onToggled: {
            frequencyManager.rememberPerBand = checked
            checked = Qt.binding(() => frequencyManager.rememberPerBand)
        }

        Tip {
            visible: parent.hovered
            delay: 400
            text: "Keeps the notches and the filter you leave on each mode and band, and puts "
                  + "them back when you tune into that band again. A memory that holds "
                  + "notches still wins on recall."
        }
    }

    // The contrast curve, input across and output up, each point and the
    // bars under it in the colour the waterfall gives that output. The
    // dim diagonal is the straight map; the tick is the pivot.
    RowLayout {
        spacing: 8

        Item { Layout.minimumWidth: 150 }

        ContrastCurve {
            Layout.preferredWidth: 240
            Layout.preferredHeight: 120
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

    FitRow {
        id: captionZoomRow
        label: "caption hover zoom"
        minimum: ScaleSettings.minCaptionZoom
        maximum: ScaleSettings.maxCaptionZoom
        lowLabel: "none"
        highLabel: "large"
        tip: "How much a transcript caption on the waterfall grows when the pointer stops on it. "
             + "At none it keeps its resting size and only unfolds to its whole text."
        onMoved: (value) => ScaleSettings.captionZoom = value
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

    // Band-plan overlay labels (feature 18); bits are models/live_labels.h's
    // LabelCategory.
    RCheckBox {
        text: "label channels and identified signals on the span"
        checked: SpanLabelSettings.showLabels
        onToggled: {
            SpanLabelSettings.showLabels = checked
            checked = Qt.binding(() => SpanLabelSettings.showLabels)
        }
    }
    Flow {
        Layout.fillWidth: true
        spacing: 8
        Repeater {
            model: [
                { bit: 1, name: "heard now" }, { bit: 2, name: "amateur" },
                { bit: 4, name: "weather" }, { bit: 8, name: "aviation" },
                { bit: 16, name: "marine" }, { bit: 32, name: "public safety" },
                { bit: 64, name: "personal" }, { bit: 128, name: "broadcast" },
                { bit: 256, name: "other" }
            ]
            delegate: RCheckBox {
                required property var modelData
                text: modelData.name
                enabled: SpanLabelSettings.showLabels
                checked: (SpanLabelSettings.categories & modelData.bit) !== 0
                onToggled: {
                    SpanLabelSettings.setCategory(modelData.bit, checked)
                    checked = Qt.binding(() => (SpanLabelSettings.categories & modelData.bit) !== 0)
                }
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

    // Engine-wide: finds a comb of evenly spaced interference lines across
    // the span and subtracts it before the channelizer. Cancels nothing until
    // it has locked; the control row shows what it is doing. Remembered and
    // re-sent on each connection; models/settings.h, kCombCancel.
    RCheckBox {
        text: "cancel comb interference across the span"
        checked: engineLink.combCancel
        onToggled: {
            engineLink.combCancel = checked
            checked = Qt.binding(() => engineLink.combCancel)
        }
    }

    // Engine-wide, beside the comb: a carrier the detector finds unkeyed,
    // steady and bare, or one that keeps its offset when the radio retunes,
    // is cancelled one line at a time by the same stage. Never where a
    // receiver is tuned. Remembered; models/settings.h, kSpurCancel.
    RCheckBox {
        text: "cancel spurs (unkeyed carriers)"
        checked: engineLink.spurCancel
        onToggled: {
            engineLink.spurCancel = checked
            checked = Qt.binding(() => engineLink.spurCancel)
        }
    }

    // Time-shift: how much of the source the engine keeps in memory for the
    // span's scrub bar. The smaller of the two bounds wins; changing either
    // empties what is held. Remembered; models/settings.h, kTimeShiftMinutes.
    RowLayout {
        spacing: 8
        Label {
            text: "time-shift"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            Layout.minimumWidth: 70
        }
        RSlider {
            id: shiftMinutes
            Layout.preferredWidth: 160
            from: 0
            to: 10
            stepSize: 0.5
            snapMode: Slider.SnapAlways
            value: engineLink.timeShiftMinutes
            onMoved: engineLink.timeShiftMinutes = value
        }
        Label {
            text: shiftMinutes.value === 0 ? "off" : shiftMinutes.value.toFixed(1) + " min"
            color: Theme.ink
            font.pixelSize: Theme.sizeBody
            Layout.minimumWidth: 56
        }
        RSlider {
            id: shiftBudget
            Layout.preferredWidth: 160
            from: 0.5
            to: 16
            stepSize: 0.5
            snapMode: Slider.SnapAlways
            value: engineLink.timeShiftBudgetGiB
            onMoved: engineLink.timeShiftBudgetGiB = value
        }
        Label {
            text: "at most " + shiftBudget.value.toFixed(1) + " GiB of RAM"
            color: Theme.ink
            font.pixelSize: Theme.sizeBody
        }
    }
    Label {
        visible: engineLink.timeShiftCapacitySeconds > 0
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        text: "room for " + Math.round(engineLink.timeShiftCapacitySeconds)
              + " s at this source's rate, " + Math.round(engineLink.timeShiftHeldSeconds)
              + " s held now"
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

    Rule {}

    SectionTitle { text: "capture" }

    // The engine's rolling capture, as its own flags. Edited and then applied,
    // because applying restarts the engine; models/capture_settings.h. An
    // engine somebody else started keeps the flags they gave it.
    component CaptureField: RowLayout {
        id: field

        required property string label
        required property string problem
        property string value: ""
        property string placeholder: ""
        property string hint: ""
        property bool mono: true
        property int fieldWidth: 120
        signal edited(string text)

        spacing: 8
        Layout.fillWidth: true

        Label {
            text: field.label
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
            Layout.minimumWidth: 70
        }

        RTextField {
            Layout.preferredWidth: field.fieldWidth
            mono: field.mono
            enabled: captureSettings.engineOurs
            text: field.value
            placeholderText: field.placeholder
            onTextEdited: field.edited(text)
        }

        Label {
            text: field.problem !== "" ? field.problem : field.hint
            color: field.problem !== "" ? Theme.accent : Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.maximumWidth: 460
        }
    }

    Label {
        visible: !captureSettings.engineOurs
        Layout.fillWidth: true
        Layout.maximumWidth: 640
        wrapMode: Text.WordWrap
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        text: "This window did not start the engine it is connected to, so whether that engine "
              + "captures is set by whoever started it, with the flags it was given. These "
              + "settings apply to an engine this window starts."
    }

    RCheckBox {
        text: "keep a rolling capture of the raw stream"
        enabled: captureSettings.engineOurs
        checked: captureSettings.enabled
        onToggled: {
            captureSettings.enabled = checked
            checked = Qt.binding(() => captureSettings.enabled)
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.maximumWidth: 640
        wrapMode: Text.WordWrap
        color: Theme.inkDim
        font.pixelSize: Theme.sizeSmall
        text: captureSettings.costText
    }

    CaptureField {
        label: "folder"
        problem: captureSettings.folderProblem
        fieldWidth: 360
        mono: false
        value: captureSettings.folder
        placeholder: captureSettings.defaultFolder
        onEdited: (text) => captureSettings.folder = text
    }

    CaptureField {
        label: "keep up to"
        problem: captureSettings.retainProblem
        hint: "such as 50G or 500M; empty keeps everything"
        value: captureSettings.retain
        placeholder: "no limit"
        onEdited: (text) => captureSettings.retain = text
    }

    CaptureField {
        label: "keep for"
        problem: captureSettings.maxAgeProblem
        hint: "such as 24h or 7d; empty keeps any age"
        value: captureSettings.maxAge
        placeholder: "no limit"
        onEdited: (text) => captureSettings.maxAge = text
    }

    RowLayout {
        spacing: 8
        visible: captureSettings.engineOurs

        RButton {
            text: "apply and restart the engine"
            enabled: captureSettings.canApply
            onClicked: captureSettings.apply()
        }

        RButton {
            flat: true
            text: "revert"
            ink: Theme.inkDim
            enabled: captureSettings.changed
            onClicked: captureSettings.revert()
        }

        Label {
            Layout.fillWidth: true
            Layout.maximumWidth: 420
            wrapMode: Text.WordWrap
            color: Theme.inkDim
            font.pixelSize: Theme.sizeSmall
            text: "Restarting closes the source and the receivers; the radio used last "
                  + "reopens. A recording that was playing does not."
        }
    }
}
