import QtQuick

// Native-feeling wheel scrolling for a Flickable or ListView.
//
// Qt Quick's own wheel handling turns each notch into a small fling tuned for
// touch, which reads as slow and rubbery with a mouse and as stepped with a
// precision touchpad. This replaces it: a mouse notch scrolls the system's
// lines-per-notch, eased the way Windows smooth scrolling is, and rapid notches
// accumulate into one glide; a touchpad's pixel deltas are applied 1:1 with no
// fling. Dragging with a finger is untouched.
//
// Declare it inside the Flickable: `SmoothWheel { flick: list }`.
WheelHandler {
    id: root
    required property Flickable flick
    // Emitted after any wheel movement, with whether the view ended up at the bottom.
    signal scrolled(bool atEnd)

    // One notch, in pixels: the system's lines-per-notch times a comfortable line.
    readonly property real notch: Math.max(1, Qt.styleHints.wheelScrollLines) * 26
    property real goal: 0

    target: null
    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

    function lowest() { return flick.originY - flick.topMargin }
    function highest() {
        return Math.max(lowest(), flick.originY + flick.contentHeight - flick.height + flick.bottomMargin)
    }
    function clamp(y) { return Math.min(highest(), Math.max(lowest(), y)) }

    onWheel: function (ev) {
        if (flick.contentHeight <= flick.height) {
            ev.accepted = false
            return
        }
        // Continue from where the glide is heading, so fast notches add up.
        const from = glide.running ? goal : flick.contentY
        if (ev.pixelDelta.y !== 0) {
            glide.stop()
            goal = clamp(flick.contentY - ev.pixelDelta.y)
            flick.contentY = goal
        } else {
            goal = clamp(from - ev.angleDelta.y / 120 * notch)
            glide.to = goal
            glide.restart()
        }
        ev.accepted = true
        root.scrolled(goal >= highest() - 2)
    }

    property NumberAnimation glide: NumberAnimation {
        target: root.flick
        property: "contentY"
        duration: 140
        easing.type: Easing.OutCubic
    }
}
