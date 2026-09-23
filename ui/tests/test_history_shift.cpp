// plan_history_shift and shift_row: what both waterfalls do with their
// history when their axis moves, the passband one when the receiver moves and
// the span one when the front end retunes.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>

#include "render/history_shift.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::HistoryAxis;
using revenant::ui::plan_history_shift;

namespace {

// A 48 kHz nfm display 960 pixels wide: 50 Hz a pixel.
constexpr double kLow = 98'076'000.0;
constexpr double kHigh = 98'124'000.0;
constexpr int kWidth = 960;

}  // namespace

TEST_CASE("the first frame starts a history", "[historyshift]")
{
    const auto plan = plan_history_shift(HistoryAxis{}, kLow, kHigh, kWidth);
    CHECK(plan.reset);
    CHECK(plan.axis.low_hz == kLow);
    CHECK(plan.axis.hz_per_px == 50.0);
}

TEST_CASE("an unmoved axis shifts nothing", "[historyshift]")
{
    const auto plan = plan_history_shift(HistoryAxis{kLow, 50.0}, kLow, kHigh, kWidth);
    CHECK_FALSE(plan.reset);
    CHECK(plan.shift_px == 0);
}

// Rejects a shift in the wrong direction, which walks a signal away from where
// it was instead of holding it. The receiver moving up puts the display's left
// edge higher, so a signal already stored is further left.
TEST_CASE("the receiver moving up moves the history left", "[historyshift]")
{
    const auto plan =
        plan_history_shift(HistoryAxis{kLow, 50.0}, kLow + 500.0, kHigh + 500.0, kWidth);
    CHECK_FALSE(plan.reset);
    CHECK(plan.shift_px == -10);
    CHECK(plan.axis.low_hz == kLow + 500.0);
}

// Rejects rounding each move on its own and forgetting the remainder, which
// lets a drifting receiver's history walk off by a pixel every few moves. A
// hundred moves of 30 Hz, 0.6 of a pixel each, have to land within half a
// pixel of 3 kHz.
TEST_CASE("the remainder carries, so many small moves do not drift", "[historyshift]")
{
    HistoryAxis axis{kLow, 50.0};
    int total_px = 0;
    for (int i = 1; i <= 100; ++i) {
        const double low = kLow + 30.0 * i;
        const auto plan = plan_history_shift(axis, low, low + (kHigh - kLow), kWidth);
        REQUIRE_FALSE(plan.reset);
        total_px += plan.shift_px;
        axis = plan.axis;
        CHECK(std::fabs(axis.low_hz - low) <= 25.0);
    }
    CHECK(total_px == -60);
}

// Rejects stretching the history to a new width, which draws every stored
// signal at a width it never had.
TEST_CASE("a different span discards the history", "[historyshift]")
{
    const auto plan =
        plan_history_shift(HistoryAxis{kLow, 50.0}, kLow, kLow + 96'000.0, kWidth);
    CHECK(plan.reset);
    CHECK_THAT(plan.axis.hz_per_px, WithinAbs(100.0, 1e-9));
}

// Rejects a history keyed on the filter's width, which is what this was while
// the pane was the fine stream and its span was the demodulation rate. The
// span is half the display rate now, and docs/ui-spectrum.md's own example is
// a USB high edge dragged from 1 kHz to 20 kHz in 250 Hz steps on a 75 kS/s
// channel, which moves the display rate at three of the 77 positions. Frames
// at every other position carry the same axis and must keep every row.
TEST_CASE("only a rung of the display rate discards the history", "[historyshift]")
{
    // The display rate at each position, by the rate rule: the largest rung
    // of 1, 2, 4, 8, 20 that keeps the pane at least four reaches wide, where
    // a USB band [0, B] reaches B.
    const auto display_rate = [](double high_edge_hz) {
        for (const double rung : {20.0, 8.0, 4.0, 2.0, 1.0}) {
            const double rate = 75'000.0 / rung;
            if (rate / 2.0 >= 4.0 * high_edge_hz) {
                return rate;
            }
        }
        return 75'000.0;
    };

    constexpr double kMix = 14'230'000.0;
    HistoryAxis axis;
    int resets = 0;
    for (int step = 0; step <= 76; ++step) {
        const double high = 1'000.0 + 250.0 * step;
        const double pane = display_rate(high) / 2.0;
        const auto plan = plan_history_shift(axis, kMix - pane / 2.0, kMix + pane / 2.0, kWidth);
        if (step > 0 && plan.reset) {
            ++resets;
        }
        if (!plan.reset) {
            CHECK(plan.shift_px == 0);
        }
        axis = plan.axis;
    }
    CHECK(resets == 3);
}

TEST_CASE("a move of the whole width or more is a fresh start", "[historyshift]")
{
    const auto plan = plan_history_shift(HistoryAxis{kLow, 50.0}, kLow + 48'000.0,
                                         kHigh + 48'000.0, kWidth);
    CHECK(plan.reset);

    // And says it moved rather than rescaled, so the span waterfall can blank
    // its pixels and keep the sample ranges beside them.
    CHECK(plan.beyond);
}

// Rejects reading every reset as a move off the display, which would keep a
// time record against rows drawn at another scale.
TEST_CASE("a change of scale is not a move beyond the width", "[historyshift]")
{
    const auto plan =
        plan_history_shift(HistoryAxis{kLow, 50.0}, kLow, kLow + 96'000.0, kWidth);
    CHECK(plan.reset);
    CHECK_FALSE(plan.beyond);
    CHECK_FALSE(plan_history_shift(HistoryAxis{}, kLow, kHigh, kWidth).beyond);
}

TEST_CASE("no width or no span is a reset with nothing to draw on", "[historyshift]")
{
    CHECK(plan_history_shift(HistoryAxis{kLow, 50.0}, kLow, kHigh, 0).reset);
    CHECK(plan_history_shift(HistoryAxis{kLow, 50.0}, kHigh, kLow, kWidth).reset);
}

// The main span's case, which is the owner's report of 2026-09-23: a retune by
// a twentieth of a 2.4 MHz span, the wheel's step, on a waterfall 1600 pixels
// wide. Rejects clearing the history on a tune, which is what the waterfall
// did, and rejects a shift of the wrong size, which smears every stored
// carrier off its frequency.
TEST_CASE("a front-end retune slides the span history by the tune", "[historyshift]")
{
    constexpr double kSpan = 2'400'000.0;
    constexpr int kSpanWidth = 1600;
    constexpr double kCentre = 98'100'000.0;
    const HistoryAxis stored{kCentre - kSpan / 2.0, kSpan / kSpanWidth};

    const double tuned = kCentre + kSpan / 20.0;
    const auto plan =
        plan_history_shift(stored, tuned - kSpan / 2.0, tuned + kSpan / 2.0, kSpanWidth);
    CHECK_FALSE(plan.reset);

    // 120 kHz at 1500 Hz a pixel is 80 pixels, and the radio moving up puts
    // what was stored further left.
    CHECK(plan.shift_px == -80);
    CHECK_THAT(plan.axis.low_hz, WithinAbs(tuned - kSpan / 2.0, 1e-6));
}

// The pixels. Rejects a shift that leaves the old pixels where they were at
// the edge the history moved away from, which draws the carrier twice.
TEST_CASE("shift_row moves pixels and fills what comes in from outside", "[historyshift]")
{
    using revenant::ui::shift_row;
    constexpr std::uint32_t kFill = 0xFF000000U;

    std::uint32_t right[6] = {1, 2, 3, 4, 5, 6};
    shift_row(right, 6, 2, kFill);
    CHECK(right[0] == kFill);
    CHECK(right[1] == kFill);
    CHECK(right[2] == 1);
    CHECK(right[5] == 4);

    std::uint32_t left[6] = {1, 2, 3, 4, 5, 6};
    shift_row(left, 6, -2, kFill);
    CHECK(left[0] == 3);
    CHECK(left[3] == 6);
    CHECK(left[4] == kFill);
    CHECK(left[5] == kFill);

    std::uint32_t gone[4] = {1, 2, 3, 4};
    shift_row(gone, 4, -9, kFill);
    for (const std::uint32_t pixel : gone) {
        CHECK(pixel == kFill);
    }

    std::uint32_t still[3] = {7, 8, 9};
    shift_row(still, 3, 0, kFill);
    CHECK(still[0] == 7);
    CHECK(still[2] == 9);
}
