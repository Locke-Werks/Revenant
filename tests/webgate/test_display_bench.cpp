// Timing for PhoneDisplay::take, the per-phone, per-row display work on the
// gateway strand. Hidden: wall-clock numbers are a measurement, not a pass or
// fail, and would flake on a loaded CI runner. Run with
//   test_webgate.exe "[bench]" -s
// and read the per-row microseconds from the output.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "tools/webgate/display_sync.h"

namespace {

using revenant::webgate::DisplayFrame;
using revenant::webgate::DisplayRow;
using revenant::webgate::FrameLevels;
using revenant::webgate::PhoneDisplay;

constexpr std::size_t kBins = 65'536;
// An iPhone 15 Pro's width in device pixels.
constexpr std::size_t kWidthPx = 1179;
constexpr int kWarmRows = 200;
constexpr int kTimedRows = 2000;

[[nodiscard]] std::vector<float> noisy_frame(std::uint32_t seed)
{
    std::vector<float> out(kBins);
    std::uint32_t x = seed;
    for (float& v : out) {
        x = x * 1664525U + 1013904223U;
        v = -95.0F + static_cast<float>(x >> 24) / 25.0F;
    }
    out[kBins / 3] = -40.0F;
    return out;
}

// The largest bin of each column, as reduce_columns gives them.
[[nodiscard]] std::vector<float> columns_of(std::span<const float> shown)
{
    std::vector<float> out(kWidthPx);
    for (std::size_t c = 0; c < kWidthPx; ++c) {
        const std::size_t from = c * shown.size() / kWidthPx;
        const std::size_t to = std::max(from + 1, (c + 1) * shown.size() / kWidthPx);
        out[c] = *std::max_element(shown.begin() + static_cast<std::ptrdiff_t>(from),
                                   shown.begin() + static_cast<std::ptrdiff_t>(to));
    }
    return out;
}

// Microseconds per row through take(), frames cycling so the work is not one
// cached frame. shared hands take() levels measured beforehand, the way the
// gateway's cache does for every phone after the first; otherwise each row
// measures the whole frame itself.
[[nodiscard]] double time_take(std::size_t shown_bins, bool shared)
{
    std::vector<std::vector<float>> frames;
    for (std::uint32_t s = 1; s <= 8; ++s) {
        frames.push_back(noisy_frame(s));
    }
    const std::size_t first = (kBins - shown_bins) / 2;
    std::vector<std::vector<float>> columns;
    for (const auto& f : frames) {
        columns.push_back(columns_of(std::span<const float>(f).subspan(first, shown_bins)));
    }
    std::vector<FrameLevels> levels;
    for (const auto& f : frames) {
        levels.push_back(FrameLevels::of(f, shown_bins < kBins));
    }

    PhoneDisplay display;
    DisplayFrame facts;
    facts.count = kBins;
    facts.rate = 2'400'000;
    facts.first_bin_hz = 100e6;
    facts.floor_db = -98.0F;
    facts.ceiling_db = -60.0F;
    float sink = 0.0F;
    std::uint64_t start = 0;
    const auto row = [&](int n) {
        const auto& f = frames[static_cast<std::size_t>(n) % frames.size()];
        const auto& c = columns[static_cast<std::size_t>(n) % columns.size()];
        facts.start = start;
        start += kBins;
        const std::span<const float> whole(f);
        const std::span<const float> shown = whole.subspan(first, shown_bins);
        const std::size_t per_column = shown_bins / kWidthPx + 1;
        const DisplayRow& out =
            shared ? display.take(facts, levels[static_cast<std::size_t>(n) % levels.size()],
                                  shown, c, per_column)
                   : display.take(facts, whole, shown, c, per_column);
        sink += out.trace_columns.front();
    };
    for (int n = 0; n < kWarmRows; ++n) {
        row(n);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int n = 0; n < kTimedRows; ++n) {
        row(n);
    }
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(sink == sink);
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / kTimedRows;
}

// The share of a zoomed row that is the same for every phone, timed alone.
[[nodiscard]] double time_frame_levels()
{
    const std::vector<float> f = noisy_frame(3);
    float sink = 0.0F;
    const auto t0 = std::chrono::steady_clock::now();
    for (int n = 0; n < kTimedRows; ++n) {
        sink += FrameLevels::of(f, true).levels.floor_db;
    }
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(sink == sink);
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / kTimedRows;
}

}  // namespace

TEST_CASE("take() per row at a full 65,536-bin frame", "[.][bench][display]")
{
    std::printf("take() unzoomed: %.1f us/row\n", time_take(kBins, false));
    std::printf("take() zoomed 1/8, measuring the frame: %.1f us/row\n",
                time_take(kBins / 8, false));
    std::printf("take() zoomed 1/8, frame levels shared: %.1f us/row\n",
                time_take(kBins / 8, true));
    std::printf("frame levels, once per frame: %.1f us\n", time_frame_levels());
}
