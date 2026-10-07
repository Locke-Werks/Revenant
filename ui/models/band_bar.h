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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
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

// Clear space between two segments that share an edge in one lane, split
// evenly between them so neither band looks shorter than the other.
inline constexpr double kBandBarAbutGapPx = 2.0;

// Edges this close count as shared. Two rows a few hertz apart land within a
// pixel of each other and would merge just the same.
inline constexpr double kBandBarAbutTolerancePx = 1.0;

// An elided label must keep at least this many characters before its
// ellipsis. "8…" or "2 m …" names nothing; the bracket alone says a band is
// there and the tooltip says which.
inline constexpr int kBandBarMinLabelChars = 4;

// Whether a label is worth drawing, given the name's length and how many of
// its characters survived eliding. A name that fits whole is always drawn,
// so a short channel name such as "8TAC91" shows whenever it has the room.
[[nodiscard]] inline bool band_bar_label_readable(int full_chars, int visible_chars)
{
    if (full_chars <= 0 || visible_chars <= 0) {
        return false;
    }
    return visible_chars >= full_chars || visible_chars >= kBandBarMinLabelChars;
}

// name with a leading "prefix " removed, or name unchanged when it does not
// start that way. The space is required so "2 mm" is not cut under "2 m", and
// a name equal to the prefix stays whole rather than becoming empty.
[[nodiscard]] inline std::string_view band_bar_strip_prefix(std::string_view name,
                                                            std::string_view prefix)
{
    if (prefix.empty() || name.size() <= prefix.size() + 1 ||
        name.substr(0, prefix.size()) != prefix || name[prefix.size()] != ' ') {
        return name;
    }
    std::string_view rest = name.substr(prefix.size());
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    return rest.empty() ? name : rest;
}

struct BandBarSegment {
    double x0 = 0.0;
    double x1 = 0.0;
    int lane = 0;

    // The band runs on past this edge of the view, so no hard edge is drawn
    // there: a tick at the view's edge would claim the band ends where the
    // screen does.
    bool clipped_left = false;
    bool clipped_right = false;

    // Another segment in the same lane ends where this one starts (or starts
    // where it ends). Drawn flush, the two brackets would fuse into one thick
    // bar and "][" would read as a single band.
    bool abuts_left = false;
    bool abuts_right = false;

    // The extent to paint: x0..x1 pulled in by half the gap on each abutting
    // side. x0..x1 stay the true edges, for hover and for labels.
    double draw_x0 = 0.0;
    double draw_x1 = 0.0;

    const Band* band = nullptr;

    // The label text: band->name less a leading prefix that repeats the name
    // of a band drawn in a lane above and containing it, so "2 m CW only"
    // under "2 m" reads "CW only". The data keeps full names because the menu,
    // the palette and the tooltip show a row with no parent beside it.
    std::string_view display_name;

    // A bracket is drawn on a side that is the band's real edge.
    [[nodiscard]] bool bracket_left() const { return !clipped_left; }
    [[nodiscard]] bool bracket_right() const { return !clipped_right; }
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
        BandBarSegment segment;
        segment.x0 = x0;
        segment.x1 = x1;
        segment.lane = lane;
        segment.clipped_left = true_x0 < 0.0;
        segment.clipped_right = true_x1 > width_px;
        segment.band = band;
        out.segments.push_back(segment);
        out.lanes_used = std::max(out.lanes_used, lane + 1);
    }

    // Abutment is decided after packing, since a later row can land against
    // an earlier one. Lanes hold a handful of segments, so pairwise is fine.
    for (auto& a : out.segments) {
        for (const auto& b : out.segments) {
            if (&a == &b || a.lane != b.lane) {
                continue;
            }
            if (!a.clipped_left && std::abs(b.x1 - a.x0) <= kBandBarAbutTolerancePx &&
                b.x0 < a.x0) {
                a.abuts_left = true;
            }
            if (!a.clipped_right && std::abs(b.x0 - a.x1) <= kBandBarAbutTolerancePx &&
                b.x1 > a.x1) {
                a.abuts_right = true;
            }
        }
    }
    const double half_gap = kBandBarAbutGapPx / 2.0;
    for (auto& s : out.segments) {
        s.draw_x0 = s.abuts_left ? s.x0 + half_gap : s.x0;
        s.draw_x1 = s.abuts_right ? s.x1 - half_gap : s.x1;
        // A segment narrower than its own gaps still keeps a sliver to show.
        if (s.draw_x1 <= s.draw_x0) {
            const double mid = (s.x0 + s.x1) / 2.0;
            s.draw_x0 = mid - 0.5;
            s.draw_x1 = mid + 0.5;
        }
    }

    // Prefix stripping only against bands actually drawn above: a parent that
    // went to the overflow or was never in view leaves the child its full
    // name, since nothing on screen supplies the part that was cut. Deepest
    // ancestor first, so "2 m repeater in 1" under "2 m repeater" under "2 m"
    // loses both layers.
    for (auto& s : out.segments) {
        s.display_name = s.band->name;
        for (int lane = s.lane - 1; lane >= 0; --lane) {
            for (const auto& p : out.segments) {
                if (p.lane != lane || p.band->low_hz > s.band->low_hz ||
                    p.band->high_hz < s.band->high_hz) {
                    continue;
                }
                const std::string_view cut = band_bar_strip_prefix(s.display_name, p.band->name);
                if (cut.size() != s.display_name.size()) {
                    s.display_name = cut;
                    break;
                }
            }
        }
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
