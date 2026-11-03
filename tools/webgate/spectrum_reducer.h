// Reduces an engine spectrum frame (up to 65,536 float dB bins) to one byte per
// pixel of the phone's waterfall row.
//
// Pure function: no engine types and no I/O, so it is tested on synthetic
// frames. The caller turns SpectrumGeometry into first_bin_hz and bin_hz.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace revenant::webgate {

// The part of the spectrum the phone is looking at.
struct View {
    double center_hz = 0.0;
    double span_hz = 0.0;
    std::uint32_t width_px = 0;
};

struct ReducedFrame {
    std::vector<std::uint8_t> px;
    float offset_db = 0.0f;
    float scale_db = 1.0f;
    std::uint64_t source_start = 0;
    View view;
};

// Widest row reduce() will produce. View::width_px beyond this is capped, and
// the returned view reports the capped value.
inline constexpr std::uint32_t kMaxWidthPx = 8192;

// decode: db = offset_db + px * scale_db, so px 0 is the floor.
//
// A pixel covering several bins takes their maximum, never the mean: a
// single-bin carrier must survive any reduction or narrow signals vanish
// when the phone is zoomed out. A pixel narrower than one bin takes the bin
// under its centre. Pixels outside the spectrum are 0 (floor).
//
// first_bin_hz is the centre frequency of bin 0 and bin_hz the spacing.
// Values are clamped to [floor_db, ceiling_db] and quantised to 256 steps, so
// the round trip is within scale_db / 2 of the clamped source maximum.
ReducedFrame reduce(std::span<const float> power_db, double first_bin_hz, double bin_hz,
                    std::uint64_t source_start, const View& view, float floor_db,
                    float ceiling_db);

// How many source blocks to skip between frames so the phone gets about
// target_fps rows a second. Never less than 1.
std::uint32_t every_nth_for(double block_rate_hz, double target_fps = 20.0);

}  // namespace revenant::webgate
