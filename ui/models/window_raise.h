// When a click on the span brings the receiver window forward, and how.
//
// The owner's request of 2026-09-23: the receiver window is a separate
// top-level window, and a click on a signal, one that tunes or focuses a
// receiver on the span, has to bring it to the front, showing it if it was
// hidden or minimised. It is the app raising its own window on the
// operator's own click.
//
// WHICH CLICKS. The ones that change what the receiver window is about:
// focusing another receiver, retuning the focused one, opening the first,
// and the second click of a double click that adds one. A second click that
// adds nothing does not, and neither does one refused on a full rack, whose
// first click has already raised the window. models/receiver_rack.h's
// classify_span_click is what names them, and this reads its answer.
//
// HOW, AND WHAT WINDOWS DOES WITH IT. Three cases, by the window's state.
//
// Already on screen: raise() and nothing else. QWindow::raise() on Windows is
// a SetWindowPos to the top of the z-order, and the operator's click has
// just made this process the foreground one, which is what Windows'
// foreground lock asks before it lets a window come forward. Keyboard focus
// is left where the operator clicked. Whether Windows activates the window as
// a side effect of the z-order change is the platform's; nothing here asks
// for it, and nothing breaks if it happens, because every key is an
// application-wide shortcut (ui/qml/Commands.qml) that reaches either window,
// and the wheel goes to the window under the pointer whether it is active or
// not, which is Windows 10 and 11's "scroll inactive windows" default. What
// does follow activation is where the command palette opens, which is
// whichever window is active.
//
// Hidden, or minimised: shown, or restored, then raised, then
// requestActivate(). Windows activates a window it shows or restores anyway,
// and asking makes that the rule rather than an accident: without it a
// window restored while the foreground lock refused activation comes up
// behind with a flashing taskbar button, which is the complaint this exists
// to answer. The receiver window has the keyboard after that; see above for
// why the next key and the next wheel still land.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <cstdint>

#include "models/receiver_rack.h"

namespace revenant::ui {

// The window's state, from QWindow::visibility.
enum class ReceiverWindowState : std::uint8_t {
    Hidden,
    Minimised,
    OnScreen,
};

// What to do to the receiver window, in this order: show or restore, raise,
// activate.
struct ReceiverWindowRaise {
    bool show = false;
    bool restore = false;
    bool raise = false;
    bool activate = false;

    [[nodiscard]] constexpr bool any() const { return show || restore || raise || activate; }
};

// Whether a click of this kind is one the receiver window comes forward for.
[[nodiscard]] constexpr bool click_brings_receivers_forward(SpanClick kind)
{
    // No default: a new kind of click is a warning the CI build stops on.
    switch (kind) {
        case SpanClick::Focus:
        case SpanClick::Retune:
        case SpanClick::Open:
        case SpanClick::Add:
            return true;
        case SpanClick::Full:
        case SpanClick::Nothing:
            return false;
    }
    return false;
}

// What bringing it forward takes, from the state it is in.
[[nodiscard]] constexpr ReceiverWindowRaise plan_receiver_window_raise(ReceiverWindowState state)
{
    ReceiverWindowRaise out;
    out.raise = true;
    switch (state) {
        case ReceiverWindowState::Hidden:
            out.show = true;
            out.activate = true;
            break;
        case ReceiverWindowState::Minimised:
            out.restore = true;
            out.activate = true;
            break;
        case ReceiverWindowState::OnScreen:
            break;
    }
    return out;
}

}  // namespace revenant::ui
