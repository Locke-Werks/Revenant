// The two readings marked on the span spectrum, and where their plates go:
// the noise floor, and the strongest signal across the whole source.
//
// HEADER ONLY, AND HOLDING NO Qt, so ui/tests can assert all of it without a
// window. render/spectrum_item.cpp feeds it frames and QML draws what it
// returns.
//
// ---------------------------------------------------------------------------
// THE NOISE FLOOR IS MEASURED HERE, BECAUSE THE ENGINE DOES NOT PUBLISH ONE
// ---------------------------------------------------------------------------
//
// The detector keeps a noise floor, per fine bin, and uses it on every
// decision: Detector::noise_floor() in core/detect/detector.h, and each
// candidate's noise_floor_dbfs beside it. None of it crosses the wire.
// rpc::Detection has no floor, rpc::DetectionList has the threshold and not
// the floor it is measured from, and SourceStats carries only the front
// end's floor lift, which is relative to a session low-water mark. So the
// floor drawn here is this window's own estimate, and it says so on its
// plate.
//
// IT IS NOT THE AUTO-SCALE FLOOR, which was the first thing to reach for.
// SpectrumFrame::floor_db is the colour map's bottom end: the frame's fifth
// percentile, smoothed on the device with a thirty second decay, which the
// span display lifts by a reduction correction so that an empty band's
// columns land on the bottom edge. On a flat band that is near the noise,
// which is what the correction is for, and a marker on it is then a line
// along the bottom edge saying the number the floor plate beside it already
// says. It stops being near the noise exactly when a floor marker would have
// something to say: when the operator pins the floor, for the thirty seconds
// the floor takes to follow noise that rose, and on a front end whose band
// edges roll off, where the fifth percentile comes from the edges.
//
// Measured with the auto-scale free, on the two sources the task names: the
// synthetic wideband scene, seed 7 at 2.4 MS/s, drew its floor at
// -92.5 dBFS and this estimate read -94.8; the labelled SigMF scene at
// 2.16 MS/s drew -92.5 and read -95.2. So on a flat band the estimate sits a
// few decibels under the bottom edge, the line is not drawn, and the plate
// says so with an arrow; qml/SpanView.qml has that case.
//
// What is measured instead is a low percentile of the trace as drawn,
// kNoiseColumnPermille of its columns, so the line runs through the lower
// part of the noise the operator is looking at. A quarter rather than the
// median because a broadcast band can be more than half occupied, and a
// median then sits on the stations' skirts; the quarter holds until three
// columns in four are signal. It is in the trace's own unit, the largest
// bin under a column, so it reads on the same scale as the peak beside it
// and moves by a decibel or two when the window is resized, as the trace's
// noise does. It is a display figure and not a calibrated noise density.
//
// ---------------------------------------------------------------------------
// THE PEAK IS THE WHOLE FRAME'S, HELD SO IT CAN BE READ
// ---------------------------------------------------------------------------
//
// The whole of what the source delivers, per the owner's correction of
// 2026-09-23: not the focused receiver's passband, and not a fallback that
// depends on whether a receiver is open. The span spectrum has no zoom, so
// the frame and the pane are the same width today; the search is written
// against the frame so that a zoom added later does not quietly narrow it to
// what is on screen.
//
// The largest bin of each frame is the right answer for one frame and an
// unreadable one for a sequence. Two carriers within a decibel of each other
// trade the maximum on noise alone, so the marker would jump across the pane
// at the frame rate; and the loudest bin inside one broadcast FM station
// wanders a hundred kilohertz either side with the programme, so even a
// single strong signal would make the marker shiver.
//
// So the peak is held. Each frame looks for the strongest bin near where the
// marker already is, and the marker moves to the frame's overall maximum only
// when that is kPeakSwitchDb louder than both the strongest bin near it and
// the level the marker is showing. Staying, the level
// and the position are both eased toward the new reading over
// kPeakSmoothSeconds of source time; switching, they jump, because easing the
// marker across the pane from one signal to another would draw it over
// frequencies neither of them is on.
//
// The cost of the hold, stated plainly: while the marker stays, the signal it
// names can be up to kPeakSwitchDb quieter than the loudest bin in the frame.
// A marker that hopped to every bin that won by a tenth of a decibel would be
// exact and useless.
//
// Time comes from the frames' sample indices, never a wall clock, on the rule
// the detection fade follows: a replayed capture eases at the rate it was
// recorded, not at the rate it is drawn.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace revenant::ui {

// ---------------------------------------------------------------------------
// The noise floor
// ---------------------------------------------------------------------------

// The share of the trace's columns that sit below the line, in thousandths.
inline constexpr std::uint32_t kNoiseColumnPermille = 250;

// The share below the plate's top edge, so the plate can be put under the
// noise rather than across it. See place_noise_plate.
inline constexpr std::uint32_t kNoiseLowPermille = 50;

// Easing, in seconds of source time. Slower than the peak's, because a floor
// that moves is news only when it moves for longer than a gust does.
inline constexpr double kNoiseSmoothSeconds = 1.0;

// The level below which a given share of values sit. Takes a copy, because
// the caller's columns are drawn after this and must stay in order.
[[nodiscard]] inline double percentile_of(std::span<const float> values, std::uint32_t permille)
{
    if (values.empty()) {
        return 0.0;
    }
    std::vector<float> sorted(values.begin(), values.end());
    const std::size_t last = sorted.size() - 1;
    const std::size_t at = std::min(
        last, static_cast<std::size_t>(static_cast<double>(last) *
                                       static_cast<double>(std::min(permille, 1000U)) / 1000.0));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(at),
                     sorted.end());
    return sorted[at];
}

struct NoiseFloor {
    bool valid = false;

    // Where the line is drawn, in the trace's unit.
    double level_db = 0.0;

    // Where the bottom of the noise is, for placing the plate under it.
    double low_db = 0.0;
};

// The floor after one more frame of columns. dt_seconds as for
// track_span_peak: zero or less takes the reading as it stands.
[[nodiscard]] inline NoiseFloor track_noise_floor(const NoiseFloor& previous,
                                                  std::span<const float> columns,
                                                  double dt_seconds)
{
    NoiseFloor next;
    if (columns.empty()) {
        return next;
    }
    next.valid = true;
    const double level = percentile_of(columns, kNoiseColumnPermille);
    const double low = percentile_of(columns, kNoiseLowPermille);
    if (!previous.valid || !(dt_seconds > 0.0)) {
        next.level_db = level;
        next.low_db = low;
        return next;
    }
    const double ease = 1.0 - std::exp(-dt_seconds / kNoiseSmoothSeconds);
    next.level_db = previous.level_db + (level - previous.level_db) * ease;
    next.low_db = previous.low_db + (low - previous.low_db) * ease;
    return next;
}

// ---------------------------------------------------------------------------
// The peak
// ---------------------------------------------------------------------------

// How much louder the frame's maximum has to be than the strongest bin near
// the held peak before the marker moves to it. Three decibels is twice the
// power: a signal that is plainly the stronger, where one a decibel up is a
// coin toss on a fading band.
inline constexpr double kPeakSwitchDb = 3.0;

// The easing time constant while the marker stays on one signal, in seconds
// of source time. A quarter second takes the frame-to-frame scatter out of
// the plate's figures and still follows a carrier being keyed.
inline constexpr double kPeakSmoothSeconds = 0.25;

// How far either side of the held peak counts as near it, as a fraction of
// the frame's bins, with a floor of kPeakNearMinBins. A two-hundredth of a
// 2.4 MHz span is 12 kHz, which holds a narrowband carrier through its drift
// and keeps the marker inside a broadcast station while its loudest bin
// wanders.
inline constexpr double kPeakNearFraction = 0.005;
inline constexpr std::size_t kPeakNearMinBins = 2;

struct SpanPeak {
    bool valid = false;

    // The bin, as a fractional index because it is eased, and the level in
    // dBFS of the bin it names: the same unit the trace draws, since a column
    // of the trace is the largest bin under it.
    double bin = 0.0;
    double level_db = 0.0;

    // How many bins the frame had, so a frame of another width is recognised
    // as a new axis rather than eased across.
    std::size_t bins = 0;
};

// The largest bin in [begin, end). end > begin is the caller's to ensure.
[[nodiscard]] inline std::size_t peak_bin(std::span<const float> bins, std::size_t begin,
                                          std::size_t end)
{
    std::size_t best = begin;
    for (std::size_t i = begin + 1; i < end; ++i) {
        if (bins[i] > bins[best]) {
            best = i;
        }
    }
    return best;
}

// The peak after one more frame.
//
// dt_seconds is the source time since the last frame this was given, from the
// frames' own sample indices. Zero or less means there is no previous frame
// to ease from, which is what a reconnect or a retune is, and takes the
// reading as it stands.
[[nodiscard]] inline SpanPeak track_span_peak(const SpanPeak& previous,
                                              std::span<const float> bins, double dt_seconds)
{
    SpanPeak next;
    if (bins.empty()) {
        return next;
    }
    next.valid = true;
    next.bins = bins.size();

    const std::size_t overall = peak_bin(bins, 0, bins.size());
    const double overall_db = bins[overall];

    const bool can_hold = previous.valid && previous.bins == bins.size() && dt_seconds > 0.0;
    if (!can_hold) {
        next.bin = static_cast<double>(overall);
        next.level_db = overall_db;
        return next;
    }

    const auto reach = std::max(
        kPeakNearMinBins,
        static_cast<std::size_t>(std::llround(static_cast<double>(bins.size()) * kPeakNearFraction)));
    const auto centre = static_cast<std::size_t>(std::clamp(
        std::llround(previous.bin), 0LL, static_cast<long long>(bins.size() - 1)));
    const std::size_t begin = centre > reach ? centre - reach : 0;
    const std::size_t end = std::min(bins.size(), centre + reach + 1);
    const std::size_t near = peak_bin(bins, begin, end);
    const double near_db = bins[near];

    // Against the louder of the held signal's reading this frame and its
    // eased level. The eased level is what stops a fade: two carriers a
    // decibel apart, each fading by one either way, put the frame's maximum
    // three decibels over the held carrier's own reading every few hundred
    // frames, and against that reading alone the marker hopped four times in
    // four hundred frames of ui/tests' case. The frame's reading is what lets
    // go of a signal that has stopped: its eased level falls to where the
    // next strongest takes over within a few frames rather than at once.
    if (overall_db > std::max(near_db, previous.level_db) + kPeakSwitchDb) {
        next.bin = static_cast<double>(overall);
        next.level_db = overall_db;
        return next;
    }

    const double ease = 1.0 - std::exp(-dt_seconds / kPeakSmoothSeconds);
    next.bin = previous.bin + (static_cast<double>(near) - previous.bin) * ease;
    next.level_db = previous.level_db + (near_db - previous.level_db) * ease;
    return next;
}

// ---------------------------------------------------------------------------
// Where the plates go
// ---------------------------------------------------------------------------
//
// Neither plate may cover the trace or the detection labels, and the owner
// asked for both. What makes it possible is that each reading says where the
// trace is not. Above the strongest signal there is no trace at all, since
// every other column is lower; and under the noise there is only the fill
// under the trace. So the peak's plate goes over its marker and the noise's
// goes under the noise, and each has one fallback for when the pane leaves
// no room, which is written down beside it.

struct PlateBox {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] constexpr double right() const { return x + width; }
    [[nodiscard]] constexpr double bottom() const { return y + height; }
    [[nodiscard]] constexpr bool empty() const { return !(width > 0.0) || !(height > 0.0); }

    [[nodiscard]] constexpr bool overlaps(const PlateBox& other) const
    {
        return !empty() && !other.empty() && x < other.right() && other.x < right() &&
               y < other.bottom() && other.y < bottom();
    }
};

// What the pane already has along its edges, which a plate keeps off.
struct PaneRoom {
    double width = 0.0;
    double height = 0.0;

    // The detection labels' strip along the top, which nothing covers.
    double top_px = 0.0;

    // The level scale's labels up the right edge.
    double right_px = 0.0;

    // The ceiling's pin plate in the top left corner, which a peak plate
    // steps around rather than covering.
    PlateBox keep_clear;
};

// The gap between a marker and the thing it marks, and the marker's length.
inline constexpr double kMarkerGapPx = 3.0;
inline constexpr double kPeakTickPx = 7.0;
inline constexpr double kPlateMarginPx = 4.0;

struct PeakPlacement {
    // The plate.
    PlateBox plate;

    // The tick from the plate down toward the peak, drawn only while the
    // plate is over it. x is the peak's column.
    bool tick = false;
    double tick_x = 0.0;
    double tick_top = 0.0;
    double tick_bottom = 0.0;

    // The peak is above the top of the scale, so it is not on screen as a
    // height; the plate says so rather than pointing at the top edge.
    bool above_scale = false;
};

// Where the peak's plate goes, for a peak drawn at (peak_x, peak_y) in the
// pane's own coordinates. peak_y is unclamped: above zero is on the trace,
// below zero is above the ceiling.
//
// Over the peak when there is room between it and the detection strip: that
// space is trace-free, because the peak is the tallest column. Otherwise
// beside it, just under the detection strip, on whichever side has room,
// which covers only what reaches that high beside the strongest signal.
[[nodiscard]] inline PeakPlacement place_peak_plate(double peak_x, double peak_y,
                                                    double plate_width, double plate_height,
                                                    const PaneRoom& room)
{
    PeakPlacement out;
    out.above_scale = peak_y < 0.0;
    out.plate.width = plate_width;
    out.plate.height = plate_height;

    const double left_bound = kPlateMarginPx;
    const double right_bound =
        std::max(left_bound, room.width - room.right_px - kPlateMarginPx - plate_width);
    const auto clamp_x = [&](double x) { return std::clamp(x, left_bound, right_bound); };

    const double marked_y = std::clamp(peak_y, 0.0, room.height);
    const double tick_bottom = marked_y - kMarkerGapPx;
    const double tick_top = tick_bottom - kPeakTickPx;
    const double over_y = tick_top - plate_height;

    if (!out.above_scale && over_y >= room.top_px) {
        out.plate.x = clamp_x(peak_x - plate_width / 2.0);
        out.plate.y = over_y;
        if (out.plate.overlaps(room.keep_clear)) {
            out.plate.x = clamp_x(room.keep_clear.right() + kPlateMarginPx);
        }
        out.tick = true;
        out.tick_x = peak_x;
        out.tick_top = tick_top;
        out.tick_bottom = tick_bottom;
        return out;
    }

    // Beside it, under the strip. Right of the peak first, since the pin
    // plate is in the left corner, then left of it.
    out.plate.y = room.top_px;
    const double right_of = peak_x + kMarkerGapPx + kPlateMarginPx;
    const double left_of = peak_x - kMarkerGapPx - kPlateMarginPx - plate_width;
    out.plate.x = right_of <= right_bound ? right_of : clamp_x(left_of);
    if (out.plate.overlaps(room.keep_clear)) {
        out.plate.y = room.keep_clear.bottom() + kPlateMarginPx;
    }
    return out;
}

// Where the noise floor's plate goes, for a line at noise_y whose noise
// reaches down to low_y.
//
// Right-aligned, under the scale's labels rather than beside them, because it
// is a reading on that scale. Under the noise when there is room between the
// bottom of the noise and the bottom of the pane: that strip holds only the
// fill under the trace. Otherwise on the bottom edge, which is where the noise
// itself is by then.
[[nodiscard]] inline PlateBox place_noise_plate(double noise_y, double low_y,
                                                double plate_width, double plate_height,
                                                const PaneRoom& room)
{
    PlateBox plate;
    plate.width = plate_width;
    plate.height = plate_height;
    plate.x = std::max(kPlateMarginPx, room.width - kPlateMarginPx - plate_width);

    const double under = std::max(noise_y, low_y) + kMarkerGapPx;
    const double lowest = room.height - kMarkerGapPx - plate_height;
    plate.y = std::max(room.top_px, std::min(under, lowest));
    return plate;
}

}  // namespace revenant::ui
