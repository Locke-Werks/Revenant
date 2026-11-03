#include "tools/webgate/spectrum_reducer.h"

#include <algorithm>
#include <cmath>

namespace revenant::webgate {

namespace {

// Pixel edges land exactly on bin edges whenever the view is aligned to the
// spectrum, and double rounding there would otherwise hand a boundary bin to
// the wrong pixel or to none. A bin is owned by every pixel that overlaps it
// by more than this fraction of a bin.
constexpr double kEdgeSlack = 1e-9;

}  // namespace

ReducedFrame reduce(std::span<const float> power_db, double first_bin_hz, double bin_hz,
                    std::uint64_t source_start, const View& view, float floor_db,
                    float ceiling_db) {
    ReducedFrame out;
    out.source_start = source_start;
    out.view = view;
    out.offset_db = floor_db;
    // A degenerate range would divide by zero below; any positive step keeps
    // decode well defined and every pixel at the floor.
    const float range = ceiling_db - floor_db;
    out.scale_db = range > 0.0f ? range / 255.0f : 1.0f;
    out.px.assign(view.width_px, 0);

    if (power_db.empty() || view.width_px == 0 || !(bin_hz > 0.0) || !(view.span_hz > 0.0)) {
        return out;
    }

    const auto bins = static_cast<std::int64_t>(power_db.size());
    const double px_hz = view.span_hz / static_cast<double>(view.width_px);
    const double left_hz = view.center_hz - view.span_hz / 2.0;

    const auto quantise = [&](float db) -> std::uint8_t {
        if (std::isnan(db)) return 0;
        const float c = std::clamp(db, floor_db, ceiling_db);
        const float q = std::round((c - out.offset_db) / out.scale_db);
        return static_cast<std::uint8_t>(std::clamp(q, 0.0f, 255.0f));
    };

    for (std::uint32_t i = 0; i < view.width_px; ++i) {
        // Position in bin units where bin j spans [j, j + 1): bin_hz wide and
        // centred on first_bin_hz + j * bin_hz.
        const double u0 = (left_hz + px_hz * i - first_bin_hz) / bin_hz + 0.5;
        const double u1 = u0 + px_hz / bin_hz;

        std::int64_t lo;
        std::int64_t hi;
        if (px_hz <= bin_hz) {
            // Narrower than a bin: the bin under the centre, so zooming in
            // shows blocks of that bin rather than a max over its neighbours.
            lo = hi = static_cast<std::int64_t>(std::floor((u0 + u1) / 2.0));
        } else {
            lo = static_cast<std::int64_t>(std::floor(u0 + kEdgeSlack));
            hi = static_cast<std::int64_t>(std::ceil(u1 - kEdgeSlack)) - 1;
        }
        if (hi < 0 || lo >= bins) continue;  // outside the spectrum: floor
        lo = std::max<std::int64_t>(lo, 0);
        hi = std::min<std::int64_t>(hi, bins - 1);

        float peak = power_db[static_cast<std::size_t>(lo)];
        for (std::int64_t j = lo + 1; j <= hi; ++j) {
            peak = std::max(peak, power_db[static_cast<std::size_t>(j)]);
        }
        out.px[i] = quantise(peak);
    }
    return out;
}

std::uint32_t every_nth_for(double block_rate_hz, double target_fps) {
    if (!(block_rate_hz > 0.0) || !(target_fps > 0.0)) return 1;
    const double n = std::round(block_rate_hz / target_fps);
    // Cap before the cast: a corrupt rate must not overflow into 0 or UB.
    return n < 1.0 ? 1u : static_cast<std::uint32_t>(std::min(n, 4.0e9));
}

}  // namespace revenant::webgate
