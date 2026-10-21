// render/waterfall_scale.h: the automatic pivot, the noise and signal split it
// is read from, and the easing it follows.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <optional>
#include <random>

#include "render/waterfall_scale.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::auto_pivot;
using revenant::ui::AutoPivotFollower;
using revenant::ui::encode_level;
using revenant::ui::kAutoPivotNoiseMarginDb;
using revenant::ui::kAutoPivotSeconds;
using revenant::ui::kMaxCurvePivot;
using revenant::ui::kMinCurvePivot;
using revenant::ui::LevelHistogram;
using revenant::ui::MapEnds;
using revenant::ui::noise_signal_split_db;

namespace {

// Log power of complex Gaussian noise about `mean_db`: the shape the history
// actually holds, skewed low, not a symmetric bell.
void add_noise(LevelHistogram& histogram, float mean_db, int count, std::mt19937& rng)
{
    std::exponential_distribution<float> power(1.0F);
    for (int i = 0; i < count; ++i) {
        const float db = mean_db + 10.0F * std::log10(power(rng) + 1e-9F);
        histogram.add(encode_level(db));
    }
}

void add_tone(LevelHistogram& histogram, float db, int count, std::mt19937& rng)
{
    std::normal_distribution<float> spread(0.0F, 1.5F);
    for (int i = 0; i < count; ++i) {
        histogram.add(encode_level(db + spread(rng)));
    }
}

}  // namespace

// Rejects a split that answers a level for an empty history, which would drag
// the pivot to the floor of the code range before the first row lands.
TEST_CASE("an empty history offers no split", "[auto_pivot]")
{
    const LevelHistogram empty;
    CHECK_FALSE(noise_signal_split_db(empty).has_value());
    CHECK_FALSE(auto_pivot(empty, MapEnds{-120.0F, -60.0F}).has_value());
}

// Rejects a percentile with a margin too small for a strong band and a split
// that lands inside either mode: the cut must fall in the gap.
TEST_CASE("a bimodal history splits between noise and signal", "[auto_pivot]")
{
    std::mt19937 rng(7);
    LevelHistogram histogram;
    add_noise(histogram, -110.0F, 90000, rng);
    add_tone(histogram, -70.0F, 10000, rng);

    const std::optional<float> split = noise_signal_split_db(histogram);
    REQUIRE(split.has_value());
    CHECK(*split > -103.0F);
    CHECK(*split < -76.0F);

    const MapEnds ends{-120.0F, -60.0F};
    const std::optional<float> pivot = auto_pivot(histogram, ends);
    REQUIRE(pivot.has_value());
    CHECK_THAT(*pivot, WithinAbs((*split - ends.floor_db) / ends.span_db(), 1e-5));
}

// Rejects a split that only works for a heavy signal class: a waterfall where
// one pixel in fifty is a carrier must still cut above the noise.
TEST_CASE("a sparse signal still splits above the noise", "[auto_pivot]")
{
    std::mt19937 rng(11);
    LevelHistogram histogram;
    add_noise(histogram, -110.0F, 98000, rng);
    add_tone(histogram, -60.0F, 2000, rng);

    const std::optional<float> split = noise_signal_split_db(histogram);
    REQUIRE(split.has_value());
    CHECK(*split > -103.0F);
    CHECK(*split < -66.0F);
}

// Rejects plain Otsu, which halves pure noise and would set the pivot in the
// middle of the speckle.
TEST_CASE("a unimodal history falls back to the noise median plus a margin", "[auto_pivot]")
{
    std::mt19937 rng(3);
    LevelHistogram histogram;
    add_noise(histogram, -110.0F, 100000, rng);

    const std::optional<float> split = noise_signal_split_db(histogram);
    REQUIRE(split.has_value());
    CHECK_THAT(*split, WithinAbs(histogram.percentile(500) + kAutoPivotNoiseMarginDb, 1e-4));
}

// Rejects a pivot handed back outside the curve's own range when the split
// sits off either end of the map.
TEST_CASE("the pivot is clamped to the curve's range", "[auto_pivot]")
{
    std::mt19937 rng(5);
    LevelHistogram histogram;
    add_noise(histogram, -110.0F, 10000, rng);

    CHECK(*auto_pivot(histogram, MapEnds{-60.0F, -20.0F}) == kMinCurvePivot);
    CHECK(*auto_pivot(histogram, MapEnds{-200.0F, -150.0F}) == kMaxCurvePivot);
}

// Rejects easing in from the default on the first offer, which would sweep the
// picture for seconds after the switch is turned on.
TEST_CASE("the follower takes its first offer as it is", "[auto_pivot]")
{
    AutoPivotFollower follower;
    CHECK_FALSE(follower.started());
    CHECK_THAT(follower.step(0.6F, 0.0), WithinAbs(0.6, 1e-6));
    CHECK(follower.started());
}

// Rejects a per-frame blend factor, which would ease faster at a higher frame
// rate: the same two seconds must close the same share of the gap either way.
TEST_CASE("the follower eases with a two second time constant", "[auto_pivot]")
{
    const double expected = 0.8 + (0.2 - 0.8) * std::exp(-1.0);
    for (const double frame : {1.0 / 30.0, 1.0 / 144.0}) {
        AutoPivotFollower follower;
        follower.step(0.2F, 0.0);
        for (double t = 0.0; t < kAutoPivotSeconds - frame / 2.0; t += frame) {
            follower.step(0.8F, frame);
        }
        CHECK_THAT(follower.value(), WithinAbs(expected, 2e-3));
    }
}

// Rejects a follower that keeps its old value across a reset, so turning the
// switch off and on again restarts from the next offer.
TEST_CASE("a reset follower snaps to the next offer", "[auto_pivot]")
{
    AutoPivotFollower follower;
    follower.step(0.2F, 0.0);
    follower.reset();
    CHECK_THAT(follower.step(0.7F, 0.016), WithinAbs(0.7, 1e-6));
    CHECK_THAT(follower.step(5.0F, 0.0), WithinAbs(0.7, 1e-6));
    CHECK(follower.step(5.0F, 100.0) <= kMaxCurvePivot);
}
