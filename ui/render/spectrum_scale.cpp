#include "render/spectrum_scale.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace revenant::ui {
namespace {

// Stops for the colour map, darkest first. A dark blue through teal to a warm
// white, chosen for monotone luminance: a ramp that dips in brightness puts a
// false edge in the middle of an otherwise smooth noise floor, and on a
// waterfall that reads as a band boundary.
struct Stop {
    float position;
    Rgb colour;
};

constexpr std::array<Stop, 7> kStops{{
    {0.00F, {4, 6, 16}},
    {0.18F, {18, 30, 92}},
    {0.38F, {24, 86, 160}},
    {0.56F, {34, 160, 148}},
    {0.72F, {176, 196, 64}},
    {0.86F, {244, 158, 48}},
    {1.00F, {255, 246, 214}},
}};

[[nodiscard]] std::uint8_t mix(std::uint8_t low, std::uint8_t high, float t)
{
    const float value = static_cast<float>(low) +
                        (static_cast<float>(high) - static_cast<float>(low)) * t;
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 255.0F)));
}

}  // namespace

void reduce_peak(std::span<const float> bins, std::span<float> columns)
{
    const std::size_t bin_count = bins.size();
    const std::size_t column_count = columns.size();
    if (bin_count == 0 || column_count == 0) {
        return;
    }

    for (std::size_t c = 0; c < column_count; ++c) {
        // Integer arithmetic on the numerator so the runs tile the span
        // exactly: a float step accumulates and leaves the last column
        // covering one bin more or fewer than it should, which is a
        // frequency error at the right-hand edge that nothing else explains.
        const std::size_t begin = bin_count * c / column_count;
        std::size_t end = bin_count * (c + 1) / column_count;
        if (end <= begin) {
            end = begin + 1;
        }
        end = std::min(end, bin_count);

        float peak = bins[begin];
        for (std::size_t i = begin + 1; i < end; ++i) {
            peak = std::max(peak, bins[i]);
        }
        columns[c] = peak;
    }
}

float peak_reduction_headroom_db(std::size_t bins_per_column)
{
    constexpr double kEulerMascheroni = 0.577215664901532861;
    const double count = static_cast<double>(bins_per_column);
    const double harmonic =
        bins_per_column <= 1 ? 1.0 : std::log(count) + kEulerMascheroni + 0.5 / count;

    const double fraction = static_cast<double>(kSpectrumLowPermille) / 1000.0;
    const double percentile_of_mean = -std::log(1.0 - fraction);

    return static_cast<float>(10.0 * std::log10(harmonic / percentile_of_mean));
}

MapEnds map_ends(float frame_floor_db, float frame_ceiling_db, float headroom_db,
                 bool floor_pinned, bool ceiling_pinned)
{
    MapEnds ends;
    ends.floor_db = floor_pinned ? frame_floor_db : frame_floor_db + headroom_db;
    ends.ceiling_db = frame_ceiling_db;

    // The engine already holds its own two ends apart, and the correction
    // above has just eaten into that gap, so what is drawn against gets the
    // same bound re-applied. It comes out of whichever end is not pinned.
    if (ends.span_db() < kSpectrumMinimumSpanDb) {
        if (ceiling_pinned && !floor_pinned) {
            ends.floor_db = ends.ceiling_db - kSpectrumMinimumSpanDb;
        } else if (!ceiling_pinned) {
            ends.ceiling_db = ends.floor_db + kSpectrumMinimumSpanDb;
        }
        // Both pinned: the operator has said exactly what they want,
        // including a narrow span, and gets it.
    }
    return ends;
}

Rgb colour_at(float level)
{
    const float position = std::clamp(level, 0.0F, 1.0F);

    std::size_t upper = 1;
    while (upper + 1 < kStops.size() && kStops[upper].position < position) {
        ++upper;
    }

    const Stop& low = kStops[upper - 1];
    const Stop& high = kStops[upper];
    const float width = high.position - low.position;
    const float t = width <= 0.0F ? 0.0F : (position - low.position) / width;

    return Rgb{
        mix(low.colour.r, high.colour.r, t),
        mix(low.colour.g, high.colour.g, t),
        mix(low.colour.b, high.colour.b, t),
    };
}

std::uint32_t colour_argb_at(float level)
{
    const Rgb rgb = colour_at(level);
    return 0xFF000000U | (static_cast<std::uint32_t>(rgb.r) << 16U) |
           (static_cast<std::uint32_t>(rgb.g) << 8U) | static_cast<std::uint32_t>(rgb.b);
}

}  // namespace revenant::ui
