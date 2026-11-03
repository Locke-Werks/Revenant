// The spectrum reducer: 65,536 dB bins down to a phone's pixel width.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "tools/webgate/spectrum_reducer.h"

namespace {

using revenant::webgate::every_nth_for;
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
    for (std::size_t i = 0; i < out.px.size(); ++i) CHECK(decode(out, i) >= kFloor);
    CHECK(std::fabs(decode(out, 0) - bins[0]) <= out.scale_db);
    CHECK(std::fabs(decode(out, 999) - bins[9]) <= out.scale_db);
}

TEST_CASE("view partly off the spectrum", "[webgate][spectrum]") {
    std::vector<float> bins(100, -40.0f);
    // The view runs -50..50 Hz, so its left half lies below bin 0.
    const View v{.center_hz = 0.0, .span_hz = 100.0, .width_px = 100};
    const auto out = reduce(bins, 0.0, 1.0, 0, v, kFloor, kCeil);
    REQUIRE(out.px.size() == 100);
    for (std::size_t i = 0; i < 49; ++i) CHECK(out.px[i] == 0);
    for (std::size_t i = 52; i < 100; ++i) {
        CHECK(out.px[i] > 0);
        CHECK(std::fabs(decode(out, i) - (-40.0f)) <= out.scale_db);
    }

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

TEST_CASE("every_nth_for", "[webgate][spectrum]") {
    CHECK(every_nth_for(305.0) == 15);
    CHECK(every_nth_for(10.0) == 1);
    CHECK(every_nth_for(0.0) == 1);
}
