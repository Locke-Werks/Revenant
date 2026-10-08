// render/waterfall_scale.h: the span waterfall's levels, the history's
// percentiles, and the ends that follow them.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "render/waterfall_scale.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::CodeColourer;
using revenant::ui::decode_level;
using revenant::ui::encode_level;
using revenant::ui::HistoryLevels;
using revenant::ui::kDisplayMinimumSpanDb;
using revenant::ui::kHistoryContractSeconds;
using revenant::ui::kHistoryExpandSeconds;
using revenant::ui::kHistoryNoisePermille;
using revenant::ui::kHistoryStrongPermille;
using revenant::ui::kNoiseFraction;
using revenant::ui::kNoLevel;
using revenant::ui::LevelHistogram;
using revenant::ui::MapEnds;
using revenant::ui::ScalePins;
using revenant::ui::set_ceiling_pin;
using revenant::ui::set_floor_pin;

namespace {

// Steps the levels for `seconds` in frames of `frame` seconds.
void run(HistoryLevels& levels, float noise, float strong, double seconds, double frame = 0.01)
{
    for (double t = 0.0; t < seconds; t += frame) {
        levels.update(noise, strong, frame);
    }
}

}  // namespace

// Rejects 8-bit codes, which over a tuner's range are 0.6 dB a step and band
// a smooth fade on a 40 dB map. A level comes back within half a sixty-fourth.
TEST_CASE("a level survives its code to a sixty-fourth of a decibel", "[waterfall-scale]")
{
    for (const float db : {-200.0F, -137.31F, -92.5F, -20.0F, 0.0F, 12.75F}) {
        INFO(db);
        CHECK_THAT(decode_level(encode_level(db)), WithinAbs(db, 1.0 / 128.0 + 1e-4));
    }
    // Nothing a frame carries encodes as the empty pixel.
    CHECK(encode_level(-1000.0F) != kNoLevel);
    CHECK(encode_level(std::nanf("")) != kNoLevel);
}

// Rejects counting pixels never written. A ring a tenth full would otherwise
// measure its noise on the empty rows and put the floor at the bottom of the
// code range.
TEST_CASE("the histogram counts written levels only", "[waterfall-scale]")
{
    LevelHistogram histogram;
    std::vector<std::uint16_t> row(100, kNoLevel);
    for (int i = 0; i < 10; ++i) {
        row[static_cast<std::size_t>(i)] = encode_level(-90.0F);
    }
    histogram.add_row(row.data(), static_cast<int>(row.size()));
    CHECK(histogram.total() == 10);
    CHECK_THAT(histogram.percentile(kHistoryNoisePermille), WithinAbs(-90.0, 0.5));

    // A row overwritten takes its own levels out with it.
    histogram.remove_row(row.data(), static_cast<int>(row.size()));
    CHECK(histogram.total() == 0);
}

// Rejects a median for the noise, which on a band more than half stations
// is a station, and a maximum for the strong level, which is one spur.
TEST_CASE("the history's noise and strong levels are its quarter and its top thousandth",
          "[waterfall-scale]")
{
    LevelHistogram histogram;
    // 60% stations at -40, 40% noise at -100: the median is a station.
    for (int i = 0; i < 4000; ++i) {
        histogram.add(encode_level(-100.0F));
    }
    for (int i = 0; i < 6000; ++i) {
        histogram.add(encode_level(-40.0F));
    }
    CHECK_THAT(histogram.percentile(kHistoryNoisePermille), WithinAbs(-100.0, 0.5));

    // Five pixels in ten thousand at +10: under one in a thousand, so they
    // do not set the strong level.
    for (int i = 0; i < 5; ++i) {
        histogram.add(encode_level(10.0F));
    }
    CHECK_THAT(histogram.percentile(kHistoryStrongPermille), WithinAbs(-40.0, 0.5));
}

// Rejects ends that follow each row. The first estimate is taken as it
// stands, and after that a change takes time.
TEST_CASE("the ends start where the history is and then ease", "[waterfall-scale]")
{
    HistoryLevels levels;
    CHECK_FALSE(levels.valid());
    levels.update(-100.0F, -60.0F, 0.0);
    CHECK(levels.valid());
    CHECK(levels.noise_db() == -100.0F);
    CHECK(levels.strong_db() == -60.0F);

    // One 10 ms frame of a strong signal 40 dB up moves the strong level a
    // fraction of the way, not all of it.
    levels.update(-100.0F, -20.0F, 0.01);
    CHECK(levels.strong_db() > -60.0F);
    CHECK(levels.strong_db() < -59.0F);
}

// Rejects a symmetric time constant. A signal arriving widens the map over
// a few seconds; the same signal leaving narrows it over thirty.
TEST_CASE("the ends expand in seconds and contract over thirty", "[waterfall-scale]")
{
    HistoryLevels levels;
    levels.update(-100.0F, -60.0F, 0.0);

    // Expanding: after one expand time constant the strong level is 63% of
    // the way to its target, and after five it is there.
    run(levels, -100.0F, -20.0F, kHistoryExpandSeconds);
    CHECK_THAT(levels.strong_db(), WithinAbs(-60.0 + 40.0 * (1.0 - std::exp(-1.0)), 0.3));
    run(levels, -100.0F, -20.0F, 4.0 * kHistoryExpandSeconds);
    CHECK_THAT(levels.strong_db(), WithinAbs(-20.0, 0.3));

    // Contracting: the signal leaves. After the same few seconds the strong
    // level has barely moved; after one contract time constant it is 63% of
    // the way down.
    run(levels, -100.0F, -60.0F, kHistoryExpandSeconds);
    CHECK(levels.strong_db() > -26.0F);
    run(levels, -100.0F, -60.0F, kHistoryContractSeconds - kHistoryExpandSeconds);
    CHECK_THAT(levels.strong_db(), WithinAbs(-20.0 - 40.0 * (1.0 - std::exp(-1.0)), 0.5));

    // The floor is the other way round: noise falling is expanding.
    HistoryLevels floor;
    floor.update(-90.0F, -60.0F, 0.0);
    run(floor, -110.0F, -60.0F, 5.0 * kHistoryExpandSeconds);
    CHECK_THAT(floor.noise_db(), WithinAbs(-110.0, 0.3));
    run(floor, -90.0F, -60.0F, kHistoryExpandSeconds);
    CHECK(floor.noise_db() < -107.0F);
}

// Rejects a waterfall whose noise is not dark: the history's noise goes an
// eighth up the map, by the spectrum's rule.
TEST_CASE("the waterfall's noise sits low and dark", "[waterfall-scale]")
{
    HistoryLevels levels;
    levels.update(-95.0F, -93.0F, 0.0);
    const MapEnds ends = levels.ends(ScalePins{});
    CHECK_THAT(ends.span_db(), WithinAbs(kDisplayMinimumSpanDb, 1e-4));
    CHECK_THAT((-95.0 - ends.floor_db) / ends.span_db(), WithinAbs(kNoiseFraction, 1e-4));

    // And it is drawn in the dark end of the map: the noise's colour is
    // nearer the background than the middle of the map is.
    const CodeColourer colour(ends);
    const std::uint32_t noise = colour(encode_level(-95.0F));
    const std::uint32_t middle = colour(encode_level(ends.floor_db + ends.span_db() / 2.0F));
    const auto luma = [](std::uint32_t c) {
        return (c & 0xFFU) + ((c >> 8U) & 0xFFU) + ((c >> 16U) & 0xFFU);
    };
    CHECK(luma(noise) < luma(middle));
}

// Rejects pins the history ignores, and pins the easing moves.
TEST_CASE("pins override the history's ends", "[waterfall-scale]")
{
    HistoryLevels levels;
    levels.update(-100.0F, -60.0F, 0.0);
    run(levels, -80.0F, -10.0F, 1.0);

    ScalePins pins = set_floor_pin(ScalePins{}, -130.0F);
    pins = set_ceiling_pin(pins, -50.0F);
    const MapEnds ends = levels.ends(pins);
    CHECK(ends.floor_db == -130.0F);
    CHECK(ends.ceiling_db == -50.0F);

    const MapEnds floor_only = levels.ends(set_floor_pin(ScalePins{}, -125.0F));
    CHECK(floor_only.floor_db == -125.0F);
}

// Rejects recolouring that loses the empty pixel: a pixel never written draws
// the background whatever the ends, and a level is coloured by where it sits
// between them.
TEST_CASE("a code is coloured against the ends it is given", "[waterfall-scale]")
{
    const MapEnds low{-120.0F, -80.0F};
    const MapEnds high{-60.0F, -20.0F};
    const std::uint16_t code = encode_level(-80.0F);

    // At the top of one map and the bottom of the other.
    CHECK(CodeColourer(low)(code) == CodeColourer(MapEnds{-200.0F, -160.0F})(encode_level(0.0F)));
    CHECK(CodeColourer(high)(code) == CodeColourer(high)(kNoLevel));
    CHECK(CodeColourer(low)(kNoLevel) == CodeColourer(high)(kNoLevel));
}
