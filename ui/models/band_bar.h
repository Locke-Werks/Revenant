// The band bar: which band-plan rows the visible span crosses, as pixel
// segments stacked into lanes. render/spectrum_item.h draws it.
//
// Qt-free so ui/tests can hold it. The x of every edge comes from ruler_x, the
// one hertz-to-pixel mapping the trace, the ruler and the overlays share, so a
// band edge lands on the column that frequency is drawn in.
//
// LANES ARE GREEDY INTERVAL PACKING IN THE ORDER GIVEN. bands_overlapping hands
// rows over widest first, so an allocation takes lane 0 and its sub-bands and
// channels stack beneath it. A row that fits in no lane below the cap is not
// drawn; it is counted into a "+N" marker over where it would have been, so
// the bar never claims a stretch of spectrum is unallocated when it is only
// crowded.
//
// CHANNELS ONLY WHEN THEY ARE WIDE ENOUGH TO READ. A channel row narrower than
// kBandBarMinChannelPx is left out entirely: at a wide span the FRS channels
// would otherwise fill every lane with slivers, push the allocation's own
// sub-bands into the overflow and turn the marker into a count of channels
// nobody can see. Left out, they count toward neither lanes nor overflow.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "models/band_plan.h"
#include "models/ruler.h"

namespace revenant::ui {

inline constexpr int kBandBarMaxLanes = 3;

// Below this a segment is not drawn at all. A sub-pixel sliver is a band the
// operator cannot hover and whose edge ticks would sit on one another.
inline constexpr double kBandBarMinSegmentPx = 1.0;

// Below this a channel row is left out, see above.
inline constexpr double kBandBarMinChannelPx = 6.0;

struct BandBarSegment {
    double x0 = 0.0;
    double x1 = 0.0;
    int lane = 0;

    // The band runs on past this edge of the view, so no hard edge is drawn
    // there: a tick at the view's edge would claim the band ends where the
    // screen does.
    bool clipped_left = false;
    bool clipped_right = false;

    const Band* band = nullptr;
};

// A run of rows that fit in no lane, and where to say so.
struct BandBarOverflow {
    double x = 0.0;
    int count = 0;
};

struct BandBar {
    std::vector<BandBarSegment> segments;
    std::vector<BandBarOverflow> overflow;

    // The deepest lane in use plus one, so the item knows how tall to be.
    int lanes_used = 0;
};

[[nodiscard]] inline BandBar build_band_bar(const std::vector<const Band*>& rows,
                                            double span_low_hz, double span_high_hz,
                                            double width_px, int max_lanes = kBandBarMaxLanes)
{
    BandBar out;
    if (!(span_high_hz > span_low_hz) || !(width_px > 0.0) || max_lanes <= 0) {
        return out;
    }

    struct Interval {
        double x0;
        double x1;
    };
    std::vector<std::vector<Interval>> lanes(static_cast<std::size_t>(max_lanes));
    std::vector<Interval> dropped;

    const auto overlaps = [](const Interval& a, const Interval& b) {
        // Touching is not overlapping: two adjacent channels share an edge and
        // belong in one lane.
        return a.x0 < b.x1 && b.x0 < a.x1;
    };

    for (const Band* band : rows) {
        if (band == nullptr) {
            continue;
        }
        const double true_x0 =
            ruler_x(static_cast<double>(band->low_hz), span_low_hz, span_high_hz, width_px);
        const double true_x1 =
            ruler_x(static_cast<double>(band->high_hz), span_low_hz, span_high_hz, width_px);
        if (band->kind == BandKind::kChannel && true_x1 - true_x0 < kBandBarMinChannelPx) {
            continue;
        }

        const double x0 = std::max(true_x0, 0.0);
        const double x1 = std::min(true_x1, width_px);
        if (x1 - x0 < kBandBarMinSegmentPx) {
            continue;
        }

        const Interval here{x0, x1};
        int lane = -1;
        for (int i = 0; i < max_lanes && lane < 0; ++i) {
            const auto& taken = lanes[static_cast<std::size_t>(i)];
            const bool clear = std::none_of(taken.begin(), taken.end(), [&](const Interval& t) {
                return overlaps(t, here);
            });
            if (clear) {
                lane = i;
            }
        }
        if (lane < 0) {
            dropped.push_back(here);
            continue;
        }

        lanes[static_cast<std::size_t>(lane)].push_back(here);
        out.segments.push_back(BandBarSegment{x0, x1, lane, true_x0 < 0.0,
                                              true_x1 > width_px, band});
        out.lanes_used = std::max(out.lanes_used, lane + 1);
    }

    // One marker per cluster of overlapping drops rather than one per row, so
    // six crowded channels read as "+6" once and not six stacked badges.
    std::sort(dropped.begin(), dropped.end(),
              [](const Interval& a, const Interval& b) { return a.x0 < b.x0; });
    for (std::size_t i = 0; i < dropped.size();) {
        double lo = dropped[i].x0;
        double hi = dropped[i].x1;
        int count = 0;
        while (i < dropped.size() && dropped[i].x0 < hi) {
            hi = std::max(hi, dropped[i].x1);
            ++count;
            ++i;
        }
        lo = std::max(lo, 0.0);
        out.overflow.push_back(BandBarOverflow{(lo + hi) / 2.0, count});
    }
    return out;
}

}  // namespace revenant::ui
