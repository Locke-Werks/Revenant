// receiver_gone_sentence: what the window says when the engine let a receiver
// go, and what it refuses to claim about why.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. The wrong implementations here are all versions of
// saying more than is known: the window learns a receiver is gone by asking
// for the engine's inventory and not finding it, which says nothing at all
// about the cause. A sentence that always blames the retune is right most of
// the time and wrong exactly when another client removed the receiver, which
// is the case an operator would most need the truth about.

#include <catch2/catch_test_macros.hpp>

#include "models/receiver_gone.h"

using revenant::ui::megahertz_text;
using revenant::ui::receiver_gone_sentence;
using revenant::ui::receiver_retuned_away_sentence;

namespace {

// Broadcast FM on an R820T at 2.4 MS/s, which is where this failure was seen.
constexpr double kSpanLow = 96'900'000.0;
constexpr double kSpanHigh = 99'300'000.0;

}  // namespace

// The case the sentence exists for: the wheel walked the front end away and
// the receiver could not come. The window can check this one for itself, so it
// is the one time a cause is offered.
TEST_CASE("a receiver outside the span is told it was left behind", "[receivergone]")
{
    const std::string said = receiver_gone_sentence(146'520'000, kSpanLow, kSpanHigh);

    CHECK(said == "146.520 MHz is outside the span now, so the receiver there was let go");
}

// Rejects a sentence that blames the retune whatever happened. A receiver
// whose frequency is still inside the span was not left behind by the radio
// moving, so something else took it: another client's remove, or an engine
// that restarted. Naming the span there would be a guess dressed as a finding,
// and it would send an operator looking at their own tuning for a fault that
// is somewhere else entirely.
TEST_CASE("a receiver still inside the span gets no invented cause", "[receivergone]")
{
    const std::string said = receiver_gone_sentence(98'100'000, kSpanLow, kSpanHigh);

    CHECK(said == "98.100 MHz: the engine no longer has that receiver");
}

// Rejects a rule that treats an unreported span as "outside it". A source that
// has just opened has not said what it covers, and comparing against two
// zeroes puts every frequency outside the span, so the window would explain a
// receiver's disappearance with a retune that never happened.
TEST_CASE("no span means no cause offered", "[receivergone]")
{
    const std::string said = receiver_gone_sentence(98'100'000, 0.0, 0.0);
    CHECK(said == "98.100 MHz: the engine no longer has that receiver");

    // Reversed ends are not a span either.
    CHECK(receiver_gone_sentence(98'100'000, kSpanHigh, kSpanLow) == said);
}

// The edges, because a sweep stops where it stops and a receiver sitting
// exactly on the boundary is the one the engine had to decide about. Inside is
// inside, matching the rule the engine itself applies.
TEST_CASE("the span edges are inside the span", "[receivergone]")
{
    CHECK(receiver_gone_sentence(96'900'000, kSpanLow, kSpanHigh) ==
          "96.900 MHz: the engine no longer has that receiver");
    CHECK(receiver_gone_sentence(99'300'000, kSpanLow, kSpanHigh) ==
          "99.300 MHz: the engine no longer has that receiver");

    CHECK(receiver_gone_sentence(96'899'999, kSpanLow, kSpanHigh) ==
          "96.899 MHz is outside the span now, so the receiver there was let go");
    CHECK(receiver_gone_sentence(99'300'001, kSpanLow, kSpanHigh) ==
          "99.300 MHz is outside the span now, so the receiver there was let go");
}

// The engine's own answer to a retune names the removed receiver and the
// frequency it was on, so this sentence states the cause outright. Rejects
// reading the frequency back off the window's own record, which is the guess
// the inventory path has to make, and rejects a sentence for nothing.
TEST_CASE("a receiver the retune removed is told so, at the engine's frequency",
          "[receivergone]")
{
    CHECK(receiver_retuned_away_sentence(146'520'000) ==
          "the front end moved off 146.520 MHz, so the receiver there was let go");
    CHECK(receiver_retuned_away_sentence(0).empty());
}

// Nothing to say when there was no receiver. Zero is not a frequency the
// window ever tuned to, so it is how "there was nothing here" arrives.
TEST_CASE("no receiver means no sentence", "[receivergone]")
{
    CHECK(receiver_gone_sentence(0, kSpanLow, kSpanHigh).empty());
}

// Rejects a frequency formatted through a double, for the reason
// models/bookmarks.h gives: 100.3 is not representable, and a line that reads
// 100.299 once beside a bookmark list that reads 100.300 describes two
// different stations.
TEST_CASE("a frequency is written the same way it is written everywhere else",
          "[receivergone]")
{
    CHECK(megahertz_text(100'300'000) == "100.300 MHz");
    CHECK(megahertz_text(98'100'000) == "98.100 MHz");

    // Truncated rather than rounded, so 7.199 does not become the 7.200 that
    // is a different frequency somebody may also be holding.
    CHECK(megahertz_text(7'199'900) == "7.199 MHz");

    // Leading zeros in the fraction, which a bare to_string of the remainder
    // drops and turns 162.005 into 162.5.
    CHECK(megahertz_text(162'005'000) == "162.005 MHz");
    CHECK(megahertz_text(162'000'000) == "162.000 MHz");
}
