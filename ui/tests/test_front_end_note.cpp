// front_end_sentence and front_end_is_fault, which are the rule that would
// have named three phantom tracks on 2026-09-20 for what they were.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives.
//
// The wrong implementations on this path are a line that says "your front
// end is overloading", which is a diagnosis the engine cannot support and
// which an operator stops believing the first time they chase it to a
// switching supply; a line on every state, which nobody reads; and one
// colour for both of the states that do speak, which makes a gain control
// moving look like a fault.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "models/front_end_note.h"

using revenant::ui::front_end_is_fault;
using revenant::ui::FrontEndSample;
using revenant::ui::front_end_sentence;

namespace {

[[nodiscard]] FrontEndSample running(FrontEndSample::State state, double slope = 0.0,
                                     double lift = 0.0)
{
    FrontEndSample sample;
    sample.state = state;
    sample.slope = slope;
    sample.floor_lift_db = lift;
    sample.engine_running = true;
    return sample;
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("a healthy front end says nothing at all", "[frontend]")
{
    // Both ordinary states are silent. An implementation that reported
    // "front end: steady" on every pass passes no case here, because a
    // status strip that always carries a line is one nobody reads and the
    // one line that matters arrives in a place the eye already skips.
    CHECK(front_end_sentence(running(FrontEndSample::State::Unmeasured)).empty());
    CHECK(front_end_sentence(running(FrontEndSample::State::Steady)).empty());

    CHECK_FALSE(front_end_is_fault(running(FrontEndSample::State::Unmeasured)));
    CHECK_FALSE(front_end_is_fault(running(FrontEndSample::State::Steady)));
}

TEST_CASE("a stopped engine says nothing, whatever it last saw", "[frontend]")
{
    // The verdict is a measurement over a window and a stopped engine is
    // not measuring, so its last answer is whatever was true when it
    // stopped. An implementation that ignored engine_running would freeze
    // a warning on screen about a band nothing is listening to, and
    // "engine stopped" is already on the status line and is the better
    // sentence.
    FrontEndSample stopped = running(FrontEndSample::State::FloorFollowsSignal, 2.9, 6.3);
    stopped.engine_running = false;

    CHECK(front_end_sentence(stopped).empty());
    CHECK_FALSE(front_end_is_fault(stopped));
}

TEST_CASE("a gain control moving is stated and is not a fault", "[frontend]")
{
    // A slope of one is a multiplication, which is what the tuner's own
    // AGC does with gain=auto, and it is why a waterfall breathes. An
    // implementation that stayed silent here leaves an operator looking
    // for a fault in the display; one that coloured it as a fault is
    // crying wolf about a setting.
    const FrontEndSample sample = running(FrontEndSample::State::SpanScales, 1.02, 4.1);
    const std::string said = front_end_sentence(sample);

    REQUIRE_FALSE(said.empty());
    CHECK(contains(said, "gain"));
    CHECK_FALSE(front_end_is_fault(sample));
}

TEST_CASE("the floor outrunning the signal carries both numbers", "[frontend]")
{
    // A slope alone says the floor is following and nothing about how far
    // it has got, and a lift alone says nothing about why. An
    // implementation that reported the verdict as a bare word leaves an
    // operator no way to tell a 1.6 from a 3.0, which is the difference
    // between a hint and a front end well past its intercept point.
    const FrontEndSample sample = running(FrontEndSample::State::FloorFollowsSignal, 2.9, 6.3);
    const std::string said = front_end_sentence(sample);

    REQUIRE_FALSE(said.empty());
    CHECK(contains(said, "2.9"));
    CHECK(contains(said, "6.3"));
    CHECK(contains(said, "gain"));
    CHECK(front_end_is_fault(sample));
}

TEST_CASE("the sentence states what was seen and offers the other cause", "[frontend]")
{
    // The measurement is a correlated floor lift, which several things
    // other than compression produce: core/detect/front_end.h lists them.
    // An implementation that wrote "your front end is overloading" states
    // a cause the engine cannot establish, and the first operator who
    // chases it to a switching power supply stops believing the line for
    // good. So the wording describes the observation and names the second
    // cause out loud.
    const std::string said =
        front_end_sentence(running(FrontEndSample::State::FloorFollowsSignal, 3.0, 8.0));

    CHECK(contains(said, "noise floor"));
    CHECK(contains(said, "interferer"));
    CHECK_FALSE(contains(said, "overload"));
}

TEST_CASE("the figures round to one decimal rather than printing six", "[frontend]")
{
    // std::to_string on a double gives six decimals, which puts
    // "2.899999" in a status line. Same rule and same reason as
    // source_pacing.h's factor_text, and it is checked because the
    // rounding is written out by hand in integers rather than handed to a
    // formatter that could bring a locale with it.
    const std::string said =
        front_end_sentence(running(FrontEndSample::State::FloorFollowsSignal, 2.8499, 0.04));

    CHECK(contains(said, "2.8"));
    CHECK(contains(said, "0.0"));
    CHECK_FALSE(contains(said, "2.85"));
}
