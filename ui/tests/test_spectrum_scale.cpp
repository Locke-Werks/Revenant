// render/spectrum_scale.h: the column reduction, the correction it forces on
// the floor, the colour map and the operator's pins.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows. These functions draw every pixel of both span displays
// and were tested only by looking at the picture until 2026-09-22, which is
// the one test a plausible wrong answer passes.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <vector>

#include "render/spectrum_scale.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::colour_argb_at;
using revenant::ui::colour_at;
using revenant::ui::kMinPinnedSpanDb;
using revenant::ui::kSpectrumMinimumSpanDb;
using revenant::ui::map_ends;
using revenant::ui::peak_reduction_headroom_db;
using revenant::ui::pin_level;
using revenant::ui::reduce_peak;
using revenant::ui::resolve_ends;
using revenant::ui::ScalePins;
using revenant::ui::set_ceiling_pin;
using revenant::ui::set_floor_pin;

// Rejects a mean. A one-bin carrier among seven hundred bins of noise is the
// thing the operator is looking for, and averaging it into its column makes
// it invisible.
TEST_CASE("a column draws the largest bin in its run", "[scale]")
{
    const std::vector<float> bins{-90.0F, -40.0F, -95.0F, -91.0F, -92.0F, -93.0F};
    std::vector<float> columns(2);
    reduce_peak(bins, columns);
    CHECK(columns[0] == -40.0F);
    CHECK(columns[1] == -91.0F);
}

// Rejects a float step, which accumulates and leaves the last column covering
// a bin more or fewer than it should: a frequency error at the right-hand
// edge. With integer tiling every bin lands in exactly one column.
TEST_CASE("the runs tile the span exactly", "[scale]")
{
    constexpr std::size_t kBins = 8192;
    for (const std::size_t count : {1577U, 1000U, 7U, 8192U}) {
        std::vector<float> bins(kBins, -100.0F);
        // A marker in the last bin must reach the last column and no other.
        bins.back() = 0.0F;
        std::vector<float> columns(count);
        reduce_peak(bins, columns);
        INFO(count);
        CHECK(columns.back() == 0.0F);
        for (std::size_t c = 0; c + 1 < count; ++c) {
            CHECK(columns[c] == -100.0F);
        }
    }
}

// More columns than bins repeats a bin rather than interpolating one, which
// would invent structure between two measurements.
TEST_CASE("more columns than bins steps rather than interpolating", "[scale]")
{
    const std::vector<float> bins{-10.0F, -20.0F};
    std::vector<float> columns(4);
    reduce_peak(bins, columns);
    CHECK(columns == std::vector<float>{-10.0F, -10.0F, -20.0F, -20.0F});
}

TEST_CASE("nothing to reduce leaves the columns alone", "[scale]")
{
    std::vector<float> columns{1.0F, 2.0F};
    reduce_peak(std::vector<float>{}, columns);
    CHECK(columns == std::vector<float>{1.0F, 2.0F});
}

// The headroom is the gap between the fifth percentile of one exponential
// and the expected largest of K. At K = 1 it is 10 log10(1 / -ln 0.95),
// which is about 12.9 dB: a display at one bin per column still draws a mean
// above the fifth percentile, so this is not zero.
TEST_CASE("the headroom at one bin a column is the exact figure", "[scale]")
{
    const double expected = 10.0 * std::log10(1.0 / -std::log(0.95));
    CHECK_THAT(peak_reduction_headroom_db(1), WithinAbs(expected, 1e-4));
    CHECK_THAT(peak_reduction_headroom_db(0), WithinAbs(expected, 1e-4));
}

// Rejects a headroom that does not grow with K: the more bins a column
// covers, the higher an empty band's peak sits above its percentile.
TEST_CASE("the headroom grows with the bins a column covers", "[scale]")
{
    float previous = peak_reduction_headroom_db(1);
    for (const std::size_t k : {2U, 5U, 16U, 100U, 1000U}) {
        const float now = peak_reduction_headroom_db(k);
        INFO(k);
        CHECK(now > previous);
        previous = now;
    }
    // Against the harmonic number computed the long way at K = 16.
    double harmonic = 0.0;
    for (int i = 1; i <= 16; ++i) {
        harmonic += 1.0 / i;
    }
    const double expected = 10.0 * std::log10(harmonic / -std::log(0.95));
    CHECK_THAT(peak_reduction_headroom_db(16), WithinAbs(expected, 0.01));
}

// Rejects correcting the ceiling. A column holding a real signal draws that
// signal's own bin, which a maximum over K does not inflate.
TEST_CASE("the correction raises the floor and leaves the ceiling", "[scale]")
{
    const auto ends = map_ends(-100.0F, -40.0F, 15.0F);
    CHECK(ends.floor_db == -85.0F);
    CHECK(ends.ceiling_db == -40.0F);
}

TEST_CASE("the map is held a minimum span wide by moving the ceiling", "[scale]")
{
    const auto ends = map_ends(-100.0F, -90.0F, 5.0F);
    CHECK(ends.floor_db == -95.0F);
    CHECK(ends.ceiling_db == -95.0F + kSpectrumMinimumSpanDb);
}

// Rejects the CLI's first version, which drew a pinned ceiling 21.4 dB above
// where it was asked for. When something has to give it is the automatic end.
TEST_CASE("a pinned end is drawn where it was pinned", "[scale]")
{
    // Pinned ceiling, automatic floor, too narrow: the floor gives.
    const auto ceiling = map_ends(-100.0F, -90.0F, 5.0F, false, true);
    CHECK(ceiling.ceiling_db == -90.0F);
    CHECK(ceiling.floor_db == -90.0F - kSpectrumMinimumSpanDb);

    // Pinned floor takes no correction.
    const auto floor = map_ends(-100.0F, -40.0F, 15.0F, true, false);
    CHECK(floor.floor_db == -100.0F);

    // Both pinned and narrow: nothing gives.
    const auto both = map_ends(-60.0F, -55.0F, 15.0F, true, true);
    CHECK(both.floor_db == -60.0F);
    CHECK(both.ceiling_db == -55.0F);
}

// Rejects pins that are read and then ignored: the frame's own ends must not
// leak through where a pin stands.
TEST_CASE("resolve_ends draws against the pins in place of the frame", "[scale]")
{
    ScalePins pins;
    pins = set_floor_pin(pins, -110.0F);
    pins = set_ceiling_pin(pins, -30.0F);
    const auto ends = resolve_ends(-95.0F, -60.0F, 12.0F, pins);
    CHECK(ends.floor_db == -110.0F);
    CHECK(ends.ceiling_db == -30.0F);

    // Unpinned, the frame and its correction are back.
    const auto automatic = resolve_ends(-95.0F, -60.0F, 12.0F, ScalePins{});
    CHECK(automatic.floor_db == -83.0F);
    CHECK(automatic.ceiling_db == -60.0F);
}

// Rejects a pin pair that crosses. Two pins obeyed as written can make a map
// of no width, which divides by zero, or a negative one, which draws the
// waterfall upside down.
TEST_CASE("two pins never cross, and the one being moved gives", "[scale]")
{
    ScalePins pins = set_ceiling_pin(ScalePins{}, -60.0F);
    pins = set_floor_pin(pins, -50.0F);
    CHECK(pins.ceiling_db == -60.0F);
    CHECK(pins.floor_db == -60.0F - kMinPinnedSpanDb);

    pins = set_ceiling_pin(pins, -100.0F);
    CHECK(pins.ceiling_db == pins.floor_db + kMinPinnedSpanDb);

    // An unpinned other end constrains nothing.
    const ScalePins alone = set_floor_pin(ScalePins{}, -10.0F);
    CHECK(alone.floor_db == -10.0F);
}

TEST_CASE("a pin lands where the plate says the map is", "[scale]")
{
    CHECK(pin_level(-81.34F) == -81.3F);
    CHECK(pin_level(-93.25F) == -93.3F);
    CHECK(pin_level(-93.24F) == -93.2F);
}

// Rejects wrapping. A frame above its own ceiling saturates white rather
// than folding back to black, which on a waterfall would draw the strongest
// signal as the weakest.
TEST_CASE("the colour map clamps at its ends", "[scale]")
{
    const auto black = colour_at(0.0F);
    const auto below = colour_at(-3.0F);
    CHECK(below.r == black.r);
    CHECK(below.g == black.g);
    CHECK(below.b == black.b);

    const auto white = colour_at(1.0F);
    const auto above = colour_at(7.0F);
    CHECK(above.r == white.r);
    CHECK(above.g == white.g);
    CHECK(above.b == white.b);
}

// Whether the map rises in lightness is ui/tests/test_colour_map.cpp, beside
// the map, which is render/colour_map.h since 2026-09-22.

TEST_CASE("the packed colour is the same colour", "[scale]")
{
    for (const float level : {0.0F, 0.3F, 0.72F, 1.0F}) {
        const auto rgb = colour_at(level);
        const std::uint32_t packed = colour_argb_at(level);
        CHECK((packed >> 24U) == 0xFFU);
        CHECK(((packed >> 16U) & 0xFFU) == rgb.r);
        CHECK(((packed >> 8U) & 0xFFU) == rgb.g);
        CHECK((packed & 0xFFU) == rgb.b);
    }
}
