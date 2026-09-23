// The device picker.
//
// WHAT THIS REPLACES. Until openSource landed the source URI was a
// command-line argument to revenant-engine, so changing radio meant
// restarting the process and losing the operator's receivers, their
// waterfall history and their audio. Retuning the front end has worked
// over the wire since 2026-09-20; opening one did not.
//
// AN INLINE PANEL AND NOT A MODAL DIALOG. A modal would be the obvious
// shape and is the wrong one here: choosing a radio is something an
// operator does while watching the waterfall they are about to lose,
// and a dialog covering it takes away the one thing that says whether
// the change was worth making.
//
// EVERY RULE UNDER IT IS IN ui/models/source_choice.h WITH ITS OWN
// CASES IN ui/tests, which is the rule the whole window follows: which
// keys a backend takes, where a rate lands, which gain step a request
// rounds to and whether a control is offered at all. This file binds
// names and chooses colours.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Revenant

ColumnLayout {
    Layout.fillWidth: true
    spacing: 6

    // The same spacing the window's column uses, so the three rows below
    // sit exactly where they sat when they were three rows of that column.
    // Shown whenever any of them is: the fault row can outlive the
    // connection, and an empty wrapper that stayed visible would still
    // take a gap in the column.
    visible: engineLink.connected || engineLink.sourceFault.length > 0

    RowLayout {
        id: sourceRow

        // What the operator has picked in the list, as an index into
        // engineLink.sources. -1 is nothing picked.
        //
        // AN INDEX AND NOT A COPY OF THE ROW. composeSourceUri takes the
        // index and reads the descriptor C++ side, so the settings are
        // applied against the device's own description rather than against
        // whatever a QML copy still held after a refresh replaced the list.
        property int chosen: -1
        property bool open: false

        // A refresh replaces the list wholesale, so an index into the old
        // one means nothing against the new. Cleared rather than kept,
        // because the alternative is a selection that silently moves to
        // whichever device now sits at that position.
        Connections {
            target: engineLink
            function onSourcesChanged() {
                if (sourceRow.chosen >= engineLink.sources.length)
                    sourceRow.chosen = -1
            }
        }

        // The chosen row, or null when there is not one.
        //
        // EVERY GUARD BELOW TESTS THIS FOR TRUTH AND NOT FOR `!== null`,
        // and the difference is not style. `chosen` can point past the end
        // of `sources`: it is set from a list that a refresh replaces
        // wholesale, so a device unplugged between two refreshes leaves an
        // index with nothing at it. `sources[n]` is then `undefined`, and
        // `undefined !== null` is TRUE in JavaScript, so a guard written
        // that way passes and the next line dereferences it.
        //
        // Measured rather than reasoned about: with the panel forced open
        // and chosen at 0 before the first listing arrived, the window
        // logged eight "TypeError: Cannot read property ... of undefined"
        // on startup. They are invisible without QT_FORCE_STDERR_LOGGING,
        // because this is a GUI-subsystem binary and Qt's warnings do not
        // reach stderr in one by default. A binding that throws leaves its
        // property at the last value it held, so the panel would have shown
        // the previous device's controls for the new one.
        readonly property var picked:
            (chosen >= 0 && chosen < engineLink.sources.length)
            ? engineLink.sources[chosen] : null

        Layout.fillWidth: true
        spacing: 8
        visible: engineLink.connected

        Label {
            text: "source"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeTitle
            font.bold: true
        }

        // WHAT IS OPEN, WHICH IS A STATE THE WINDOW COULD NOT BE IN BEFORE
        // closeSource EXISTED. An engine with no source is connected, has a
        // device, and has no rate, no grid and no spectrum; without this
        // line it reads as an engine that is somehow broken.
        Label {
            Layout.minimumWidth: 0
            text: engineLink.sourceOpen
                  ? engineLink.sourceRate + " S/s"
                  : "no source open"
            color: engineLink.sourceOpen ? Theme.ink : Theme.inkWarn
            font.pixelSize: Theme.sizeBody
            font.bold: !engineLink.sourceOpen
            elide: Text.ElideRight
        }

        RButton {
            text: sourceRow.open ? "hide" : "change radio"
            font.pixelSize: Theme.sizeBody
            onClicked: {
                sourceRow.open = !sourceRow.open

                // Asked when the panel opens rather than on a timer. The
                // listing opens every device index to answer, including
                // ones with nothing behind them, so it pays a libusb
                // timeout per absent dongle: that is a cost to pay when
                // somebody is looking at the list, not once a second
                // forever.
                if (sourceRow.open)
                    engineLink.refreshSources()
            }
        }

        RButton {
            text: "close source"
            font.pixelSize: Theme.sizeBody
            enabled: engineLink.sourceOpen
            onClicked: {
                sourceRow.chosen = -1
                engineLink.closeSource()
            }
        }

        // THE FRONT END'S GAIN, drawn only where the device says it has a
        // stage to drive. Every file and every synthetic scene reports
        // none, and a slider over those would be a control that always
        // refuses.
        //
        // Labelled with the stage's OWN name rather than "gain", because
        // that is what it drives: on an R820T the one stage moves the LNA,
        // the mixer and the VGA together, and a device with three separate
        // stages would want three controls rather than one lying label.
        // engineLink.sourceGainStages says how many the device reported so
        // the row can admit when it is showing fewer than there are.
        RowLayout {
            spacing: 6
            visible: engineLink.sourceGainStage !== ""

            Label {
                text: engineLink.sourceGainStage + " gain"
                color: Theme.inkTune
                font.pixelSize: Theme.sizeBody
                font.bold: true
            }

            RSlider {
                id: gainSlider
                Layout.preferredWidth: 140
                from: 0.0
                to: 1.0

                // Zero on a continuous stage, which Slider reads as no
                // stepping. See gain_fraction_step for why the size comes
                // off the count of steps and not off the distance between
                // two of them.
                stepSize: engineLink.sourceGainStep
                snapMode: Slider.SnapAlways
                enabled: !engineLink.sourceGainAuto

                // THE HANDLE FOLLOWS THE DEVICE, NOT THE POINTER. The
                // binding is restored whenever the link answers, so a step
                // the tuner rounded to shows up as the handle settling onto
                // it rather than staying where it was let go.
                value: engineLink.sourceGainFraction

                onMoved: engineLink.setSourceGainFraction(value)
            }

            // What the device took, and nothing at all before it has said.
            // A number here that was only ever a request is the lie
            // sourceGainKnown exists to prevent.
            Label {
                text: engineLink.sourceGainAuto
                      ? "device choosing"
                      : (engineLink.sourceGainKnown
                         ? engineLink.sourceGainDb.toFixed(1) + " dB"
                         : "not set from here")
                color: engineLink.sourceGainKnown || engineLink.sourceGainAuto
                       ? Theme.ink
                       : Theme.inkDim
                font.pixelSize: Theme.sizeBody
            }

            // Offered only where the device will do it. Whether it should
            // is the operator's call: README.md has the measurement of what
            // this dongle's own AGC did to the detector's track list.
            Label {
                visible: engineLink.sourceGainHasAuto
                text: engineLink.sourceGainAuto ? "manual" : "auto"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -3
                    cursorShape: Qt.PointingHandCursor
                    onClicked: engineLink.setSourceGainAuto(!engineLink.sourceGainAuto)
                }
            }

            Label {
                visible: engineLink.sourceGainStages > 1
                text: "+" + (engineLink.sourceGainStages - 1) + " more"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
            }
        }

        Item { Layout.fillWidth: true }

        Label {
            Layout.minimumWidth: 0
            visible: engineLink.sourceGainFault !== ""
            text: engineLink.sourceGainFault
            color: Theme.inkWarn
            font.pixelSize: Theme.sizeBody
            elide: Text.ElideRight
            Layout.maximumWidth: 420
        }

        Label {
            Layout.minimumWidth: 0
            visible: engineLink.sourcesBusy
            text: "asking every device…"
            color: Theme.inkDim
            font.pixelSize: Theme.sizeBody
        }
    }

    // What the last open or close said when it refused. Its own row rather
    // than beside the button, because a registry refusal names the backends
    // it does know and that sentence is longer than a status strip.
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: engineLink.sourceFault.length > 0

        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: engineLink.sourceFault
            color: Theme.inkBad
            font.pixelSize: Theme.sizeBody
            wrapMode: Text.WordWrap
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 6
        visible: engineLink.connected && sourceRow.open

        Repeater {
            model: engineLink.sources

            RowLayout {
                id: sourceEntry

                required property var modelData
                required property int index

                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: sourceEntry.index === sourceRow.chosen ? "▸" : " "
                    color: Theme.inkTune
                    font.pixelSize: Theme.sizeBody
                }

                Label {
                    Layout.minimumWidth: 0
                    Layout.fillWidth: true
                    text: sourceEntry.modelData.displayName
                          + (sourceEntry.modelData.length.length > 0
                             ? "  ·  " + sourceEntry.modelData.length : "")
                    // AN UNAVAILABLE BACKEND IS SHOWN AND NOT HIDDEN. A
                    // missing DLL and an unplugged radio are different
                    // problems, and a list that omits both looks identical
                    // to a list with nothing attached.
                    color: sourceEntry.modelData.available ? Theme.ink : Theme.inkDim
                    font.pixelSize: Theme.sizeBody
                    elide: Text.ElideRight

                    MouseArea {
                        anchors.fill: parent
                        enabled: sourceEntry.modelData.available
                        cursorShape: Qt.PointingHandCursor
                        onClicked: sourceRow.chosen = sourceEntry.index
                    }
                }

                Label {
                    Layout.minimumWidth: 0
                    text: sourceEntry.modelData.available
                          ? sourceEntry.modelData.backend
                          : sourceEntry.modelData.unavailable
                    color: sourceEntry.modelData.available
                           ? Theme.inkDim : Theme.inkWarn
                    font.pixelSize: Theme.sizeBody
                    elide: Text.ElideRight
                }
            }
        }

        // The conditions that did not stop the source opening. Each one is
        // the answer to "why is this at the wrong frequency" an hour later:
        // a WAV whose auxi chunk carries no centre, a SigMF sidecar with an
        // empty captures array. A listing that showed only failures would
        // say nothing about any of them.
        Repeater {
            model: sourceRow.picked ? sourceRow.picked.notes : []

            Label {
                required property string modelData

                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: "note: " + modelData
                color: Theme.inkWarn
                font.pixelSize: Theme.sizeBody
                wrapMode: Text.WordWrap
            }
        }

        // The settings, and only the ones this device has. A control that
        // could never work is not offered, which is the case where a clean
        // refusal is not good enough because the operator has to discover
        // it by trying.
        RowLayout {
            id: sourceSettings

            // sourceRow.picked, not a second lookup. One place decides
            // what "a row is chosen" means, and its note says why that
            // cannot be a `!== null` test.
            readonly property var row: sourceRow.picked

            Layout.fillWidth: true
            spacing: 8
            visible: !!sourceSettings.row

            Label {
                visible: !!sourceSettings.row && sourceSettings.row.tunable
                text: "centre"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody
            }

            RTextField {
                id: sourceFreqField

                visible: !!sourceSettings.row && sourceSettings.row.tunable
                Layout.preferredWidth: 120
                font.pixelSize: Theme.sizeBody
                placeholderText: "98.1M"
                selectByMouse: true
            }

            Label {
                text: "rate"
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody
            }

            RTextField {
                id: sourceRateField

                Layout.preferredWidth: 110
                font.pixelSize: Theme.sizeBody

                // SAMPLES PER SECOND, and the placeholder says so with a
                // unit because the box next to it is a frequency and reads
                // a bare number the other way. engineLink.parseRateHz is
                // what parses this; see frequency_entry.h's BareNumber.
                placeholderText: "2400000 S/s"
                selectByMouse: true
            }

            Label {
                visible: !!sourceSettings.row && sourceSettings.row.hasGain
                text: sourceSettings.row && sourceSettings.row.hasGain
                      ? sourceSettings.row.gainName + " gain" : ""
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody
            }

            // GAIN IS A NUMBER AND auto IS A CHECKBOX BESIDE IT, rather
            // than "auto" being the bottom of the slider. They are
            // different requests: a number asks the device for a gain, and
            // auto hands the decision to the tuner's own AGC, which
            // maximises the level at its output and is therefore set by the
            // loudest thing anywhere in the span. Measured on air on
            // 2026-09-20 that put three intermodulation products in the
            // detector's track list at confidence 1.00; README.md carries
            // the measurement and the 5.7 dB it cost the wanted station.
            RTextField {
                id: sourceGainField

                visible: !!sourceSettings.row
                         && sourceSettings.row.hasGain
                         && !sourceGainAuto.checked
                Layout.preferredWidth: 70
                font.pixelSize: Theme.sizeBody
                placeholderText: "20"
                selectByMouse: true
            }

            RCheckBox {
                id: sourceGainAuto

                visible: !!sourceSettings.row
                         && sourceSettings.row.hasGain
                         && sourceSettings.row.gainHasAuto
                text: "auto"
                font.pixelSize: Theme.sizeBody
            }

            Item { Layout.fillWidth: true }

            // WHAT WILL ACTUALLY BE SENT, before anything is sent.
            // Composed by the same call the button makes, so the line
            // cannot disagree with the request: a preview built separately
            // in QML would be a second copy of every settling rule.
            Label {
                Layout.minimumWidth: 0
                Layout.maximumWidth: 420
                visible: !!sourceSettings.row
                text: engineLink.composeSourceUri(
                          sourceRow.chosen,
                          sourceFreqField.text,
                          sourceRateField.text,
                          sourceGainField.text,
                          sourceGainAuto.checked)
                color: Theme.inkDim
                font.pixelSize: Theme.sizeSmall
                elide: Text.ElideMiddle
            }

            RButton {
                text: "open"
                font.pixelSize: Theme.sizeBody

                // A TUNABLE DEVICE NEEDS A CENTRE AND THE BUTTON SAYS SO BY
                // BEING DEAD, rather than letting the operator press it and
                // read a refusal.
                //
                // An RTL-SDR opened with no freq= is refused by its own
                // backend, because leaving the tuner where opening it left it
                // means asking it to lock DC, which fails and leaves it
                // unable to tune at all. That refusal is correct and it is
                // still the wrong thing for an operator to discover by
                // pressing a button that looked ready.
                //
                // Only for a device that HAS a tuner. A file and a synthetic
                // scene have no centre to give and no box to give it in.
                enabled: !!sourceSettings.row && sourceSettings.row.available
                         && (!sourceSettings.row.tunable
                             || engineLink.parseHz(sourceFreqField.text) > 0)
                onClicked: engineLink.openSource(
                               engineLink.composeSourceUri(
                                   sourceRow.chosen,
                                   sourceFreqField.text,
                                   sourceRateField.text,
                                   sourceGainField.text,
                                   sourceGainAuto.checked))
            }
        }

        // Why the open button is dead, which is one cause and worth a
        // sentence: a tuner with nowhere to point cannot be opened.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: !!sourceSettings.row && sourceSettings.row.available
                     && sourceSettings.row.tunable
                     && engineLink.parseHz(sourceFreqField.text) <= 0

            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: "this device needs a centre frequency before it can be opened: "
                      + "opening a tuner with nowhere to point leaves it unable to tune "
                      + "at all afterwards."
                color: Theme.inkDim
                font.pixelSize: Theme.sizeBody
                wrapMode: Text.WordWrap
            }
        }

        // WHAT THE DEVICE ACTUALLY TOOK, which is the fifth of the five
        // conditions docs/rpc.md lists and the one that is deliberately not
        // a wire field.
        //
        // configure() in the RTL-SDR backend reads the achieved rate and
        // centre back from the device and substitutes them for what was
        // asked, which is correct; what nothing states is the DIFFERENCE.
        // It does not need a field, because the asker holds both halves:
        // this window composed the request and EngineInfo carries what the
        // device took. An engine re-deriving a request the client never
        // forgot would be the same number twice, and the one on the wire
        // would be the one that could go stale.
        //
        // Only when they differ, because on a device with a fine enough
        // step they agree and a line saying so is noise. The RTL-SDR's rate
        // comes from a 28.8 MHz clock over an integer and its PLL step is a
        // few hundred hertz, so this is where both show up.
        RowLayout {
            id: sourceTook

            Layout.fillWidth: true
            spacing: 8

            readonly property double askedRate:
                engineLink.parseRateHz(sourceRateField.text)
            readonly property double askedCentre:
                engineLink.parseHz(sourceFreqField.text)

            visible: engineLink.sourceOpen
                     && ((askedRate > 0 && askedRate !== engineLink.sourceRate)
                         || (askedCentre > 0
                             && askedCentre !== engineLink.sourceCenterHz))

            Label {
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                text: {
                    var parts = []
                    if (sourceTook.askedRate > 0
                        && sourceTook.askedRate !== engineLink.sourceRate)
                        parts.push("asked " + sourceTook.askedRate
                                   + " S/s, took " + engineLink.sourceRate)
                    if (sourceTook.askedCentre > 0
                        && sourceTook.askedCentre !== engineLink.sourceCenterHz)
                        parts.push("asked "
                                   + (sourceTook.askedCentre / 1.0e6).toFixed(6)
                                   + " MHz, took "
                                   + (engineLink.sourceCenterHz / 1.0e6).toFixed(6)
                                   + " MHz")
                    return "the device " + parts.join("  ·  ")
                }
                color: Theme.inkWarn
                font.pixelSize: Theme.sizeBody
                elide: Text.ElideRight
            }
        }
    }
}
