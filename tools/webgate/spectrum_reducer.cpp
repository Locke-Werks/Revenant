#include "tools/webgate/spectrum_reducer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace revenant::webgate {

namespace {

// Pixel edges land exactly on bin edges whenever the view is aligned to the
// spectrum, and double rounding there would otherwise hand a boundary bin to
// the wrong pixel or to none. A bin is owned by every pixel that overlaps it
// by more than this fraction of a bin.
constexpr double kEdgeSlack = 1e-9;

// A pixel within this fraction of a bin wide counts as a full bin. At exactly
// one bin per pixel the centre-bin rule rounds the centre onto a bin boundary
// and can duplicate one bin while dropping its neighbour; the overlap rule
// with kEdgeSlack is stable there.
constexpr double kTieFraction = 1e-6;

constexpr float kNothing = -std::numeric_limits<float>::infinity();

}  // namespace

ReducedColumns reduce_columns(std::span<const float> power_db, double first_bin_hz, double bin_hz,
                              const View& view_in) {
    ReducedColumns out;
    out.view = view_in;
    // The phone chooses its own width. Nothing is allocated from it unbounded.
    out.view.width_px = std::min(view_in.width_px, kMaxWidthPx);
    const View& view = out.view;
    out.db.assign(view.width_px, kNothing);

    // Everything below casts doubles to integers, which is undefined for NaN,
    // infinity and out-of-range values, so refuse them here once.
    if (power_db.empty() || view.width_px == 0 || !std::isfinite(view.center_hz) ||
        !std::isfinite(view.span_hz) || !(view.span_hz > 0.0) || !std::isfinite(first_bin_hz) ||
        !std::isfinite(bin_hz) || !(bin_hz > 0.0)) {
        return out;
    }

    const auto bins = static_cast<std::int64_t>(power_db.size());
    const double px_hz = view.span_hz / static_cast<double>(view.width_px);
    const double px_bins = px_hz / bin_hz;
    out.bins_per_px = px_bins >= 2.0 && px_bins < 1e9 ? static_cast<std::size_t>(px_bins) : 1;
    // Subtract the large absolute frequencies (RF in the GHz) from each other
    // before adding the small per-pixel offset, so they cancel exactly.
    const double base_hz = (view.center_hz - first_bin_hz) - view.span_hz / 2.0;
    const double lo_clamp = -1.0;
    const double hi_clamp = static_cast<double>(bins) + 1.0;
    const bool narrow = px_bins < 1.0 - kTieFraction;

    std::int64_t lowest_bin = bins;
    std::int64_t highest_bin = -1;
    bool any = false;
    for (std::uint32_t i = 0; i < view.width_px; ++i) {
        // Position in bin units where bin j spans [j, j + 1): bin_hz wide and
        // centred on first_bin_hz + j * bin_hz. Clamped before any integer
        // conversion; the spectrum is [0, bins) so nothing is lost.
        // Both edges come from the raw position: clamping u0 first and then
        // adding the width would drag a pixel far below the spectrum back in.
        const double raw0 = (base_hz + px_hz * i) / bin_hz + 0.5;
        const double u0 = std::clamp(raw0, lo_clamp, hi_clamp);
        const double u1 = std::clamp(raw0 + px_bins, lo_clamp, hi_clamp);

        std::int64_t lo;
        std::int64_t hi;
        if (narrow) {
            // Narrower than a bin: the bin under the centre, so zooming in
            // shows blocks of that bin rather than a max over its neighbours.
            // The centre is taken unclamped so a pixel that straddles the
            // spectrum edge is judged by where its centre really is.
            const double centre = (base_hz + px_hz * (i + 0.5)) / bin_hz + 0.5;
            if (!(centre >= 0.0) || centre >= static_cast<double>(bins)) continue;
            lo = hi = static_cast<std::int64_t>(std::floor(centre));
        } else {
            lo = static_cast<std::int64_t>(std::floor(u0 + kEdgeSlack));
            hi = static_cast<std::int64_t>(std::ceil(u1 - kEdgeSlack)) - 1;
        }
        if (hi < 0 || lo >= bins) continue;  // outside the spectrum
        lo = std::max<std::int64_t>(lo, 0);
        hi = std::min<std::int64_t>(hi, bins - 1);

        if (!any) out.first_valid = i;
        any = true;
        out.end_valid = i + 1;
        lowest_bin = std::min(lowest_bin, lo);
        highest_bin = std::max(highest_bin, hi);

        // NaN bins carry no signal; a pixel of nothing else stays at the floor.
        float peak = kNothing;
        for (std::int64_t j = lo; j <= hi; ++j) {
            const float v = power_db[static_cast<std::size_t>(j)];
            if (v > peak) peak = v;  // false for NaN
        }
        out.db[i] = peak;
    }
    if (any) {
        out.first_bin = static_cast<std::size_t>(lowest_bin);
        out.bin_count = static_cast<std::size_t>(highest_bin - lowest_bin + 1);
    }
    return out;
}

std::vector<std::uint8_t> quantise(std::span<const float> db, float floor_db, float ceiling_db,
                                   float& offset_db, float& scale_db) {
    const bool range_ok = std::isfinite(floor_db) && std::isfinite(ceiling_db) &&
                          ceiling_db > floor_db;
    offset_db = std::isfinite(floor_db) ? floor_db : 0.0f;
    scale_db = range_ok ? (ceiling_db - floor_db) / 255.0f : 1.0f;
    std::vector<std::uint8_t> px(db.size(), 0);
    if (!range_ok) {
        return px;
    }
    for (std::size_t i = 0; i < db.size(); ++i) {
        const float c = std::clamp(db[i], floor_db, ceiling_db);
        const float q = std::round((c - offset_db) / scale_db);
        px[i] = static_cast<std::uint8_t>(std::clamp(q, 0.0f, 255.0f));
    }
    return px;
}

ReducedFrame reduce(std::span<const float> power_db, double first_bin_hz, double bin_hz,
                    std::uint64_t source_start, const View& view_in, float floor_db,
                    float ceiling_db) {
    ReducedFrame out;
    out.source_start = source_start;
    const ReducedColumns columns = reduce_columns(power_db, first_bin_hz, bin_hz, view_in);
    out.view = columns.view;
    out.px = quantise(columns.db, floor_db, ceiling_db, out.offset_db, out.scale_db);
    return out;
}

std::uint32_t every_nth_at_most(double block_rate_hz, double max_fps) {
    if (!std::isfinite(block_rate_hz) || !std::isfinite(max_fps) || !(block_rate_hz > 0.0) ||
        !(max_fps > 0.0)) {
        return 1;
    }
    // Up, since this is a ceiling: rounding to the nearest sends 61 rows a
    // second from 305 under a 60 cap. The slack keeps a rate a hair over an
    // exact multiple, from float division, from costing a whole extra skip.
    const double n = std::ceil(block_rate_hz / max_fps - 1e-9);
    // Cap before the cast: a corrupt rate must not overflow into 0 or UB.
    return n < 1.0 ? 1u : static_cast<std::uint32_t>(std::min(n, 4.0e9));
}

}  // namespace revenant::webgate
