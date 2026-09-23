// plan_ruler: the ticks and labels on the frequency ruler between the
// spectrum and the waterfall.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. A ruler that is wrong still looks like a ruler, which
// is the whole difficulty: labels that overlap at one width, a tick drawn
// from a mapping that drifts from the displays' toward the edges, a label
// that rounds 100.3 down to 100.299.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>

#include "models/ruler.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::kRulerLabelGapPx;
using revenant::ui::kRulerMinorMinPx;
using revenant::ui::plan_ruler;
using revenant::ui::ruler_decimals;
using revenant::ui::ruler_label;
using revenant::ui::ruler_minor_step;
using revenant::ui::ruler_next_step;
using revenant::ui::ruler_unit_hz;
using revenant::ui::ruler_x;
using revenant::ui::RulerPlan;

namespace {

// Broadcast FM on an R820T at 2.4 MS/s, with the half-bin edges the engine's
// 8192-bin frame puts either side.
constexpr double kFmLow = 96'900'000.0 - 146.484375;
constexpr double kFmHigh = 99'300'000.0 - 146.484375;

// Cascadia Mono at 11 px is about 6.6 px a character.
constexpr double kCharPx = 6.6;

// Every pair of neighbouring labels has at least the gap between them.
void check_no_overlap(const RulerPlan& plan)
{
    double last_right = -1.0e9;
    for (const auto& tick : plan.ticks) {
        if (tick.label.empty()) {
            continue;
        }
        const double half = static_cast<double>(tick.label.size()) * kCharPx / 2.0;
        CHECK(tick.x_px - half >= last_right + kRulerLabelGapPx - 1e-9);
        last_right = tick.x_px + half;
    }
}

}  // namespace

TEST_CASE("the step walks 1, 2, 5 up the decades", "[ruler]")
{
    CHECK(ruler_next_step(1) == 2);
    CHECK(ruler_next_step(2) == 5);
    CHECK(ruler_next_step(5) == 10);
    CHECK(ruler_next_step(10) == 20);
    CHECK(ruler_next_step(500'000) == 1'000'000);
}

// Rejects minor ticks that land between round numbers. Quarters of a 1 put a
// tick at 0.25, and fifths of a 2 at 0.4, both of which read as a mistake.
TEST_CASE("minor ticks land on round numbers", "[ruler]")
{
    CHECK(ruler_minor_step(1'000'000) == 200'000);
    CHECK(ruler_minor_step(2'000'000) == 500'000);
    CHECK(ruler_minor_step(5'000'000) == 1'000'000);

    // Nothing finer than a hertz exists to draw.
    CHECK(ruler_minor_step(1) == 0);
    CHECK(ruler_minor_step(2) == 0);
}

TEST_CASE("the unit follows the largest magnitude in view", "[ruler]")
{
    CHECK(ruler_unit_hz(kFmLow, kFmHigh) == 1'000'000);
    CHECK(ruler_unit_hz(530'000.0, 1'700'000.0) == 1'000'000);
    CHECK(ruler_unit_hz(530'000.0, 900'000.0) == 1'000);
    CHECK(ruler_unit_hz(-1'350'000.0, 1'050'000.0) == 1'000'000);
    CHECK(ruler_unit_hz(-500.0, 500.0) == 1);

    // ADS-B stays in megahertz, with the thousands set apart, rather than
    // reading 1.090 in gigahertz.
    CHECK(ruler_unit_hz(1'089'000'000.0, 1'091'000'000.0) == 1'000'000);
}

// Rejects labels formatted through a double, which print 100.3 MHz as
// 100.299 999 and turn a leading zero in the fraction into nothing.
TEST_CASE("labels are exact and grouped in threes", "[ruler]")
{
    CHECK(ruler_label(100'300'000, 1'000'000, 1) == "100.3");
    CHECK(ruler_label(98'100'000, 1'000'000, 3) == "98.100");
    CHECK(ruler_label(98'100'500, 1'000'000, 4) == "98.100 5");
    CHECK(ruler_label(162'005'000, 1'000'000, 3) == "162.005");
    CHECK(ruler_label(1'090'000'000, 1'000'000, 0) == "1 090");
    CHECK(ruler_label(-500'000, 1'000'000, 1) == "-0.5");
    CHECK(ruler_label(0, 1'000'000, 1) == "0.0");
}

TEST_CASE("decimals are what the step needs and no more", "[ruler]")
{
    CHECK(ruler_decimals(1'000'000, 1'000'000) == 0);
    CHECK(ruler_decimals(200'000, 1'000'000) == 1);
    CHECK(ruler_decimals(50'000, 1'000'000) == 2);
    CHECK(ruler_decimals(500, 1'000'000) == 4);
    CHECK(ruler_decimals(1, 1'000'000) == 6);
    CHECK(ruler_decimals(7, 1) == 0);
}

// Rejects a mapping of its own. A tick placed by anything but the displays'
// edge-to-edge stretch agrees with the bins at the centre and drifts from
// them toward the ends, which is where nobody checks it.
TEST_CASE("the span's edges are the ruler's edges, exactly", "[ruler]")
{
    CHECK(ruler_x(kFmLow, kFmLow, kFmHigh, 1577.0) == 0.0);
    CHECK(ruler_x(kFmHigh, kFmLow, kFmHigh, 1577.0) == 1577.0);
    CHECK_THAT(ruler_x(98'100'000.0, kFmLow, kFmHigh, 1600.0),
               WithinAbs((98'100'000.0 - kFmLow) / (kFmHigh - kFmLow) * 1600.0, 1e-9));

    // A tick exactly on an edge is drawn on it.
    const auto plan = plan_ruler(96'000'000.0, 100'000'000.0, 800.0, kCharPx);
    REQUIRE_FALSE(plan.ticks.empty());
    CHECK(plan.ticks.front().hz == 96'000'000);
    CHECK(plan.ticks.front().x_px == 0.0);
    CHECK(plan.ticks.back().hz == 100'000'000);
    CHECK(plan.ticks.back().x_px == 800.0);
}

TEST_CASE("a broadcast FM span at a full-width window", "[ruler]")
{
    const auto plan = plan_ruler(kFmLow, kFmHigh, 1577.0, kCharPx);

    CHECK(plan.unit == "MHz");
    CHECK(plan.major_hz == 100'000);
    CHECK(plan.decimals == 1);
    CHECK(plan.minor_hz == 20'000);
    check_no_overlap(plan);

    // Every tick is inside the span, only major ones carry a label, and a
    // label is never cut by an edge.
    for (const auto& tick : plan.ticks) {
        CHECK(tick.x_px >= 0.0);
        CHECK(tick.x_px <= 1577.0);
        if (!tick.label.empty()) {
            CHECK(tick.major);
            const double half = static_cast<double>(tick.label.size()) * kCharPx / 2.0;
            CHECK(tick.x_px - half >= 0.0);
            CHECK(tick.x_px + half <= 1577.0);
        }
    }

    // And a label is dropped rather than moved: a reserved start keeps the
    // first one off the unit.
    const auto reserved = plan_ruler(kFmLow, kFmHigh, 1577.0, kCharPx, 60.0);
    for (const auto& tick : reserved.ticks) {
        if (!tick.label.empty()) {
            CHECK(tick.x_px - static_cast<double>(tick.label.size()) * kCharPx / 2.0 >= 60.0);
        }
    }
}

// Rejects a step chosen for one width and kept for all. The same span in a
// narrow window has to thin its labels out until they no longer touch, at
// every width down to one where a single label barely fits.
TEST_CASE("labels never collide, however narrow the window", "[ruler]")
{
    for (double width = 1600.0; width >= 60.0; width -= 7.0) {
        const auto plan = plan_ruler(kFmLow, kFmHigh, width, kCharPx);
        check_no_overlap(plan);
    }
}

TEST_CASE("minor ticks are dropped before they turn into a comb", "[ruler]")
{
    for (double width = 1600.0; width >= 60.0; width -= 13.0) {
        const auto plan = plan_ruler(kFmLow, kFmHigh, width, kCharPx);
        if (plan.minor_hz > 0) {
            CHECK(static_cast<double>(plan.minor_hz) / (kFmHigh - kFmLow) * width >=
                  kRulerMinorMinPx);
        }
    }
}

// A synthetic scene is centred on zero, so half its span is negative.
TEST_CASE("a span across zero is labelled with signs", "[ruler]")
{
    const auto plan = plan_ruler(-1'350'000.0, 1'050'000.0, 1500.0, kCharPx);
    CHECK(plan.unit == "MHz");
    check_no_overlap(plan);

    bool saw_negative = false;
    bool saw_zero = false;
    for (const auto& tick : plan.ticks) {
        if (!tick.label.empty() && tick.hz < 0) {
            saw_negative = true;
            CHECK(tick.label.front() == '-');
        }
        if (tick.major && tick.hz == 0) {
            saw_zero = true;
        }
    }
    CHECK(saw_negative);
    CHECK(saw_zero);
}

// Nothing to draw is an empty plan, not a division by zero.
TEST_CASE("no span, no width or no font is an empty ruler", "[ruler]")
{
    CHECK(plan_ruler(0.0, 0.0, 1000.0, kCharPx).ticks.empty());
    CHECK(plan_ruler(kFmHigh, kFmLow, 1000.0, kCharPx).ticks.empty());
    CHECK(plan_ruler(kFmLow, kFmHigh, 0.0, kCharPx).ticks.empty());
    CHECK(plan_ruler(kFmLow, kFmHigh, 1000.0, 0.0).ticks.empty());
}
