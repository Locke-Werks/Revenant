// The audio strip's arithmetic: its top, hertz to pixels, bins into columns
// and the auto range.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

#include "render/audio_spectrum_map.h"

using namespace revenant::ui;
using Catch::Matchers::WithinAbs;

TEST_CASE("the strip shows up to the bandwidth or the Nyquist", "[audio_strip]")
{
    // 513 bins of 46.875 Hz is DC to 24 kHz.
    REQUIRE_THAT(audio_strip_top_hz(46.875, 513, 0.0), WithinAbs(24'000.0, 1e-9));
    REQUIRE_THAT(audio_strip_top_hz(46.875, 513, 3'000.0), WithinAbs(3'000.0, 1e-9));
    REQUIRE_THAT(audio_strip_top_hz(46.875, 513, 30'000.0), WithinAbs(24'000.0, 1e-9));
    REQUIRE_THAT(audio_strip_x(1'500.0, 3'000.0, 200.0), WithinAbs(100.0, 1e-9));
    REQUIRE_THAT(audio_strip_x(1'500.0, 0.0, 200.0), WithinAbs(0.0, 1e-9));
}

TEST_CASE("a narrow tone survives a strip narrower than the row", "[audio_strip]")
{
    std::vector<float> row(513, -100.0F);
    row[64] = -10.0F;  // 3 kHz at 46.875 Hz a bin
    std::vector<float> columns(100);
    audio_strip_columns(row, 46.875, 24'000.0, columns);
    // 240 Hz a column: 3 kHz is column 12.
    REQUIRE_THAT(columns[12], WithinAbs(-10.0, 1e-6));
    REQUIRE_THAT(columns[11], WithinAbs(-100.0, 1e-6));
    REQUIRE_THAT(columns[13], WithinAbs(-100.0, 1e-6));
}

TEST_CASE("a strip wider than the row takes the nearest bin", "[audio_strip]")
{
    const std::vector<float> row{-50.0F, -40.0F, -30.0F};
    std::vector<float> columns(8);
    // 100 Hz a bin, 0 to 200 Hz across eight columns of 25 Hz.
    audio_strip_columns(row, 100.0, 200.0, columns);
    REQUIRE_THAT(columns[0], WithinAbs(-50.0, 1e-6));
    REQUIRE_THAT(columns[3], WithinAbs(-40.0, 1e-6));
    REQUIRE_THAT(columns[7], WithinAbs(-30.0, 1e-6));
}

TEST_CASE("the strip's range follows the row with headroom and a minimum span",
          "[audio_strip]")
{
    std::vector<float> row(100, -90.0F);
    row[10] = -20.0F;
    std::vector<float> scratch;
    AudioStripScale scale;
    scale.update(row, scratch);
    REQUIRE_THAT(scale.floor_db, WithinAbs(-96.0, 1e-4));
    REQUIRE_THAT(scale.ceiling_db, WithinAbs(-14.0, 1e-4));
    REQUIRE_THAT(scale.level(-96.0F), WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(scale.level(0.0F), WithinAbs(1.0, 1e-6));

    // A flat row is held open, not stretched.
    const std::vector<float> flat(100, -80.0F);
    AudioStripScale held;
    held.update(flat, scratch);
    REQUIRE_THAT(held.ceiling_db - held.floor_db, WithinAbs(kAudioStripMinSpanDb, 1e-4));

    // Later frames ease rather than jump.
    scale.update(flat, scratch, 0.5F);
    REQUIRE_THAT(scale.floor_db, WithinAbs(-91.0, 1e-4));
}
