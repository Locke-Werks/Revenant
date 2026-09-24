// plan_level_scale: the dBFS ticks and labels up the edge of both spectrum
// traces.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. A scale that is wrong still looks like a scale: labels
// that overlap at one pane height, ticks placed by a mapping that is not the
// trace's, a label that prints -40.5 as -40.499999, a step that stays put
// while a pin narrows the span underneath it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <string>

#include "models/level_scale.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::kLevelLabelGapPx;
using revenant::ui::kLevelMinorMinPx;
using revenant::ui::level_label;
using revenant::ui::level_minor_step;
using revenant::ui::level_next_step;
using revenant::ui::level_y;
using revenant::ui::LevelScalePlan;
using revenant::ui::plan_level_scale;

namespace {

// Cascadia Mono at 11 px, as a QML Text reports it: 15 px a line.
constexpr double kLabelPx = 15.0;

// The detection labels' strip along the top of the span, which is
// kLabelTopPx plus overlay_label_height() in render/spectrum_item.h.
constexpr double kSpanTopReservePx = 26.0;

// Every pair of neighbouring labels has at least the gap between them, and
// every label is whole inside the room it was given.
void check_labels(const LevelScalePlan& plan, double height_px, double reserve_top_px = 0.0)
{
    double last_bottom = -1.0e9;
    for (const auto& tick : plan.ticks) {
        if (tick.label.empty()) {
            continue;
        }
        const double top = tick.y_px - kLabelPx / 2.0;
        const double bottom = tick.y_px + kLabelPx / 2.0;
        CHECK(top >= reserve_top_px - 1e-9);
        CHECK(bottom <= height_px + 1e-9);

        // Ticks run from the floor upward, so y falls: each label is above
        // the one before it and has to clear it.
        if (last_bottom > -1.0e9) {
            CHECK(bottom + kLevelLabelGapPx <= last_bottom + 1e-9);
        }
        last_bottom = top;
    }
}

[[nodiscard]] int label_count(const LevelScalePlan& plan)
{
    int count = 0;
    for (const auto& tick : plan.ticks) {
        count += tick.label.empty() ? 0 : 1;
    }
    return count;
}

}  // namespace

TEST_CASE("the step walks 1, 2, 5 up the decades in tenths", "[level_scale]")
{
    CHECK(level_next_step(1) == 2);
    CHECK(level_next_step(2) == 5);
    CHECK(level_next_step(5) == 10);
    CHECK(level_next_step(10) == 20);
    CHECK(level_next_step(50) == 100);
    CHECK(level_next_step(100) == 200);
}

// Rejects minor ticks between round numbers: quarters of a 10 dB step put one
// at 2.5 dB, and fifths of a 20 dB step one at 4 dB.
TEST_CASE("minor ticks land on round numbers", "[level_scale]")
{
    CHECK(level_minor_step(100) == 20);
    CHECK(level_minor_step(200) == 100);
    CHECK(level_minor_step(50) == 10);
    CHECK(level_minor_step(10) == 2);
    CHECK(level_minor_step(1) == 0);
}

// Rejects a label formatted through a double, and a minus sign left on zero.
TEST_CASE("labels are exact and carry the decimals the step needs", "[level_scale]")
{
    CHECK(level_label(-400, 0) == "-40");
    CHECK(level_label(-405, 1) == "-40.5");
    CHECK(level_label(-5, 1) == "-0.5");
    CHECK(level_label(0, 0) == "0");
    CHECK(level_label(0, 1) == "0.0");
    CHECK(level_label(-1200, 0) == "-120");
    CHECK(level_label(30, 0) == "3");
}

// Rejects a mapping of the scale's own. The trace draws the floor on the
// bottom edge and the ceiling on the top, and a tick placed any other way
// agrees in the middle and drifts toward the ends.
TEST_CASE("the trace's ends are the scale's ends, exactly", "[level_scale]")
{
    CHECK(level_y(-100.0, -100.0, -20.0, 240.0) == 240.0);
    CHECK(level_y(-20.0, -100.0, -20.0, 240.0) == 0.0);
    CHECK_THAT(level_y(-60.0, -100.0, -20.0, 240.0), WithinAbs(120.0, 1e-9));

    // Off either end it says which side, rather than clamping.
    CHECK(level_y(-10.0, -100.0, -20.0, 240.0) < 0.0);
    CHECK(level_y(-110.0, -100.0, -20.0, 240.0) > 240.0);

    const auto plan = plan_level_scale(-100.0, -20.0, 240.0, kLabelPx);
    REQUIRE_FALSE(plan.ticks.empty());
    CHECK(plan.ticks.front().db == -100.0);
    CHECK(plan.ticks.front().y_px == 240.0);
    CHECK(plan.ticks.back().db == -20.0);
    CHECK(plan.ticks.back().y_px == 0.0);
}

TEST_CASE("the span spectrum at its ordinary height", "[level_scale]")
{
    // A third of a 1080-line window's span, about 300 px, over the 60 to 80
    // dB the auto-scale spans on a live band.
    const auto plan = plan_level_scale(-112.4, -41.7, 300.0, kLabelPx, kSpanTopReservePx);

    CHECK(plan.major_tenths == 100);
    CHECK(plan.decimals == 0);
    CHECK(plan.minor_tenths == 20);
    check_labels(plan, 300.0, kSpanTopReservePx);
    CHECK(label_count(plan) >= 5);

    // Every label is a whole ten.
    for (const auto& tick : plan.ticks) {
        if (!tick.label.empty()) {
            CHECK(std::fmod(std::abs(tick.db), 10.0) == 0.0);
        }
    }
}

// Rejects a step chosen for one height and kept at another. The receiver's
// passband pane is let shrink to 96 px docked, and a 10 dB step there would
// put a label every 14 px, which is less than one label's height.
TEST_CASE("the passband pane at its smallest keeps its labels apart", "[level_scale]")
{
    const auto plan = plan_level_scale(-95.0, -35.0, 96.0, kLabelPx);
    CHECK(plan.major_tenths == 200);
    check_labels(plan, 96.0);
    CHECK(label_count(plan) >= 2);
}

// Every height from a sliver to a tall window, and every span from the
// narrowest two pins allow to the widest auto-scale, keeps its labels apart.
TEST_CASE("no height and no span makes two labels touch", "[level_scale]")
{
    const double spans[] = {1.0, 3.7, 12.0, 25.0, 61.3, 90.0, 140.0, 200.0};
    for (const double span : spans) {
        for (double height = 20.0; height <= 1200.0; height += 7.0) {
            const auto plan = plan_level_scale(-120.0, -120.0 + span, height, kLabelPx);
            check_labels(plan, height);

            // The step is the smallest that fits: the one below it would not.
            if (plan.major_tenths > 1) {
                std::int64_t below = 1;
                while (level_next_step(below) < plan.major_tenths) {
                    below = level_next_step(below);
                }
                const double below_px = static_cast<double>(below) / 10.0 / span * height;
                CHECK(below_px < kLabelPx + kLevelLabelGapPx);
            }
        }
    }
}

// Rejects a scale planned from the frame's ends rather than the drawn ones.
// A pin moves the drawn ends and nothing else, and the scale has to move with
// it: the operator pinning both ends 6 dB apart to compare two carriers gets
// a scale in single decibels, not one tick at each end.
TEST_CASE("two pins close together give a fine step", "[level_scale]")
{
    const auto plan = plan_level_scale(-62.0, -56.0, 300.0, kLabelPx);
    CHECK(plan.major_tenths == 10);
    CHECK(plan.decimals == 0);

    // -61 to -57: the ticks at the two ends sit on the edges, where half a
    // label would be outside the pane.
    CHECK(label_count(plan) == 5);
    check_labels(plan, 300.0);

    // The narrowest two pins may be, a decibel, steps in tenths.
    const auto narrow = plan_level_scale(-60.0, -59.0, 300.0, kLabelPx);
    CHECK(narrow.major_tenths == 1);
    CHECK(narrow.decimals == 1);
    check_labels(narrow, 300.0);
    bool saw_half = false;
    for (const auto& tick : narrow.ticks) {
        saw_half = saw_half || tick.label == "-59.5";
    }
    CHECK(saw_half);
}

// Rejects labels pulled in to fit. The top of the span is where the
// detection labels are, and a level label pulled down into that strip sits
// on a plate naming a signal; its tick is kept, its label is not.
TEST_CASE("labels stay out of the reserved strips", "[level_scale]")
{
    const auto plan = plan_level_scale(-100.0, -20.0, 320.0, kLabelPx, kSpanTopReservePx);
    check_labels(plan, 320.0, kSpanTopReservePx);

    bool dropped_at_top = false;
    for (const auto& tick : plan.ticks) {
        if (tick.major && tick.y_px < kSpanTopReservePx) {
            CHECK(tick.label.empty());
            dropped_at_top = true;
        }
    }
    CHECK(dropped_at_top);

    // The tick at the very top is still drawn.
    CHECK(plan.ticks.back().db == -20.0);
}

TEST_CASE("minor ticks are drawn only when they are apart", "[level_scale]")
{
    // 10 dB majors with 2 dB minors: 2 dB over 80 dB in 320 px is 8 px.
    const auto roomy = plan_level_scale(-100.0, -20.0, 320.0, kLabelPx);
    CHECK(roomy.minor_tenths == 20);

    // The same span in a short pane: 20 dB majors, and 10 dB minors at 20 px.
    const auto short_pane = plan_level_scale(-100.0, -20.0, 160.0, kLabelPx);
    CHECK(short_pane.major_tenths == 200);
    CHECK(short_pane.minor_tenths == 100);

    // A minor step under the minimum is dropped rather than drawn as a smear.
    for (double height = 40.0; height < 1000.0; height += 13.0) {
        const auto plan = plan_level_scale(-110.0, -30.0, height, kLabelPx);
        if (plan.minor_tenths > 0) {
            CHECK(static_cast<double>(plan.minor_tenths) / 10.0 / 80.0 * height >=
                  kLevelMinorMinPx - 1e-9);
        }
    }
}

// Rejects a scale that draws something from nothing: before the first frame
// the ends are defaults nobody measured.
TEST_CASE("no span or no room is no scale", "[level_scale]")
{
    CHECK(plan_level_scale(-50.0, -50.0, 300.0, kLabelPx).ticks.empty());
    CHECK(plan_level_scale(-40.0, -50.0, 300.0, kLabelPx).ticks.empty());
    CHECK(plan_level_scale(-100.0, -20.0, 0.0, kLabelPx).ticks.empty());
    CHECK(plan_level_scale(-100.0, -20.0, 300.0, 0.0).ticks.empty());
    CHECK(plan_level_scale(std::nan(""), -20.0, 300.0, kLabelPx).ticks.empty());
}
