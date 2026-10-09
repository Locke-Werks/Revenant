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
#include <random>
#include <vector>

#include "render/spectrum_scale.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::colour_argb_at;
using revenant::ui::colour_at;
using revenant::ui::kDisplayMinimumSpanDb;
using revenant::ui::kMinPinnedSpanDb;
using revenant::ui::kNoiseFraction;
using revenant::ui::kSpectrumMinimumSpanDb;
using revenant::ui::map_ends;
using revenant::ui::peak_reduction_headroom_db;
using revenant::ui::pin_level;
using revenant::ui::place_ends;
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

// map_ends is the passband displays' rule since 2026-10-07 and is held here
// unchanged; the span displays' rule is place_ends, further down.
//
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

    // Unpinned, the frame and its correction are back: the drawn noise is
    // -95 + 12 = -83, the strong level -60, 23 dB over it, which is under the
    // minimum span, so the map is 40 dB with the noise 12% up it.
    const auto automatic = resolve_ends(-95.0F, -60.0F, 12.0F, ScalePins{});
    CHECK_THAT(automatic.floor_db, WithinAbs(-83.0 - 0.12 * 40.0, 1e-4));
    CHECK_THAT(automatic.ceiling_db, WithinAbs(-83.0 + 0.88 * 40.0, 1e-4));
}

// ---------------------------------------------------------------------------
// Where the noise sits: place_ends
// ---------------------------------------------------------------------------

// Where a level lands up the map, 0 at the floor and 1 at the ceiling.
namespace {
[[nodiscard]] double height_of(float level_db, const revenant::ui::MapEnds& ends)
{
    return (static_cast<double>(level_db) - ends.floor_db) / ends.span_db();
}
}  // namespace

// Rejects the rule the owner saw at 145 MHz on 2026-10-07: the noise ON the
// bottom edge and a twelve decibel map, which is a band of pure noise drawn as
// a wall from the bottom of the plot to the top. The engine's ceiling decays
// down into the noise on an empty band, so the strong level here is at or
// under the noise.
TEST_CASE("a band of pure noise sits in the bottom eighth of a 40 dB map", "[scale]")
{
    for (const float strong : {-90.0F, -88.0F, -95.0F}) {
        const auto ends = place_ends(-90.0F, strong, ScalePins{});
        INFO(strong);
        CHECK_THAT(ends.span_db(), WithinAbs(kDisplayMinimumSpanDb, 1e-4));
        const double at = height_of(-90.0F, ends);
        CHECK(at >= 0.10);
        CHECK(at <= 0.15);
    }
}

// Rejects a minimum span narrow enough for noise to fill it. At one bin per
// column the noise's 99.9th percentile is 10 log10(ln 1000) = 8.4 dB over its
// mean; it must stay in the lower half, where it reads as noise, and well off
// the top.
TEST_CASE("the noise's one-in-a-thousand spike stays in the lower half", "[scale]")
{
    const float noise = -100.0F;
    const auto ends = place_ends(noise, noise, ScalePins{});
    const auto spike = static_cast<float>(noise + 10.0 * std::log10(std::log(1000.0)));
    CHECK(height_of(spike, ends) < 0.4);
}

// Rejects a placement that moves the noise when a strong signal arrives. The
// map widens to take the signal at the top and the noise stays an eighth up.
TEST_CASE("a strong signal widens the map and the noise stays put", "[scale]")
{
    const auto ends = place_ends(-100.0F, -30.0F, ScalePins{});
    CHECK_THAT(ends.ceiling_db, WithinAbs(-30.0, 1e-3));
    CHECK_THAT(height_of(-100.0F, ends), WithinAbs(kNoiseFraction, 1e-4));
    CHECK(ends.span_db() > kDisplayMinimumSpanDb);
}

// Rejects a pin that the placement moves. A pinned end is drawn where it was
// pinned and the automatic end gives; under a pinned ceiling the noise still
// sits an eighth up.
TEST_CASE("place_ends draws a pinned end where it was pinned", "[scale]")
{
    const auto floor = place_ends(-90.0F, -40.0F, set_floor_pin(ScalePins{}, -120.0F));
    CHECK(floor.floor_db == -120.0F);
    CHECK(floor.ceiling_db == -40.0F);

    // A pinned floor too close to the strong level: the ceiling gives.
    const auto narrow = place_ends(-90.0F, -85.0F, set_floor_pin(ScalePins{}, -100.0F));
    CHECK(narrow.floor_db == -100.0F);
    CHECK(narrow.ceiling_db == -100.0F + kDisplayMinimumSpanDb);

    const auto ceiling = place_ends(-100.0F, -95.0F, set_ceiling_pin(ScalePins{}, -20.0F));
    CHECK(ceiling.ceiling_db == -20.0F);
    CHECK_THAT(height_of(-100.0F, ceiling), WithinAbs(kNoiseFraction, 1e-4));

    // A pinned ceiling just over the noise: the floor gives the minimum span.
    const auto low = place_ends(-100.0F, -95.0F, set_ceiling_pin(ScalePins{}, -95.0F));
    CHECK(low.ceiling_db == -95.0F);
    CHECK(low.floor_db == -95.0F - kDisplayMinimumSpanDb);

    ScalePins both = set_floor_pin(ScalePins{}, -70.0F);
    both = set_ceiling_pin(both, -65.0F);
    const auto pinned = place_ends(-100.0F, -20.0F, both);
    CHECK(pinned.floor_db == -70.0F);
    CHECK(pinned.ceiling_db == -65.0F);
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

// ---------------------------------------------------------------------------
// The held peak: the drawn trace never reaches the top edge
// ---------------------------------------------------------------------------

using revenant::ui::drawn_max;
using revenant::ui::hold_peak;
using revenant::ui::kPeakHoldMarginDb;
using revenant::ui::kPeakHoldMarginFraction;
using revenant::ui::kPeakHoldReleaseSeconds;
using revenant::ui::PeakHold;
using revenant::ui::resolve_ends_held;

// Rejects an attack with any lag. The 98.1 MHz report was a station drawn
// above the top; one frame of a slow rise is one frame drawn clipped.
TEST_CASE("the held peak rises to a new maximum in the same frame", "[scale][peak]")
{
    PeakHold held = hold_peak(PeakHold{}, -60.0F, 0.0);
    held = hold_peak(held, -31.5F, 0.01);
    CHECK(held.valid);
    CHECK(held.level_db == -31.5F);
}

// Rejects a release that snaps down, and one that never comes down: after one
// time constant it has covered 1 - 1/e of the way, after ten nearly all.
TEST_CASE("the held peak releases with a thirty second time constant", "[scale][peak]")
{
    PeakHold held = hold_peak(PeakHold{}, -30.0F, 0.0);
    held = hold_peak(held, -70.0F, 1.0 / 60.0);
    CHECK(held.level_db > -30.1F);

    PeakHold one = hold_peak(PeakHold{true, -30.0F}, -70.0F, kPeakHoldReleaseSeconds);
    CHECK_THAT(one.level_db, WithinAbs(-30.0 - 40.0 * (1.0 - std::exp(-1.0)), 0.01));

    // Many small steps land where one large step does, so the release does not
    // depend on the frame rate.
    PeakHold stepped{true, -30.0F};
    for (int i = 0; i < 30 * 60; ++i) {
        stepped = hold_peak(stepped, -70.0F, 1.0 / 60.0);
    }
    CHECK_THAT(stepped.level_db, WithinAbs(one.level_db, 0.05));
}

// Rejects carrying a hold across a retune or a looped recording: dt of zero
// is a new start and takes the frame as it is.
TEST_CASE("a new start resets the held peak", "[scale][peak]")
{
    const PeakHold held = hold_peak(PeakHold{true, -20.0F}, -70.0F, 0.0);
    CHECK(held.level_db == -70.0F);
}

TEST_CASE("the drawn maximum is the largest reduced column", "[scale][peak]")
{
    const std::vector<float> bins{-90.0F, -31.5F, -95.0F, -91.0F, -92.0F, -93.0F};
    std::vector<float> columns(3);
    reduce_peak(bins, columns);
    CHECK(drawn_max(columns) == -31.5F);
}

// Rejects the reported bug: a percentile ceiling under the drawn peak. And
// rejects a margin of nothing, the peak touching the edge.
TEST_CASE("the top of the map sits a visible margin above the held peak", "[scale][peak]")
{
    // The owner's numbers: ceiling -37.1, peak -31.5.
    const PeakHold held{true, -31.5F};
    const auto ends = resolve_ends_held(-100.0F, -37.1F, 5.0F, held, ScalePins{});
    const float gap = ends.ceiling_db - held.level_db;
    CHECK(gap >= kPeakHoldMarginDb - 1e-4F);
    CHECK(gap / ends.span_db() >= 0.05F);
    CHECK(gap / ends.span_db() <= 0.10F);

    // A strong station on a wide map keeps the fraction, not just the decibels.
    const PeakHold strong{true, 0.0F};
    const auto wide = resolve_ends_held(-120.0F, -40.0F, 5.0F, strong, ScalePins{});
    CHECK_THAT((wide.ceiling_db - strong.level_db) / wide.span_db(),
               WithinAbs(kPeakHoldMarginFraction, 1e-4));

    // On a quiet band the minimum span wins and the margin is larger still.
    const PeakHold quiet{true, -88.0F};
    const auto narrow = resolve_ends_held(-100.0F, -85.0F, 5.0F, quiet, ScalePins{});
    CHECK(narrow.span_db() == kDisplayMinimumSpanDb);
    CHECK(narrow.ceiling_db - quiet.level_db > kPeakHoldMarginDb);
}

// Rejects the hold lowering the engine's ceiling, which is still the floor of
// the strong level.
TEST_CASE("the engine ceiling still applies above a low held peak", "[scale][peak]")
{
    const auto ends = resolve_ends_held(-100.0F, -20.0F, 5.0F, PeakHold{true, -60.0F}, ScalePins{});
    CHECK(ends.ceiling_db == -20.0F);
}

// Rejects the hold overriding a pin.
TEST_CASE("a pinned ceiling beats the held peak", "[scale][peak]")
{
    const auto ends = resolve_ends_held(-100.0F, -37.1F, 5.0F, PeakHold{true, -31.5F},
                                        set_ceiling_pin(ScalePins{}, -50.0F));
    CHECK(ends.ceiling_db == -50.0F);
}

// Rejects the hold moving the noise: it stays kNoiseFraction up the map.
TEST_CASE("the held peak leaves the noise where place_ends puts it", "[scale][peak]")
{
    const float noise = -100.0F + 5.0F;
    const auto ends = resolve_ends_held(-100.0F, -37.1F, 5.0F, PeakHold{true, -10.0F}, ScalePins{});
    CHECK_THAT((noise - ends.floor_db) / ends.span_db(), WithinAbs(kNoiseFraction, 1e-4));
}

// THE RANGE FIT SLIDER. Fully qualified rather than added to the using list
// above, so these cases read as one block.

namespace fit_cases {

namespace rv = revenant::ui;

constexpr float kFloor = -110.0F;
constexpr float kHeadroom = 5.0F;
constexpr float kNoise = kFloor + kHeadroom;

// Rejects a slider whose zero is merely close to today: any change at 0 would
// move every existing operator's picture the moment the build landed.
TEST_CASE("range fit 0 is the held ends exactly", "[scale][fit]")
{
    const rv::PeakHold held{true, -60.0F};
    const auto before = rv::resolve_ends_held(kFloor, -70.0F, kHeadroom, held, rv::ScalePins{});
    const auto at0 = rv::resolve_ends_fit(kFloor, -70.0F, kHeadroom, held, rv::ScalePins{}, 0.0F);
    CHECK(at0.floor_db == before.floor_db);
    CHECK(at0.ceiling_db == before.ceiling_db);
}

// Rejects keeping the 12% offset or the peak margin at the tight end.
TEST_CASE("range fit 1 puts the noise on the bottom and the held peak on the top",
          "[scale][fit]")
{
    const rv::PeakHold held{true, -60.0F};
    const auto at1 = rv::resolve_ends_fit(kFloor, -70.0F, kHeadroom, held, rv::ScalePins{}, 1.0F);
    CHECK_THAT(at1.floor_db, WithinAbs(kNoise, 1e-4));
    CHECK_THAT(at1.ceiling_db, WithinAbs(-60.0F, 1e-4));
}

// Rejects easing in some other space, a log of the span or a curve on the
// slider: the owner asked for linear in between.
TEST_CASE("range fit one half is the midpoint of both ends", "[scale][fit]")
{
    const rv::PeakHold held{true, -60.0F};
    const rv::ScalePins none{};
    const auto at0 = rv::resolve_ends_fit(kFloor, -70.0F, kHeadroom, held, none, 0.0F);
    const auto at1 = rv::resolve_ends_fit(kFloor, -70.0F, kHeadroom, held, none, 1.0F);
    const auto half = rv::resolve_ends_fit(kFloor, -70.0F, kHeadroom, held, none, 0.5F);
    CHECK_THAT(half.floor_db, WithinAbs((at0.floor_db + at1.floor_db) / 2.0F, 1e-4));
    CHECK_THAT(half.ceiling_db, WithinAbs((at0.ceiling_db + at1.ceiling_db) / 2.0F, 1e-4));
}

// Rejects keeping the 40 dB minimum at the tight end: a signal 1 dB over the
// noise must fill the trace's height, not a fortieth of it.
TEST_CASE("at range fit 1 a 1 dB signal fills the height", "[scale][fit]")
{
    const rv::PeakHold held{true, kNoise + 1.0F};
    const auto at1 = rv::resolve_ends_fit(kFloor, kNoise + 1.0F, kHeadroom, held, rv::ScalePins{},
                                          1.0F);
    CHECK_THAT(at1.floor_db, WithinAbs(kNoise, 1e-4));
    CHECK_THAT(at1.ceiling_db, WithinAbs(kNoise + 1.0F, 1e-4));
}

// Rejects a zero span, which divides by zero in every pixel's position.
TEST_CASE("the tight end keeps the guard span when peak and noise meet", "[scale][fit]")
{
    const rv::PeakHold held{true, kNoise - 3.0F};
    const auto at1 = rv::resolve_ends_fit(kFloor, kNoise - 3.0F, kHeadroom, held, rv::ScalePins{},
                                          1.0F);
    CHECK_THAT(at1.span_db(), WithinAbs(rv::kSpectrumTightGuardDb, 1e-4));
    CHECK(at1.floor_db == kNoise);
}

// Rejects a fit that overrides a pin: a pin is there to compare captures.
TEST_CASE("pins override the range fit", "[scale][fit]")
{
    const rv::PeakHold held{true, -60.0F};
    rv::ScalePins pins = rv::set_floor_pin(rv::ScalePins{}, -130.0F);
    for (const float s : {0.0F, 0.5F, 1.0F}) {
        const auto ends = rv::resolve_ends_fit(kFloor, -70.0F, kHeadroom, held, pins, s);
        CHECK(ends.floor_db == -130.0F);
    }
    CHECK_THAT(rv::resolve_ends_fit(kFloor, -70.0F, kHeadroom, held, pins, 1.0F).ceiling_db,
               WithinAbs(-60.0F, 1e-4));

    pins = rv::set_ceiling_pin(pins, -20.0F);
    const auto both = rv::resolve_ends_fit(kFloor, -70.0F, kHeadroom, held, pins, 1.0F);
    CHECK(both.floor_db == -130.0F);
    CHECK(both.ceiling_db == -20.0F);

    const rv::ScalePins top = rv::set_ceiling_pin(rv::ScalePins{}, -20.0F);
    const auto at1 = rv::resolve_ends_fit(kFloor, -70.0F, kHeadroom, held, top, 1.0F);
    CHECK(at1.ceiling_db == -20.0F);
    CHECK_THAT(at1.floor_db, WithinAbs(kNoise, 1e-4));
}

}  // namespace fit_cases

// ---------------------------------------------------------------------------
// Trace smoothing: flat floor, instant attack, timed release
// ---------------------------------------------------------------------------

namespace smooth_cases {

using revenant::ui::smooth_trace;

constexpr double kDt = 1.0 / 30.0;
constexpr double kDecay = 0.25;

// Rejects an attack with any lag: a carrier keyed up is drawn in the frame it
// arrives, smoothing or not.
TEST_CASE("a signal rising out of the noise is drawn in the same frame", "[scale][smooth]")
{
    std::vector<float> state;
    std::vector<float> cols(64, -100.0F);
    smooth_trace(state, cols, 0.0, kDecay, false);
    std::vector<float> next(64, -100.0F);
    next[10] = -40.0F;
    smooth_trace(state, next, kDt, kDecay, false);
    CHECK(next[10] == -40.0F);
}

// Rejects a release that snaps down, and one at the wrong rate.
TEST_CASE("a signal that leaves fades with the decay time", "[scale][smooth]")
{
    std::vector<float> state;
    std::vector<float> cols(64, -100.0F);
    cols[10] = -40.0F;
    smooth_trace(state, cols, 0.0, kDecay, false);
    std::vector<float> next(64, -100.0F);
    smooth_trace(state, next, kDecay, kDecay, false);
    const double expected =
        10.0 * std::log10(std::pow(10.0, -4.0) + (std::pow(10.0, -10.0) - std::pow(10.0, -4.0)) *
                                                     (1.0 - std::exp(-1.0)));
    CHECK_THAT(next[10], WithinAbs(expected, 0.01));
    CHECK(next[10] > -100.0F);
}

// Rejects smoothing that does nothing to the floor: noise jitter shrinks.
TEST_CASE("smoothing flattens a noisy floor", "[scale][smooth]")
{
    std::mt19937 rng(7);
    std::normal_distribution<float> jitter(0.0F, 2.0F);
    std::vector<float> state;
    std::vector<double> raw;
    std::vector<double> smooth;
    for (int frame = 0; frame < 300; ++frame) {
        std::vector<float> cols(256);
        for (float& c : cols) {
            c = -100.0F + jitter(rng);
        }
        const std::vector<float> in = cols;
        smooth_trace(state, cols, frame == 0 ? 0.0 : kDt, kDecay, false);
        if (frame > 60) {
            raw.insert(raw.end(), in.begin(), in.end());
            smooth.insert(smooth.end(), cols.begin(), cols.end());
        }
    }
    // Ripple about the trace's own level: a constant offset is not ripple.
    const auto spread = [](const std::vector<double>& v) {
        double mean = 0.0;
        for (const double x : v) {
            mean += x;
        }
        mean /= static_cast<double>(v.size());
        double var = 0.0;
        for (const double x : v) {
            var += (x - mean) * (x - mean);
        }
        return var / static_cast<double>(v.size());
    };
    CHECK(spread(smooth) < 0.25 * spread(raw));
}

// Rejects a smoother that keeps running when switched off, and one that
// restarts on a replay of the same frame.
TEST_CASE("smoothing off passes frames through, a replay keeps the averages", "[scale][smooth]")
{
    std::vector<float> state(4, -50.0F);
    std::vector<float> cols{-90.0F, -90.0F, -90.0F, -90.0F};
    smooth_trace(state, cols, kDt, 0.0, false);
    CHECK(state.empty());
    CHECK(cols[0] == -90.0F);

    smooth_trace(state, cols, 0.0, kDecay, false);
    std::vector<float> lower{-100.0F, -100.0F, -100.0F, -100.0F};
    smooth_trace(state, lower, kDt, kDecay, false);
    const float held = lower[0];
    std::vector<float> again{-100.0F, -100.0F, -100.0F, -100.0F};
    smooth_trace(state, again, 0.0, kDecay, false, true);
    CHECK(again[0] == held);
}

}  // namespace smooth_cases
