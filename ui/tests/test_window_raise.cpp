// click_brings_receivers_forward and plan_receiver_window_raise: which clicks
// on the span bring the receiver window to the front, and what that takes.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. What cannot be asserted here is what Windows does with
// the calls; models/window_raise.h says what is expected of it and why the
// next key and wheel still land either way.

#include <catch2/catch_test_macros.hpp>

#include "models/receiver_rack.h"
#include "models/window_raise.h"

using revenant::ui::classify_span_click;
using revenant::ui::click_brings_receivers_forward;
using revenant::ui::plan_receiver_window_raise;
using revenant::ui::ReceiverWindowState;
using revenant::ui::SpanClick;
using revenant::ui::SpanClickInput;

// Rejects raising only on a click that tunes. The owner's words were a click
// on a signal, and a click inside another receiver's band focuses that
// receiver without tuning anything: the receiver window changes to it, so it
// comes forward too. Rejects raising on a click that did nothing, which would
// pull the window over the span for a second click that added no receiver.
TEST_CASE("the clicks that change the receiver window bring it forward", "[raise]")
{
    CHECK(click_brings_receivers_forward(SpanClick::Focus));
    CHECK(click_brings_receivers_forward(SpanClick::Retune));
    CHECK(click_brings_receivers_forward(SpanClick::Open));
    CHECK(click_brings_receivers_forward(SpanClick::Add));
    CHECK_FALSE(click_brings_receivers_forward(SpanClick::Nothing));
    CHECK_FALSE(click_brings_receivers_forward(SpanClick::Full));
}

// The same through the click rule, from the inputs EngineLink::spanClick
// gathers, so a change to classify_span_click that moved a gesture from one
// kind to another shows up here as a window that stops coming forward.
TEST_CASE("a click on a detection and a click on another receiver both raise it", "[raise]")
{
    // A detection clicked with a receiver focused elsewhere: a retune.
    SpanClickInput on_detection;
    on_detection.had_receiver = true;
    on_detection.count = 1;
    CHECK(click_brings_receivers_forward(classify_span_click(on_detection)));

    // The first click on an empty rack opens a receiver.
    SpanClickInput first;
    CHECK(click_brings_receivers_forward(classify_span_click(first)));

    // Inside another receiver's band: a focus.
    SpanClickInput on_other;
    on_other.under = 7;
    on_other.had_receiver = true;
    on_other.count = 2;
    CHECK(click_brings_receivers_forward(classify_span_click(on_other)));
}

// Rejects raise() alone on a window that is not on screen: a hidden window
// raised stays hidden, and a minimised one stays on the taskbar.
TEST_CASE("a hidden window is shown and a minimised one restored", "[raise]")
{
    const auto hidden = plan_receiver_window_raise(ReceiverWindowState::Hidden);
    CHECK(hidden.show);
    CHECK_FALSE(hidden.restore);
    CHECK(hidden.raise);
    CHECK(hidden.activate);

    const auto minimised = plan_receiver_window_raise(ReceiverWindowState::Minimised);
    CHECK_FALSE(minimised.show);
    CHECK(minimised.restore);
    CHECK(minimised.raise);
    CHECK(minimised.activate);
}

// Rejects asking for activation of a window that is already on screen. The
// operator clicked in the main window and may be about to scroll or type
// there, so keyboard focus is left where it is and the window is only raised.
TEST_CASE("a window already on screen is raised without asking for focus", "[raise]")
{
    const auto plan = plan_receiver_window_raise(ReceiverWindowState::OnScreen);
    CHECK(plan.raise);
    CHECK_FALSE(plan.show);
    CHECK_FALSE(plan.restore);
    CHECK_FALSE(plan.activate);
    CHECK(plan.any());
}
