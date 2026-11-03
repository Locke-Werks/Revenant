// Reduces an engine spectrum frame (up to 65,536 float dB bins) to one byte per
// pixel of the phone's waterfall row.
//
// Pure function: no engine types and no I/O, so it is tested on synthetic
// frames. The caller turns SpectrumGeometry into first_bin_hz and bin_hz.

#pragma once

#include <cstddef>
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
    // The waterfall row.
    std::vector<std::uint8_t> px;
    float offset_db = 0.0f;
    float scale_db = 1.0f;
    // The trace row in its own window, the same width as px, or empty when
    // the trace is the waterfall row (protocol.h).
    std::vector<std::uint8_t> trace_px;
    float trace_offset_db = 0.0f;
    float trace_scale_db = 1.0f;
    std::uint64_t source_start = 0;
    View view;
};

// A view's pixels in dB, before quantising. A pixel over no bin, off the
// spectrum's edge, is -infinity, as is one whose bins are all NaN. The pixels
// over the spectrum are [first_valid, end_valid), which is contiguous because
// the spectrum is; first_bin and bin_count are the bins the view covers, and
// bins_per_px how many bins one pixel takes, at least 1.
struct ReducedColumns {
    std::vector<float> db;
    std::uint32_t first_valid = 0;
    std::uint32_t end_valid = 0;
    std::size_t first_bin = 0;
    std::size_t bin_count = 0;
    std::size_t bins_per_px = 1;
    View view;
};

// reduce() without the quantising, by the same rules.
ReducedColumns reduce_columns(std::span<const float> power_db, double first_bin_hz, double bin_hz,
                              const View& view);

// Columns to bytes over [floor_db, ceiling_db], 256 steps, -infinity to 0.
// offset_db and scale_db are set to the decode: db = offset_db + px * scale_db.
std::vector<std::uint8_t> quantise(std::span<const float> db, float floor_db, float ceiling_db,
                                   float& offset_db, float& scale_db);

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

// The default ceiling on rows a second to a phone. The phone draws at its
// display's refresh and scrolls between rows, so every row the engine makes
// is wanted up to the rate a 60 Hz screen can show one per refresh; past
// that, rows would only cost the gateway and the Wi-Fi.
inline constexpr double kDefaultMaxPhoneFps = 60.0;

// The smallest every_nth that keeps a block_rate_hz stream at or under
// max_fps rows a second: 1, every row, whenever the source is already under
// it. A max_fps that is not positive and finite is no cap.
std::uint32_t every_nth_at_most(double block_rate_hz, double max_fps = kDefaultMaxPhoneFps);

}  // namespace revenant::webgate
