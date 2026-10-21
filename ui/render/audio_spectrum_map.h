// The audio spectrum strip's arithmetic: hertz to pixels, a frame's bins into
// one level per column, and the strip's own auto range.
//
// No Qt in here, so ui/tests can assert it without a window. AudioSpectrumItem
// owns the drawing.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace revenant::ui {

// The narrowest range the strip will show. A quiet channel's row is a few dB
// of ripple, and stretching that over the strip reads as a signal.
inline constexpr float kAudioStripMinSpanDb = 30.0F;

// Headroom over the loudest bin and under the floor, so the trace neither
// touches the top nor sits on the bottom edge.
inline constexpr float kAudioStripHeadroomDb = 6.0F;

// The strip's x axis: 0 Hz at the left, the shown top at the right. The top is
// the audio bandwidth when one is given, else the Nyquist of the frame.
[[nodiscard]] inline double audio_strip_top_hz(double bin_hz, std::size_t bins, double max_hz)
{
    const double nyquist = bins > 1 ? bin_hz * static_cast<double>(bins - 1) : 0.0;
    if (max_hz > 0.0 && (nyquist <= 0.0 || max_hz < nyquist)) {
        return max_hz;
    }
    return nyquist;
}

[[nodiscard]] inline double audio_strip_x(double hz, double top_hz, double width_px)
{
    if (top_hz <= 0.0) {
        return 0.0;
    }
    return hz / top_hz * width_px;
}

// One level per column, the loudest bin that falls in it, so a narrow tone
// survives a strip narrower than the row. A column no bin lands in takes the
// nearer bin's level, which is what a strip wider than the row needs.
inline void audio_strip_columns(std::span<const float> power_db, double bin_hz, double top_hz,
                                std::span<float> columns)
{
    if (columns.empty()) {
        return;
    }
    if (power_db.empty() || bin_hz <= 0.0 || top_hz <= 0.0) {
        std::ranges::fill(columns, -200.0F);
        return;
    }
    const auto count = columns.size();
    const double hz_per_column = top_hz / static_cast<double>(count);
    const std::size_t last = power_db.size() - 1;
    for (std::size_t c = 0; c < count; ++c) {
        const double from_hz = static_cast<double>(c) * hz_per_column;
        const double to_hz = from_hz + hz_per_column;
        // Bins whose centres fall in [from, to).
        const auto first = static_cast<std::size_t>(std::ceil(from_hz / bin_hz));
        const auto end =
            std::min(static_cast<std::size_t>(std::ceil(to_hz / bin_hz)), last + 1);
        if (first >= end) {
            const auto nearest = std::min(
                static_cast<std::size_t>(std::lround((from_hz + to_hz) * 0.5 / bin_hz)), last);
            columns[c] = power_db[nearest];
            continue;
        }
        float level = power_db[first];
        for (std::size_t b = first + 1; b < end; ++b) {
            level = std::max(level, power_db[b]);
        }
        columns[c] = level;
    }
}

// The strip's floor and ceiling, eased toward each frame's. The floor is the
// row's twentieth percentile, the ceiling its loudest bin, both with headroom
// and held at least kAudioStripMinSpanDb apart. scratch is resized as needed.
struct AudioStripScale {
    float floor_db = -120.0F;
    float ceiling_db = -20.0F;
    bool started = false;

    void update(std::span<const float> power_db, std::vector<float>& scratch, float ease = 0.2F)
    {
        if (power_db.empty()) {
            return;
        }
        scratch.assign(power_db.begin(), power_db.end());
        const auto low = scratch.begin() + static_cast<std::ptrdiff_t>(scratch.size() / 5);
        std::ranges::nth_element(scratch, low);
        const float floor = *low - kAudioStripHeadroomDb;
        const float peak = *std::ranges::max_element(scratch);
        const float ceiling = std::max(peak + kAudioStripHeadroomDb, floor + kAudioStripMinSpanDb);
        const float alpha = started ? std::clamp(ease, 0.0F, 1.0F) : 1.0F;
        floor_db += alpha * (floor - floor_db);
        ceiling_db += alpha * (ceiling - ceiling_db);
        started = true;
    }

    // 0 at the floor, 1 at the ceiling, clamped.
    [[nodiscard]] float level(float db) const
    {
        const float span = ceiling_db - floor_db;
        if (span <= 0.0F) {
            return 0.0F;
        }
        return std::clamp((db - floor_db) / span, 0.0F, 1.0F);
    }
};

}  // namespace revenant::ui
