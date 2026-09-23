// step_dial and its helpers: what one notch of the wheel on one digit of a
// frequency dial does.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. The wrong implementations of a dial are all plausible
// on screen: a digit that wraps on its own looks like a digit being stepped,
// a limit that refuses the step looks like a wheel that stopped working, and
// a count of digits taken from the value alone looks fine until the value
// crosses a power of ten and the whole dial jumps sideways under the pointer.

#include <catch2/catch_test_macros.hpp>

#include "models/frequency_dial.h"

using revenant::ui::dial_digit;
using revenant::ui::dial_digit_count;
using revenant::ui::dial_place;
using revenant::ui::dial_separator_after;
using revenant::ui::dial_significant_digits;
using revenant::ui::DialLimits;
using revenant::ui::DialSeparator;
using revenant::ui::kDialMaxDigits;
using revenant::ui::kDialMinDigits;
using revenant::ui::step_dial;

namespace {

// An R820T's tuning range, which is the ordinary limits a dial here meets.
constexpr DialLimits kR820t{24'000'000, 1'766'000'000};
constexpr DialLimits kUnknown{};

}  // namespace

TEST_CASE("a digit is read from the magnitude, hertz digit first", "[dial]")
{
    CHECK(dial_digit(98'100'000, 0) == 0);
    CHECK(dial_digit(98'100'000, 5) == 1);
    CHECK(dial_digit(98'100'000, 6) == 8);
    CHECK(dial_digit(98'100'000, 7) == 9);
    CHECK(dial_digit(98'100'000, 8) == 0);

    // Rejects reading the digits of a negative value through the remainder
    // operator, which gives negative digits in C++.
    CHECK(dial_digit(-154'652, 0) == 2);
    CHECK(dial_digit(-154'652, 5) == 1);

    // Outside what a dial shows is zero, not a crash.
    CHECK(dial_digit(98'100'000, -1) == 0);
    CHECK(dial_digit(98'100'000, kDialMaxDigits) == 0);
    CHECK(dial_place(kDialMaxDigits) == 0);
}

// Rejects a digit that wraps on its own. On paper, and on the spectrum under
// the dial, 98.9 plus 0.1 is 99.0, and a dial that showed 98.0 there would
// have moved the radio 900 kHz the wrong way.
TEST_CASE("a nine stepped up carries into the digit above", "[dial]")
{
    const auto up = step_dial(98'900'000, 5, 1, kR820t);
    CHECK(up.hz == 99'000'000);
    CHECK_FALSE(up.clamped);

    // All the way through several nines, which is the case a per-digit carry
    // written as a single look at the next digit gets wrong.
    CHECK(step_dial(99'999'999, 0, 1, kR820t).hz == 100'000'000);
}

TEST_CASE("a zero stepped down borrows from the digit above", "[dial]")
{
    CHECK(step_dial(99'000'000, 5, -1, kR820t).hz == 98'900'000);
    CHECK(step_dial(100'000'000, 0, -1, kR820t).hz == 99'999'999);
}

// Several notches arrive as one event on a fast wheel, and each is a place
// value, not one.
TEST_CASE("notches multiply the place value", "[dial]")
{
    CHECK(step_dial(98'100'000, 3, 5, kR820t).hz == 98'105'000);
    CHECK(step_dial(98'100'000, 3, -5, kR820t).hz == 98'095'000);
}

// Rejects a limit that refuses the step. The last notch of a spin toward the
// top of the tuner has to land on the top, or the dial stops a few megahertz
// short with the wheel still turning and nothing saying why.
TEST_CASE("a step past a limit lands on the limit and says so", "[dial]")
{
    const auto high = step_dial(1'760'000'000, 7, 1, kR820t);
    CHECK(high.hz == 1'766'000'000);
    CHECK(high.clamped);

    const auto low = step_dial(30'000'000, 7, -1, kR820t);
    CHECK(low.hz == 24'000'000);
    CHECK(low.clamped);

    // Already at the limit and pushed further is still the limit.
    CHECK(step_dial(24'000'000, 0, -1, kR820t).hz == 24'000'000);
}

// Rejects clamping against a pair of zeros. A source that has not said what
// it tunes would otherwise pin every step to zero hertz.
TEST_CASE("unknown limits clamp nothing", "[dial]")
{
    const auto step = step_dial(98'100'000, 6, 1, kUnknown);
    CHECK(step.hz == 99'100'000);
    CHECK_FALSE(step.clamped);

    // Including across zero, which a synthetic scene centred there reaches.
    CHECK(step_dial(500, 3, -1, kUnknown).hz == -500);
}

TEST_CASE("a step on a digit the dial does not have moves nothing", "[dial]")
{
    CHECK(step_dial(98'100'000, kDialMaxDigits, 1, kR820t).hz == 98'100'000);
    CHECK(step_dial(98'100'000, -1, 1, kR820t).hz == 98'100'000);
}

TEST_CASE("significant digits count a zero as one", "[dial]")
{
    CHECK(dial_significant_digits(0) == 1);
    CHECK(dial_significant_digits(9) == 1);
    CHECK(dial_significant_digits(10) == 2);
    CHECK(dial_significant_digits(98'100'000) == 8);
    CHECK(dial_significant_digits(-154'652) == 6);
}

// Rejects a width taken from the value alone. Tuned from 999 MHz to 1 GHz, a
// dial that grows a digit moves every other digit one place left, so the one
// under the pointer is suddenly a different digit and the next notch steps a
// thousand times further than the last one did.
TEST_CASE("the dial is as wide as the top of the range", "[dial]")
{
    CHECK(dial_digit_count(98'100'000, kR820t) == 10);
    CHECK(dial_digit_count(1'700'000'000, kR820t) == 10);

    // With no range known it follows the value, never narrower than the
    // minimum, so an HF receiver's dial does not change width at 10 MHz.
    CHECK(dial_digit_count(7'100'000, kUnknown) == kDialMinDigits);
    CHECK(dial_digit_count(14'100'000, kUnknown) == kDialMinDigits);
    CHECK(dial_digit_count(1'090'000'000, kUnknown) == 10);
}

TEST_CASE("the groups are megahertz, kilohertz and hertz", "[dial]")
{
    CHECK(dial_separator_after(6) == DialSeparator::Point);
    CHECK(dial_separator_after(3) == DialSeparator::Gap);
    CHECK(dial_separator_after(9) == DialSeparator::Gap);
    CHECK(dial_separator_after(0) == DialSeparator::None);
    CHECK(dial_separator_after(7) == DialSeparator::None);
}
