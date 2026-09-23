// What each key does. The keys themselves, their labels and when they are
// enabled are ui/models/key_actions.h, reached through KeyMap; this file is
// the one table from a handler's name to the call it makes, and it binds one
// application-wide Shortcut per action from the same table.
//
// NOTHING HERE CHOOSES A KEY. A key is changed in the header and nowhere
// else, and the header's tests keep two actions off one key. What lives here
// is only the reach into the running window that a Qt-free table cannot have:
// the radio's dial, the receiver window's filter display, the panels.
//
// A HANDLER THE TABLE NAMES AND THIS FILE LACKS IS A FAILED SMOKE RUN.
// main.cpp calls missingText() as a --smoke-seconds run starts and exits 1 on
// anything it returns, so a row added to the table without a line here is a
// red CI run rather than a key that silently does nothing.

import QtQuick
import QtQml.Models
import Revenant

Item {
    id: commands

    objectName: "commands"
    visible: false

    // The windows and the parts of them a key reaches into.
    required property var mainWindow
    required property var receiverWindow
    required property var topBar
    required property var spanView
    required property var mainPalette
    required property var mainKeyMap

    // The digit the tuning keys step, counted from the hertz digit. The dial
    // draws a mark under it so the keys say which digit they will move.
    property int tuneDigit: KeyMap.defaultTuningDigit

    // What the window has, as the bits the table's needs are written in.
    readonly property int have: KeyMap.have({
        "connected": engineLink.connected,
        "sourceOpen": engineLink.sourceOpen,
        "canRetune": engineLink.sourceCanRetune,
        "receiver": engineLink.receiverId > 0,
        "secondReceiver": engineLink.rackCount > 1,
        "aftOffered": engineLink.aftOffered,
        "autoFilterOffered": engineLink.autoFilterOffered,
        "spectrumDrawing": commands.spanView.drawing,
        "noiseOffered": engineLink.noiseBlankerOffered,
        "notchOffered": engineLink.notchOffered,
        "autoNotchOffered": engineLink.autoNotchOffered
    })

    readonly property var handlers: ({
        "tune.step": (notches) => {
            commands.topBar.tuneDial.step(commands.tuneDigit, Number(notches))
        },
        "tune.digit": (places) => {
            commands.tuneDigit = KeyMap.moveTuningDigit(commands.tuneDigit, Number(places),
                                                        commands.topBar.tuneDial.digits)
        },
        "tune.page": (direction) => {
            engineLink.tuneSourceHz(KeyMap.pageTuneHz(
                engineLink.sourceCenterHz, engineLink.spanLowHz, engineLink.spanHighHz,
                Number(direction), engineLink.sourceTuneLowHz, engineLink.sourceTuneHighHz))
        },
        "tune.type": () => {
            commands.bringForward(commands.mainWindow)
            commands.topBar.tuneDial.edit()
        },
        "tune.band": (index) => {
            engineLink.tuneSourceHz(UiRules.bands()[Number(index)].centre)
        },
        "palette.bands": () => commands.openPalette("bands"),
        "receiver.add": () => {
            engineLink.addReceiver((engineLink.spanLowHz + engineLink.spanHighHz) / 2, "")
        },
        "receiver.centre": () => {
            engineLink.tuneReceiver((engineLink.spanLowHz + engineLink.spanHighHz) / 2, "")
        },
        "receiver.next": (step) => engineLink.focusNextReceiver(Number(step)),
        "receiver.solo": () => engineLink.toggleReceiverSolo(engineLink.focusedKey),
        "receiver.type": () => {
            commands.bringForward(commands.receiverWindow)
            commands.receiverWindow.receiverDial.edit()
        },
        "receiver.remove": () => engineLink.removeReceiver(),
        "receiver.aft": () => {
            engineLink.aftEnabled = !engineLink.aftEnabled
        },
        "receiver.auto_filter": () => {
            engineLink.autoFilterEnabled = !engineLink.autoFilterEnabled
        },
        "receiver.mode": (mode) => engineLink.setReceiverDemod(mode),
        "receiver.noise": (stage) => engineLink.toggleNoiseStage(stage),
        "filter.widen": (sign) => {
            commands.receiverWindow.passband.widenPassband(Number(sign) * KeyMap.filterStepHz)
        },
        "filter.default": () => commands.receiverWindow.passband.resetPassband(),
        "filter.keys": () => {
            commands.bringForward(commands.receiverWindow)
            commands.receiverWindow.passband.forceActiveFocus()
        },
        "audio.mute": () => {
            audioPlayer.muted = !audioPlayer.muted
        },
        "audio.volume": (presses) => {
            audioPlayer.volume = KeyMap.stepVolume(audioPlayer.volume, Number(presses))
        },
        "scale.pin": (end) => {
            if (end === "floor") {
                if (ScaleSettings.floorPinned)
                    ScaleSettings.unpinFloor()
                else
                    ScaleSettings.pinFloor(commands.spanView.drawFloorDb)
            } else {
                if (ScaleSettings.ceilingPinned)
                    ScaleSettings.unpinCeiling()
                else
                    ScaleSettings.pinCeiling(commands.spanView.drawCeilingDb)
            }
        },
        "panel.open": (name) => {
            commands.bringForward(commands.mainWindow)
            commands.topBar.togglePanel(name)
        },
        "memory.save": () => frequencyManager.addFromReceiver(""),
        "window.receivers": () => commands.receiverWindow.toggle(),
        "palette.open": () => commands.openPalette(""),
        "keymap.open": () => commands.openKeyMap()
    })

    function run(handler, argument) {
        const call = commands.handlers[handler]
        if (call === undefined) {
            console.warn("no handler for " + handler)
            return
        }
        call(argument)
    }

    // The handlers the table names and this file does not hold, comma
    // separated, empty when there are none. See the note at the top.
    function missingText() {
        return KeyMap.handlers().filter((name) => commands.handlers[name] === undefined)
                                .join(", ")
    }

    function bringForward(target) {
        target.show()
        target.raise()
        target.requestActivate()
    }

    // The palette and the key map open over whichever window is in front, so
    // the operator's eyes do not have to go to the other screen for them.
    function receiversInFront() {
        return commands.receiverWindow.visible && commands.receiverWindow.active
    }

    function openPalette(scope) {
        const palette = commands.receiversInFront() ? commands.receiverWindow.commandPalette
                                                    : commands.mainPalette
        if (palette.opened && palette.scope === scope) {
            palette.close()
            return
        }
        commands.closeOverlays()
        palette.openWith(scope, "")
    }

    function openKeyMap() {
        const keyMap = commands.receiversInFront() ? commands.receiverWindow.keyMapView
                                                   : commands.mainKeyMap
        if (keyMap.opened) {
            keyMap.close()
            return
        }
        commands.closeOverlays()
        keyMap.open()
    }

    function closeOverlays() {
        commands.mainPalette.close()
        commands.mainKeyMap.close()
        commands.receiverWindow.commandPalette.close()
        commands.receiverWindow.keyMapView.close()
    }

    // For a smoke run's photographs: the main window's palette with a query
    // typed into it, and its key map.
    function showPalette(query) {
        commands.closeOverlays()
        commands.mainPalette.openWith("", query)
    }

    function showKeyMap() {
        commands.closeOverlays()
        commands.mainKeyMap.open()
    }

    // And one of the top bar's panels, for --panel. Not bringForward: a smoke
    // run's windows are offscreen and there is nothing to raise.
    function showPanel(name) {
        commands.closeOverlays()
        commands.topBar.togglePanel(name)
    }

    // One application-wide shortcut per keyed action in the table. Application
    // rather than window context, so a key works from either window; a text
    // field keeps the keys it types with, and the filter display claims its
    // own before these see them.
    Instantiator {
        model: KeyMap.shortcuts()

        delegate: Shortcut {
            required property var modelData

            sequences: modelData.keys
            context: Qt.ApplicationShortcut
            enabled: KeyMap.enabled(modelData.id, commands.have)
            onActivated: commands.run(modelData.handler, modelData.argument)
        }
    }
}
