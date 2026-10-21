// The receiver waterfall's colouring against the span waterfall's, and the
// time markers both waterfalls draw.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "render/passband_colour.h"
#include "render/time_markers.h"
#include "render/waterfall_scale.h"

using namespace revenant::ui;

TEST_CASE("passband rows colour exactly as span rows for the same ends and curve",
          "[passband_waterfall]")
{
    std::vector<std::uint16_t> codes;
    for (float db = -140.0F; db <= -20.0F; db += 0.37F) {
        codes.push_back(encode_level(db));
    }
    codes.push_back(kNoLevel);
    const int count = static_cast<int>(codes.size());

    const PassbandRowScale scale = passband_row_scale(-125.0F, -60.0F, -118.0F, -72.0F);
    for (const float contrast : {0.0F, 0.3F, 1.0F, 1.4F, 2.0F}) {
        for (const float pivot : {0.1F, 0.25F, 0.8F}) {
            std::vector<std::uint32_t> passband(codes.size());
            colour_passband_row(codes.data(), count, scale, contrast, pivot, passband.data());

            // The span waterfall's path: blend_ends to the contrast's first
            // unit, the S past it, through the same colourer.
            const MapEnds ends =
                blend_ends(scale.padded, scale.tight, contrast < 1.0F ? contrast : 1.0F);
            std::vector<std::uint32_t> span(codes.size());
            CodeColourer(ends, curve_for(contrast, pivot)).row(codes.data(), span.data(), count);

            CAPTURE(contrast, pivot);
            REQUIRE(passband == span);
        }
    }
}

TEST_CASE("passband ends are the engine's at contrast 0 and its percentiles at 1",
          "[passband_waterfall]")
{
    const PassbandRowScale scale = passband_row_scale(-125.0F, -60.0F, -118.0F, -72.0F);
    const MapEnds padded = map_ends(-125.0F, -60.0F, 0.0F);
    CHECK(passband_ends(scale, 0.0F).floor_db == padded.floor_db);
    CHECK(passband_ends(scale, 0.0F).ceiling_db == padded.ceiling_db);
    CHECK(passband_ends(scale, 1.0F).floor_db == -118.0F);
    CHECK(passband_ends(scale, 2.0F).ceiling_db == -72.0F);

    // A frame with no percentiles measured falls back to padded.
    const PassbandRowScale bare = passband_row_scale(-125.0F, -60.0F, 0.0F, 0.0F);
    CHECK(passband_ends(bare, 1.0F).floor_db == padded.floor_db);
}

TEST_CASE("time markers fall on each multiple of the interval", "[time_markers]")
{
    TimeMarkerRing ring;
    ring.assign(1000);
    std::vector<std::int64_t> marks;
    // Rows every 130 ms for a minute, starting off a round time.
    const std::int64_t start = 1'760'000'003'456;
    int row = 999;
    for (std::int64_t t = start; t < start + 60'000; t += 130) {
        if (const std::int64_t mark = ring.write(row, t, 10); mark != 0) {
            marks.push_back(mark);
        }
        row = (row + 999) % 1000;
    }
    REQUIRE(marks.size() == 6);
    for (std::size_t i = 0; i < marks.size(); ++i) {
        CHECK(marks[i] % 10'000 == 0);
        if (i > 0) {
            CHECK(marks[i] - marks[i - 1] == 10'000);
        }
    }
    CHECK(ring.visible((row + 1) % 1000).size() == 6);
}

TEST_CASE("time markers off at zero and never on the first row", "[time_markers]")
{
    CHECK(marker_between(0, 20'000, 10) == 0);
    CHECK(marker_between(9'990, 10'010, 0) == 0);
    CHECK(marker_between(9'990, 10'010, 10) == 10'000);
    CHECK(marker_between(10'000, 10'100, 10) == 0);
    CHECK(marker_between(10'100, 10'000, 10) == 0);
}
