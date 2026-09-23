// The step arithmetic behind the tuning, page and volume keys, in
// models/key_steps.h.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS. A key that steps by
// the wrong amount is corrected by pressing it again, so what is worth
// asserting is where a step stops: past the digits the dial draws, past the
// tuner's range, past either end of the volume.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "models/key_steps.h"

using Catch::Approx;
using revenant::ui::DialLimits;
using revenant::ui::kDefaultTuningDigit;
using revenant::ui::move_tuning_digit;
using revenant::ui::page_tune_hz;
using revenant::ui::step_volume;

namespace {

// An R820T's tuning range and a 2.4 MS/s span on broadcast FM.
constexpr DialLimits kR820t{24'000'000, 1'766'000'000};
constexpr std::int64_t kCentre = 98'100'000;
constexpr std::int64_t kLow = kCentre - 1'200'000;
constexpr std::int64_t kHigh = kCentre + 1'200'000;

}  // namespace

// Rejects a tuning digit that walks off the dial. Stepping a place the dial
// does not draw moves the radio by an amount nobody can see on it.
TEST_CASE("the tuning digit stays on the dial")
{
    CHECK(kDefaultTuningDigit == 3);
    CHECK(move_tuning_digit(3, 1, 9) == 4);
    CHECK(move_tuning_digit(3, -1, 9) == 2);
    CHECK(move_tuning_digit(0, -1, 9) == 0);
    CHECK(move_tuning_digit(8, 1, 9) == 8);

    // A dial that has not drawn yet has one place, not none, so the digit
    // is still a digit.
    CHECK(move_tuning_digit(5, 0, 0) == 0);
}

// Rejects a page in hertz. A fixed page is a different fraction of the
// picture on every source rate; a tenth of the span is the same gesture on a
// dongle and on a 20 MS/s front end.
TEST_CASE("a page is a tenth of the span")
{
    CHECK(page_tune_hz(kCentre, kLow, kHigh, 1, kR820t) == kCentre + 240'000);
    CHECK(page_tune_hz(kCentre, kLow, kHigh, -1, kR820t) == kCentre - 240'000);
    CHECK(page_tune_hz(kCentre, kCentre - 10'000'000, kCentre + 10'000'000, 1, kR820t) ==
          kCentre + 2'000'000);

    // Rounded to whole hertz, never truncated towards zero on one side only.
    CHECK(page_tune_hz(0, 0, 15, 1, DialLimits{}) == 2);
    CHECK(page_tune_hz(0, 0, 15, -1, DialLimits{}) == -2);
}

// Rejects a page that leaves the tuner's range, which the engine would refuse
// and the banner would report. It stops at the edge, as a dial step does.
TEST_CASE("a page stops at the tuning limits")
{
    CHECK(page_tune_hz(24'100'000, 22'900'000, 25'300'000, -1, kR820t) == 24'000'000);
    CHECK(page_tune_hz(1'765'900'000, 1'764'700'000, 1'767'100'000, 1, kR820t) ==
          1'766'000'000);

    // Limits that are not a range yet clamp nothing.
    CHECK(page_tune_hz(kCentre, kLow, kHigh, 1, DialLimits{}) == kCentre + 240'000);

    // No span yet, no page.
    CHECK(page_tune_hz(kCentre, 0, 0, 1, kR820t) == kCentre);
}

// Rejects a volume that leaves [0, 1] or lands between the slider's steps.
TEST_CASE("the volume keys stay on the slider's grid and inside it")
{
    CHECK(step_volume(0.5, 1) == Approx(0.55));
    CHECK(step_volume(0.5, -1) == Approx(0.45));
    CHECK(step_volume(0.98, 1) == Approx(1.0));
    CHECK(step_volume(0.02, -1) == Approx(0.0));

    // A value from a drag that is off the key's grid lands back on the
    // slider's hundredths rather than carrying the drag's fraction along.
    CHECK(step_volume(0.333, 1) == Approx(0.38));
}
