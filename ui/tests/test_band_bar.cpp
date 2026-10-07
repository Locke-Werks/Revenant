// build_band_bar: the band-plan rows the span crosses, as lanes of pixel
// segments.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS. A bar that is wrong
// still looks like a bar: overlapping bands drawn on top of one another in
// one lane, a hard edge drawn at the screen's edge as if the band ended
// there, a "+N" that counts channels too narrow to have been shown.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

#include "models/band_bar.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::Band;
using revenant::ui::BandBar;
using revenant::ui::BandKind;
using revenant::ui::build_band_bar;
using revenant::ui::kRegionUSCA;

namespace {

Band make(std::int64_t low, std::int64_t high, BandKind kind = BandKind::kAllocation)
{
    Band band;
    band.name = "test";
    band.low_hz = low;
    band.high_hz = high;
    band.centre_hz = (low + high) / 2;
    band.regions = kRegionUSCA;
    band.kind = kind;
    return band;
}

}  // namespace

// Rejects: every row in lane 0, so a sub-band paints over its allocation.
TEST_CASE("overlapping rows stack into lanes, widest first in lane 0", "[band_bar]")
{
    const Band alloc = make(7'000'000, 7'300'000);
    const Band sub = make(7'000'000, 7'125'000, BandKind::kSubBand);
    const Band other = make(7'150'000, 7'250'000, BandKind::kSubBand);
    const std::vector<const Band*> rows{&alloc, &sub, &other};

    const BandBar bar = build_band_bar(rows, 7'000'000.0, 7'300'000.0, 300.0);
    REQUIRE(bar.segments.size() == 3);
    CHECK(bar.segments[0].lane == 0);
    CHECK(bar.segments[1].lane == 1);

    // Rejects: one segment per lane. The two sub-bands do not overlap, so
    // the second shares lane 1 with the first.
    CHECK(bar.segments[2].lane == 1);
    CHECK(bar.lanes_used == 2);
    CHECK(bar.overflow.empty());

    // Rejects: a mapping of its own. x is ruler_x.
    CHECK_THAT(bar.segments[1].x1, WithinAbs(125.0, 1e-9));
    CHECK_THAT(bar.segments[2].x0, WithinAbs(150.0, 1e-9));
}

// Rejects: adjacent channels treated as overlapping because they share an
// edge, which would put every other channel in a second lane.
TEST_CASE("rows that only touch share a lane", "[band_bar]")
{
    const Band a = make(100, 200);
    const Band b = make(200, 300);
    const BandBar bar = build_band_bar({&a, &b}, 100.0, 300.0, 200.0);
    REQUIRE(bar.segments.size() == 2);
    CHECK(bar.segments[0].lane == 0);
    CHECK(bar.segments[1].lane == 0);
}

// Rejects: a hard edge at the view's edge, and segments running off-screen.
TEST_CASE("segments clip at the span edges and say which side", "[band_bar]")
{
    const Band wide = make(0, 1000);
    const Band inside = make(400, 600);
    const BandBar bar = build_band_bar({&wide, &inside}, 200.0, 800.0, 600.0);
    REQUIRE(bar.segments.size() == 2);

    const auto& w = bar.segments[0];
    CHECK_THAT(w.x0, WithinAbs(0.0, 1e-9));
    CHECK_THAT(w.x1, WithinAbs(600.0, 1e-9));
    CHECK(w.clipped_left);
    CHECK(w.clipped_right);

    const auto& in = bar.segments[1];
    CHECK_FALSE(in.clipped_left);
    CHECK_FALSE(in.clipped_right);
    CHECK_THAT(in.x0, WithinAbs(200.0, 1e-9));
}

// Rejects: overflowing rows silently dropped, or one marker per row.
TEST_CASE("rows past the lane cap become one +N per cluster", "[band_bar]")
{
    const Band a = make(0, 1000);
    const Band b = make(0, 900);
    const Band c = make(0, 800);
    const Band d = make(100, 300);
    const Band e = make(200, 400);
    const Band far = make(600, 700);
    const BandBar bar = build_band_bar({&a, &b, &c, &d, &e, &far}, 0.0, 1000.0, 1000.0, 3);

    CHECK(bar.lanes_used == 3);
    CHECK(bar.segments.size() == 3);
    REQUIRE(bar.overflow.size() == 2);
    CHECK(bar.overflow[0].count == 2);
    CHECK_THAT(bar.overflow[0].x, WithinAbs(250.0, 1e-9));
    CHECK(bar.overflow[1].count == 1);
}

// Rejects: channels drawn as slivers at a wide span, and counted into the
// overflow when they are left out.
TEST_CASE("channels hide at wide spans and count toward nothing", "[band_bar]")
{
    const Band alloc = make(462'000'000, 468'000'000);
    std::vector<Band> channels;
    for (int i = 0; i < 10; ++i) {
        const std::int64_t low = 462'550'000 + i * 25'000;
        channels.push_back(make(low, low + 12'500, BandKind::kChannel));
    }
    std::vector<const Band*> rows{&alloc};
    for (const Band& ch : channels) {
        rows.push_back(&ch);
    }

    // 10 MHz across 1000 px: a 12.5 kHz channel is 1.25 px.
    const BandBar wide = build_band_bar(rows, 460'000'000.0, 470'000'000.0, 1000.0, 1);
    CHECK(wide.segments.size() == 1);
    CHECK(wide.overflow.empty());
    CHECK(wide.lanes_used == 1);

    // 500 kHz across 1000 px: 25 px each, so they are shown.
    const BandBar narrow = build_band_bar(rows, 462'500'000.0, 463'000'000.0, 1000.0);
    CHECK(narrow.segments.size() == 11);
    CHECK(narrow.segments[1].lane == 1);
}

// Rejects: segments under a pixel drawn as hover targets nobody can hit.
TEST_CASE("segments narrower than a pixel are dropped", "[band_bar]")
{
    const Band tiny = make(1000, 1001);
    const BandBar bar = build_band_bar({&tiny}, 0.0, 1'000'000.0, 1000.0);
    CHECK(bar.segments.empty());
    CHECK(bar.overflow.empty());
}

// Rejects: a division by a zero span, or a bar laid out on no width.
TEST_CASE("a zero or inverted span, or no width, lays out nothing", "[band_bar]")
{
    const Band a = make(100, 200);
    CHECK(build_band_bar({&a}, 150.0, 150.0, 100.0).segments.empty());
    CHECK(build_band_bar({&a}, 200.0, 100.0, 100.0).segments.empty());
    CHECK(build_band_bar({&a}, 100.0, 200.0, 0.0).segments.empty());
    CHECK(build_band_bar({&a}, 100.0, 200.0, 100.0, 0).segments.empty());
    CHECK(build_band_bar({}, 100.0, 200.0, 100.0).lanes_used == 0);
}
