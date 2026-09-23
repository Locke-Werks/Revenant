// plan_history_shift: what the passband waterfall does with its history when
// the receiver moves.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

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
}

TEST_CASE("no width or no span is a reset with nothing to draw on", "[historyshift]")
{
    CHECK(plan_history_shift(HistoryAxis{kLow, 50.0}, kLow, kHigh, 0).reset);
    CHECK(plan_history_shift(HistoryAxis{kLow, 50.0}, kHigh, kLow, kWidth).reset);
}
