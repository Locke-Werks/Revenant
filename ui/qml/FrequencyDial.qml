// A frequency, as a row of digits the wheel can turn one at a time.
//
// Point at a digit and it lights; turn the wheel and that digit steps, with
// carry and borrow, stopped at the limits. Click the dial to type a frequency
// instead. The digits are drawn in the monospace family in groups of three,
// megahertz before the point, so the eye counts groups rather than digits.
//
// WHAT THE DIAL SHOWS WHILE A STEP IS IN FLIGHT. The value is the engine's,
// and it arrives a round trip after the wheel moves, or 330 ms and more on a
// front end that has to stop streaming to retune. Showing the engine's value
// throughout would make every notch look ignored and the next notch step from
// a number the operator has already moved past. So a step is shown at once,
// in the dial's tint, and held until the engine reports that value or two
// seconds pass; then the dial goes back to saying what the engine says, which
// is the only number that is true.
//
// The arithmetic is models/frequency_dial.h, reached through UiRules, and it
// has its cases in ui/tests.

import QtQuick
import QtQuick.Controls
import Revenant

Item {
    id: dial

    // The engine's value, in hertz.
    property double value: 0

    // What the value may be stepped to. A pair that is not a range clamps
    // nothing, which is the state before a source has said what it tunes.
    property double low: 0
    property double high: 0

    property color tint: Theme.accent
    property int pixelSize: Theme.sizeDial

    // What typing is for, said in the field before anything is typed.
    property string placeholder: "98.1 or 98.1M"

    // A step, with the frequency it asks for.
    signal stepped(double hz)

    // Typed text, for the caller to parse, since the caller is the one that
    // knows how a frequency it cannot reach should be refused.
    signal typed(string text)

    property double pendingHz: 0
    property bool pending: false

    readonly property double shown: pending ? pendingHz : value
    readonly property int digits: UiRules.dialDigitCount(shown, low, high)
    readonly property int significant: UiRules.dialSignificantDigits(shown)
    readonly property bool editing: editor.visible
    readonly property string editText: editor.text

    // The digit under the pointer, counted from the hertz digit, or -1.
    property int activeDigit: -1

    // The digit the tuning keys step, or -1 on a dial they do not reach. It
    // carries a faint mark while the pointer is elsewhere, so the keys say
    // which digit they will move before one is pressed.
    property int keyDigit: -1

    // Wheel travel not yet spent on a notch, for touchpads that send a
    // fraction of one at a time.
    property real wheelCarry: 0

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    onValueChanged: {
        if (dial.pending && dial.value === dial.pendingHz)
            dial.pending = false
    }

    function step(digit, notches) {
        if (!dial.enabled || notches === 0)
            return
        const next = UiRules.dialStep(dial.shown, digit, notches, dial.low, dial.high)
        if (next === dial.shown)
            return
        dial.pendingHz = next
        dial.pending = true
        settle.restart()
        dial.stepped(next)
    }

    function edit() {
        if (!dial.enabled)
            return
        editor.text = ""
        editor.visible = true
        editor.forceActiveFocus()
    }

    Timer {
        id: settle
        interval: 2000
        onTriggered: dial.pending = false
    }

    Row {
        id: row

        spacing: 0
        opacity: dial.editing ? 0.0 : 1.0

        Text {
            visible: dial.shown < 0
            text: "−"
            color: Theme.ink
            font.family: Theme.monoFont
            font.pixelSize: dial.pixelSize
        }

        Repeater {
            model: dial.digits

            Row {
                id: cell

                required property int index

                // Counted from the hertz digit, which is what the rules take.
                readonly property int place: dial.digits - 1 - index
                readonly property bool active: dial.activeDigit === place && dial.enabled
                readonly property bool keyed: dial.keyDigit === place && dial.activeDigit < 0
                                              && dial.enabled && !dial.editing
                readonly property bool leading: place >= dial.significant

                Text {
                    id: digitText

                    text: UiRules.dialDigit(dial.shown, cell.place)
                    font.family: Theme.monoFont
                    font.pixelSize: dial.pixelSize
                    color: !dial.enabled ? Theme.inkOff
                           : cell.active ? dial.tint
                           : cell.leading ? Theme.inkOff
                           : dial.pending ? dial.tint
                           : Theme.ink

                    // The active digit's mark, under it rather than a box
                    // around it, so the row still reads as one number.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: cell.active ? 2 : 1
                        color: cell.active ? dial.tint : Theme.inkDim
                        visible: cell.active || cell.keyed
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: dial.enabled ? Qt.SizeVerCursor : Qt.ArrowCursor
                        onEntered: dial.activeDigit = cell.place
                        onExited: {
                            if (dial.activeDigit === cell.place)
                                dial.activeDigit = -1
                        }
                        onClicked: dial.edit()
                        onWheel: (wheel) => {
                            dial.wheelCarry += wheel.angleDelta.y
                            const notches = dial.wheelCarry > 0
                                            ? Math.floor(dial.wheelCarry / 120)
                                            : Math.ceil(dial.wheelCarry / 120)
                            dial.wheelCarry -= notches * 120
                            dial.step(cell.place, notches)
                        }
                    }
                }

                Text {
                    text: UiRules.dialSeparatorAfter(cell.place)
                    visible: text.length > 0 && cell.place > 0
                    color: cell.leading ? Theme.inkOff : Theme.inkDim
                    font.family: Theme.monoFont
                    font.pixelSize: dial.pixelSize
                    // The gap is narrower than a digit, which is what makes it
                    // read as a separator rather than a missing digit.
                    width: text === " " ? dial.pixelSize * 0.3 : implicitWidth
                }
            }
        }

        Text {
            text: " MHz"
            color: Theme.inkDim
            font.family: Theme.uiFont
            font.pixelSize: Math.round(dial.pixelSize * 0.5)
            // On the digits' baseline rather than centred on them, the way a
            // unit is set after a number in print.
            y: Math.round(dial.pixelSize * 0.58)
        }
    }

    RTextField {
        id: editor

        visible: false
        mono: true
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(row.implicitWidth, 160)
        placeholderText: dial.placeholder
        onAccepted: {
            dial.typed(editor.text)
            editor.visible = false
        }
        onActiveFocusChanged: {
            if (!activeFocus)
                editor.visible = false
        }
        Keys.onEscapePressed: editor.visible = false
    }
}
