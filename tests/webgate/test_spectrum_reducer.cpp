// The spectrum reducer: 65,536 dB bins down to a phone's pixel width.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "tools/webgate/spectrum_reducer.h"

namespace {

using revenant::webgate::every_nth_at_most;
using revenant::webgate::ReducedFrame;
using revenant::webgate::reduce;
using revenant::webgate::View;

constexpr float kFloor = -120.0f;
constexpr float kCeil = 0.0f;

float decode(const ReducedFrame& f, std::size_t i) {
    return f.offset_db + static_cast<float>(f.px[i]) * f.scale_db;
}

// Bin spacing 1 Hz, bin 0 at 0 Hz, so a view centred on (n - 1) / 2 with a
// span of n Hz covers exactly the n bins.
View whole(std::size_t n, std::uint32_t width) {
    return View{.center_hz = (static_cast<double>(n) - 1.0) / 2.0,
                .span_hz = static_cast<double>(n),
                .width_px = width};
}

}  // namespace

TEST_CASE("a single-bin carrier survives every width", "[webgate][spectrum]") {
    std::vector<float> bins(65536, -120.0f);
    bins[40001] = -20.0f;

    for (std::uint32_t width : {320u, 1179u, 2048u}) {
        const auto out = reduce(bins, 0.0, 1.0, 7, whole(bins.size(), width), kFloor, kCeil);
        REQUIRE(out.px.size() == width);
        float best = -1000.0f;
        for (std::size_t i = 0; i < out.px.size(); ++i) best = std::max(best, decode(out, i));
        CHECK(std::fabs(best - (-20.0f)) <= out.scale_db);
    }
}

TEST_CASE("zoom past resolution takes the bin under each pixel", "[webgate][spectrum]") {
    std::vector<float> bins(10);
    for (std::size_t i = 0; i < bins.size(); ++i) bins[i] = -100.0f + 8.0f * static_cast<float>(i);

    const auto out = reduce(bins, 0.0, 1.0, 0, whole(bins.size(), 1000), kFloor, kCeil);
    REQUIRE(out.px.size() == 1000);
    // 100 pixels to a bin: pixel i shows bin i / 100, and every bin is above
    // the floor, so no pixel may be 0.
    for (std::size_t i = 0; i < out.px.size(); ++i) {
        CHECK(out.px[i] > 0);
        CHECK(std::fabs(decode(out, i) - bins[i / 100]) <= out.scale_db);
    }
}

TEST_CASE("view partly off the spectrum", "[webgate][spectrum]") {
    std::vector<float> bins(100, -40.0f);
    // The view runs -50..50 Hz, so its left half lies below bin 0.
    const View v{.center_hz = 0.0, .span_hz = 100.0, .width_px = 100};
    const auto out = reduce(bins, 0.0, 1.0, 0, v, kFloor, kCeil);
    REQUIRE(out.px.size() == 100);
    // One pixel per bin (a wide pixel by the tie rule): pixel 48 covers
    // -1.5..-0.5 bins and overlaps nothing, pixel 49 overlaps bin 0.
    for (std::size_t i = 0; i < 49; ++i) CHECK(out.px[i] == 0);
    for (std::size_t i = 49; i < 100; ++i) {
        CHECK(out.px[i] > 0);
        CHECK(std::fabs(decode(out, i) - (-40.0f)) <= out.scale_db);
    }

    // Narrow pixels (0.25 bin): floor while the centre is below bin 0's lower
    // edge. Pixel i's centre is at 0.25 i - 49.375 bins, so 197 is the last
    // floor pixel and 198 the first valid one.
    const View narrow{.center_hz = 0.0, .span_hz = 100.0, .width_px = 400};
    const auto n = reduce(bins, 0.0, 1.0, 0, narrow, kFloor, kCeil);
    REQUIRE(n.px.size() == 400);
    for (std::size_t i = 0; i <= 197; ++i) CHECK(n.px[i] == 0);
    for (std::size_t i = 198; i < 400; ++i) CHECK(n.px[i] > 0);

    // Wide pixels (4 bins) take the max over whatever bins they overlap, even
    // when the pixel's centre is off the spectrum: pixel 12 spans -1.5..2.5
    // bins and sees bins 0..2, pixel 11 sees none.
    std::vector<float> ramp(100);
    for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = -100.0f + static_cast<float>(i);
    const View wide{.center_hz = 0.0, .span_hz = 100.0, .width_px = 25};
    const auto w = reduce(ramp, 0.0, 1.0, 0, wide, kFloor, kCeil);
    REQUIRE(w.px.size() == 25);
    for (std::size_t i = 0; i <= 11; ++i) CHECK(w.px[i] == 0);
    CHECK(std::fabs(decode(w, 12) - ramp[2]) <= w.scale_db);
    CHECK(std::fabs(decode(w, 13) - ramp[6]) <= w.scale_db);

    // A view entirely beyond the last bin is all floor.
    const View beyond{.center_hz = 1000.0, .span_hz = 50.0, .width_px = 64};
    const auto none = reduce(bins, 0.0, 1.0, 0, beyond, kFloor, kCeil);
    CHECK(std::all_of(none.px.begin(), none.px.end(), [](std::uint8_t p) { return p == 0; }));
}

TEST_CASE("uint8 round trip", "[webgate][spectrum]") {
    constexpr std::size_t n = 65536;
    constexpr std::uint32_t width = 1024;  // 64 bins a pixel, exactly
    std::vector<float> bins(n);
    std::uint32_t s = 12345;
    for (auto& b : bins) {
        s = s * 1664525u + 1013904223u;
        b = -130.0f + 140.0f * static_cast<float>(s >> 8) / 16777216.0f;  // spills past both limits
    }

    const auto out = reduce(bins, 0.0, 1.0, 0, whole(n, width), kFloor, kCeil);
    REQUIRE(out.px.size() == width);
    for (std::size_t i = 0; i < width; ++i) {
        float m = *std::max_element(bins.begin() + static_cast<std::ptrdiff_t>(i * 64),
                                    bins.begin() + static_cast<std::ptrdiff_t>((i + 1) * 64));
        m = std::clamp(m, kFloor, kCeil);
        CHECK(std::fabs(decode(out, i) - m) <= out.scale_db);
    }
}

TEST_CASE("reduce carries its inputs through", "[webgate][spectrum]") {
    std::vector<float> bins(16, -60.0f);
    const View v = whole(bins.size(), 8);
    const auto out = reduce(bins, 0.0, 1.0, 99, v, kFloor, kCeil);
    CHECK(out.source_start == 99);
    CHECK(out.view.width_px == 8);
    CHECK(out.offset_db == kFloor);
}

TEST_CASE("random views match a brute-force reduction", "[webgate][spectrum]") {
    constexpr std::size_t n = 1024;
    std::mt19937 rng(20261102);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    std::vector<float> bins(n);
    for (auto& b : bins) b = static_cast<float>(-130.0 + 140.0 * unit(rng));

    for (std::uint32_t width : {320u, 1000u, 1179u, 2048u, 4096u}) {
        for (int trial = 0; trial < 6; ++trial) {
            const double bin_hz = std::array{1.0, 2.5, 0.37}[static_cast<std::size_t>(trial) % 3];
            const double first = -5000.0 + 10000.0 * unit(rng);
            const double spectrum_hz = static_cast<double>(n) * bin_hz;
            // Spans from far narrower than a bin per pixel to several times the
            // spectrum, so both rules and both edges are exercised.
            const double span = spectrum_hz * std::pow(10.0, -2.0 + 3.0 * unit(rng));
            const double centre = first + spectrum_hz * (-0.5 + 2.0 * unit(rng));
            const View v{.center_hz = centre, .span_hz = span, .width_px = width};

            const auto out = reduce(bins, first, bin_hz, 0, v, kFloor, kCeil);
            REQUIRE(out.px.size() == width);

            const double px_hz = span / width;
            for (std::uint32_t i = 0; i < width; ++i) {
                const double lo_hz = centre - span / 2.0 + px_hz * i;
                const double hi_hz = lo_hz + px_hz;
                float peak = -std::numeric_limits<float>::infinity();
                if (px_hz < bin_hz * (1.0 - 1e-6)) {
                    const double c = std::floor(((lo_hz + hi_hz) / 2.0 - first) / bin_hz + 0.5);
                    if (c >= 0.0 && c < static_cast<double>(n)) peak = bins[static_cast<std::size_t>(c)];
                } else {
                    for (std::size_t j = 0; j < n; ++j) {
                        const double b_lo = first + (static_cast<double>(j) - 0.5) * bin_hz;
                        const double b_hi = b_lo + bin_hz;
                        if (b_hi > lo_hz && b_lo < hi_hz) peak = std::max(peak, bins[j]);
                    }
                }
                std::uint8_t want = 0;
                if (peak > -std::numeric_limits<float>::infinity()) {
                    const float c = std::clamp(peak, kFloor, kCeil);
                    want = static_cast<std::uint8_t>(std::round((c - kFloor) / out.scale_db));
                }
                // A pixel edge within rounding of a bin edge may go either way.
                if (out.px[i] != want) {
                    const double rel = std::fabs(std::remainder(lo_hz - first, bin_hz)) / bin_hz;
                    const double rel_hi = std::fabs(std::remainder(hi_hz - first, bin_hz)) / bin_hz;
                    INFO("width " << width << " trial " << trial << " pixel " << i << " got "
                                  << int(out.px[i]) << " want " << int(want) << " px_hz " << px_hz
                                  << " bin_hz " << bin_hz << " lo " << lo_hz << " first " << first);
                    CHECK((std::fabs(rel - 0.5) < 1e-6 || std::fabs(rel_hi - 0.5) < 1e-6));
                }
            }
        }
    }
}

TEST_CASE("one pixel per bin with a half-bin offset drops no bin", "[webgate][spectrum]") {
    std::vector<float> bins(64);
    for (std::size_t i = 0; i < bins.size(); ++i) bins[i] = -100.0f + static_cast<float>(i);
    // Pixel i covers bins i and i + 1 equally (edges at bin centres), so every
    // bin from 1 up must appear and the last pixel keeps bin 63.
    const View v{.center_hz = 32.0, .span_hz = 64.0, .width_px = 64};
    const auto out = reduce(bins, 0.0, 1.0, 0, v, kFloor, kCeil);
    REQUIRE(out.px.size() == 64);
    for (std::size_t i = 0; i < 64; ++i) {
        CHECK(std::fabs(decode(out, i) - bins[std::min<std::size_t>(i + 1, 63)]) <= out.scale_db);
    }

    // Aligned one-to-one is exact: pixel i is bin i.
    const auto same = reduce(bins, 0.0, 1.0, 0, whole(64, 64), kFloor, kCeil);
    for (std::size_t i = 0; i < 64; ++i) CHECK(std::fabs(decode(same, i) - bins[i]) <= same.scale_db);
}

TEST_CASE("RF-scale absolute frequencies still line up", "[webgate][spectrum]") {
    // 2.4 GHz centre with 1 Hz bins: the view edge is a small difference of two
    // large numbers and must not lose the bin to rounding.
    std::vector<float> bins(256);
    for (std::size_t i = 0; i < bins.size(); ++i) bins[i] = -100.0f + static_cast<float>(i) / 4.0f;
    const double first = 2.4e9 - 128.0;
    const View v{.center_hz = 2.4e9 - 0.5, .span_hz = 256.0, .width_px = 256};
    const auto out = reduce(bins, first, 1.0, 0, v, kFloor, kCeil);
    for (std::size_t i = 0; i < 256; ++i) CHECK(std::fabs(decode(out, i) - bins[i]) <= out.scale_db);
}

TEST_CASE("bad views and ranges give a floor row, not undefined behaviour", "[webgate][spectrum]") {
    std::vector<float> bins(64, -40.0f);
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto all_floor = [](const ReducedFrame& f) {
        return std::all_of(f.px.begin(), f.px.end(), [](std::uint8_t p) { return p == 0; });
    };

    CHECK(reduce(bins, 0.0, 1.0, 0, View{31.5, 64.0, 0}, kFloor, kCeil).px.empty());

    for (double span : {0.0, -64.0, nan, inf, -inf}) {
        const auto f = reduce(bins, 0.0, 1.0, 0, View{31.5, span, 32}, kFloor, kCeil);
        CHECK(f.px.size() == 32);
        CHECK(all_floor(f));
    }
    for (double c : {nan, inf, -inf}) {
        const auto f = reduce(bins, 0.0, 1.0, 0, View{c, 64.0, 32}, kFloor, kCeil);
        CHECK(f.px.size() == 32);
        CHECK(all_floor(f));
    }
    for (double first : {nan, inf}) {
        CHECK(all_floor(reduce(bins, first, 1.0, 0, whole(64, 32), kFloor, kCeil)));
    }
    for (double bin_hz : {0.0, -1.0, nan, inf}) {
        CHECK(all_floor(reduce(bins, 0.0, bin_hz, 0, whole(64, 32), kFloor, kCeil)));
    }
    // Absurd but finite values: converted without overflowing.
    CHECK(reduce(bins, 1e300, 1e-300, 0, View{1e300, 1e-300, 32}, kFloor, kCeil).px.size() == 32);
    CHECK(reduce(bins, 0.0, 1.0, 0, View{1e300, 1e300, 32}, kFloor, kCeil).px.size() == 32);

    const float fnan = std::numeric_limits<float>::quiet_NaN();
    for (const auto& [lo, hi] : {std::pair{-120.0f, -120.0f}, std::pair{-120.0f, -130.0f},
                                 std::pair{fnan, 0.0f}, std::pair{-120.0f, fnan}}) {
        const auto f = reduce(bins, 0.0, 1.0, 0, whole(64, 64), lo, hi);
        CHECK(f.px.size() == 64);
        CHECK(all_floor(f));
        CHECK(std::isfinite(f.scale_db));
        CHECK(f.scale_db > 0.0f);
    }
}

TEST_CASE("NaN and infinite bins", "[webgate][spectrum]") {
    std::vector<float> bins(64, -40.0f);
    bins[10] = std::numeric_limits<float>::quiet_NaN();
    bins[20] = std::numeric_limits<float>::infinity();
    bins[30] = -std::numeric_limits<float>::infinity();

    // One bin to a pixel, aligned.
    const auto one = reduce(bins, 0.0, 1.0, 0, whole(64, 64), kFloor, kCeil);
    CHECK(one.px[10] == 0);
    CHECK(one.px[20] == 255);
    CHECK(one.px[30] == 0);
    CHECK(one.px[11] > 0);

    // Eight bins to a pixel: a NaN is skipped, the rest still count.
    const auto wide = reduce(bins, 0.0, 1.0, 0, whole(64, 8), kFloor, kCeil);
    CHECK(wide.px[1] > 0);   // bins 8..15, NaN at 10
    CHECK(wide.px[2] == 255);  // bins 16..23 hold +inf
}

TEST_CASE("width is capped", "[webgate][spectrum]") {
    std::vector<float> bins(64, -40.0f);
    const auto out = reduce(bins, 0.0, 1.0, 0, whole(64, 4'000'000'000u), kFloor, kCeil);
    CHECK(out.px.size() == revenant::webgate::kMaxWidthPx);
    CHECK(out.view.width_px == revenant::webgate::kMaxWidthPx);
    CHECK(revenant::webgate::kMaxWidthPx == 8192);

    const auto at_cap = reduce(bins, 0.0, 1.0, 0, whole(64, 8192), kFloor, kCeil);
    CHECK(at_cap.px.size() == 8192);
}

// The display rules read the columns in dB before they are bytes, and must not
// count the pixels a pan has moved off the spectrum as noise.
TEST_CASE("columns in dB mark the pixels over the spectrum", "[webgate][spectrum]") {
    std::vector<float> bins(64, -40.0f);
    bins[40] = -10.0f;
    // A view of 128 Hz centred on the spectrum's right edge, one bin a pixel.
    const View view{.center_hz = 63.5, .span_hz = 128.0, .width_px = 128};
    const auto cols = revenant::webgate::reduce_columns(bins, 0.0, 1.0, view);
    REQUIRE(cols.db.size() == 128);
    CHECK(cols.first_valid == 0);
    CHECK(cols.end_valid == 64);
    CHECK(cols.db[40] == -10.0f);
    CHECK(std::isinf(cols.db[100]));
    CHECK(cols.first_bin == 0);
    CHECK(cols.bin_count == 64);
    CHECK(cols.bins_per_px == 1);

    // The same bytes reduce() gives, so the split changed nothing.
    float offset = 0.0f;
    float scale = 0.0f;
    const auto px = revenant::webgate::quantise(cols.db, kFloor, kCeil, offset, scale);
    const auto direct = reduce(bins, 0.0, 1.0, 0, view, kFloor, kCeil);
    CHECK(px == direct.px);
    CHECK(offset == direct.offset_db);
    CHECK(scale == direct.scale_db);
}

// Rejects a rate rounded to the nearest (305 / 60 rounds to 5, 61 rows a
// second, over the cap) and a cap that thins a source already under it.
TEST_CASE("every_nth_at_most", "[webgate][spectrum]") {
    // 2.4 MS/s in 65,536-sample blocks: every row under the default cap.
    CHECK(every_nth_at_most(2'400'000.0 / 65'536.0) == 1);
    CHECK(every_nth_at_most(36.6, 60.0) == 1);
    // 20 MS/s: 305 rows a second, thinned to 50.8, never above 60.
    CHECK(every_nth_at_most(305.0, 60.0) == 6);
    CHECK(every_nth_at_most(305.0, 20.0) == 16);
    // Exactly on the cap is every row, not every second one.
    CHECK(every_nth_at_most(60.0, 60.0) == 1);
    CHECK(every_nth_at_most(120.0, 60.0) == 2);
    // No cap, or nonsense, is every row.
    CHECK(every_nth_at_most(305.0, 0.0) == 1);
    CHECK(every_nth_at_most(305.0, std::numeric_limits<double>::infinity()) == 1);
    CHECK(every_nth_at_most(0.0, 60.0) == 1);
    CHECK(every_nth_at_most(std::numeric_limits<double>::quiet_NaN(), 60.0) == 1);
}
