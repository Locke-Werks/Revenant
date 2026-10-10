// The part of the capture span the spectrum and the waterfall draw, and the
// band menu's decision about where to put the front end and that view.
//
// WHAT A VIEW IS. A window [low, high] in fractions of the capture span, 0 the
// low edge of the first bin and 1 the high edge of the last, which is the
// convention EngineLink::frequencyAtFraction uses. The displays draw the bins
// inside it across their whole width, so zooming in shows fewer bins per
// column. It is a display zoom: the engine still computes the same FFT over
// the whole span, and once the window is narrower than one bin per column the
// trace steps rather than gaining detail. docs/ui-spectrum.md says what extra
// resolution would take.
//
// WHY THE WINDOW IS SNAPPED TO WHOLE BINS. The trace is a reduction of a run
// of bins, and the axis is frequencyAtFraction. If the window cut a bin in
// half the two would disagree by up to half a bin at each edge, which is the
// failure the comment on build_detection_boxes exists to prevent. view_bins
// rounds outward to bin edges and EngineLink maps through the rounded pair,
// so the trace, the axis and every overlay are placed from the same two
// integers.
//
// Qt-free so ui/tests can hold it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace revenant::ui {

struct SpanWindow {
    double low = 0.0;
    double high = 1.0;

    [[nodiscard]] constexpr double width() const { return high - low; }
    [[nodiscard]] constexpr bool full() const { return low <= 0.0 && high >= 1.0; }
};

// Margin either side of a band the view isolates, as a fraction of the band's
// width, so the band edges are visible as edges rather than sitting on the
// frame.
inline constexpr double kBandViewMargin = 0.05;

// Where a band too wide for the span starts, as a fraction of the span in
// from the left edge, for the same reason.
inline constexpr double kBandStartMargin = 0.02;

// The narrowest view, in bins. Below this the trace is a few wide steps and
// says nothing a narrower view would add.
inline constexpr std::size_t kMinViewBins = 32;

// One wheel notch or zoom key, as a factor on the view's width.
inline constexpr double kZoomStep = 1.5;

// One pan key or Shift+wheel notch, as a fraction of the view's width.
inline constexpr double kPanStep = 0.1;

// The narrowest window as a fraction of the span. Zero bins (no picture yet)
// allows anything, since there is nothing to draw.
[[nodiscard]] inline double min_view_width(std::size_t bins)
{
    if (bins == 0) {
        return 0.0;
    }
    return std::min(1.0, static_cast<double>(kMinViewBins) / static_cast<double>(bins));
}

// Keep the window inside the span and at least min_width wide. A window that
// runs off one end is slid back rather than cut, so the width the operator
// chose survives a pan into the edge.
[[nodiscard]] inline SpanWindow clamp_view(SpanWindow view, double min_width)
{
    double width = view.width();
    if (!(width > 0.0) || !std::isfinite(width)) {
        return {};
    }
    width = std::clamp(width, std::min(min_width, 1.0), 1.0);
    double low = std::isfinite(view.low) ? view.low : 0.0;
    low = std::clamp(low, 0.0, 1.0 - width);
    return {low, low + width};
}

// Zoom by factor (above one narrows) about the point at pointer, a fraction
// across the CURRENT view, so the frequency under the pointer stays under it.
[[nodiscard]] inline SpanWindow zoom_about(SpanWindow view, double pointer, double factor,
                                           double min_width)
{
    if (!(factor > 0.0)) {
        return view;
    }
    pointer = std::clamp(pointer, 0.0, 1.0);
    const double anchor = view.low + pointer * view.width();
    const double width = std::clamp(view.width() / factor, std::min(min_width, 1.0), 1.0);
    return clamp_view({anchor - pointer * width, anchor - pointer * width + width}, min_width);
}

// Slide by delta view-widths; positive moves the view up in frequency.
[[nodiscard]] inline SpanWindow pan_view(SpanWindow view, double delta, double min_width)
{
    const double shift = delta * view.width();
    return clamp_view({view.low + shift, view.high + shift}, min_width);
}

// The bins the window covers, rounded outward to whole bins: [first, last).
struct ViewBins {
    std::size_t first = 0;
    std::size_t last = 0;

    [[nodiscard]] constexpr std::size_t count() const { return last - first; }
};

[[nodiscard]] inline ViewBins view_bins(SpanWindow view, std::size_t bins)
{
    if (bins == 0) {
        return {};
    }
    const double n = static_cast<double>(bins);
    // A hair of tolerance so a window computed from exact bin edges does not
    // round out by a whole bin on floating-point noise.
    constexpr double kSlack = 1e-9;
    auto first = static_cast<std::size_t>(std::max(0.0, std::floor(view.low * n + kSlack)));
    auto last = static_cast<std::size_t>(std::min(n, std::ceil(view.high * n - kSlack)));
    first = std::min(first, bins - 1);
    last = std::clamp(last, first + 1, bins);
    return {first, last};
}

// The span fraction a fraction across the view lands on, through the snapped
// bins. This is what EngineLink::viewFrequencyAtFraction feeds
// frequencyAtFraction, and its inverse below is what every overlay uses.
[[nodiscard]] inline double span_fraction_at(ViewBins drawn, std::size_t bins, double fraction)
{
    if (bins == 0 || drawn.count() == 0) {
        return fraction;
    }
    return (static_cast<double>(drawn.first) + fraction * static_cast<double>(drawn.count())) /
           static_cast<double>(bins);
}

// A window covering [low_hz, high_hz] of a span [span_low, span_high].
[[nodiscard]] inline SpanWindow window_for_hz(double low_hz, double high_hz, double span_low,
                                              double span_high)
{
    const double span = span_high - span_low;
    if (!(span > 0.0)) {
        return {};
    }
    return {(low_hz - span_low) / span, (high_hz - span_low) / span};
}

// Whether a window in hertz lies wholly inside the span. A band view holds
// only while this is true: tuning the front end so the band leaves the span
// is the operator moving on, and the view goes back to the whole span.
[[nodiscard]] inline bool hz_window_inside(double low_hz, double high_hz, double span_low,
                                           double span_high)
{
    return high_hz > low_hz && low_hz >= span_low && high_hz <= span_high;
}

// What picking a band from the menu does.
struct BandViewPlan {
    // The front end's new centre.
    double tune_hz = 0.0;

    // True when the whole band, with its margin, fits in the span: the view
    // then narrows to [view_low_hz, view_high_hz]. False leaves the whole
    // span on screen with the band's low edge near the left.
    bool zoom = false;
    double view_low_hz = 0.0;
    double view_high_hz = 0.0;
};

// band_low/high are the band's edges, centre the table's landing frequency
// (models/band_plan.h chooses it as the part worth landing on), span_hz the
// capture span's width and tune_low/high the range the source will take
// (equal or inverted when unknown, which skips the clamp).
//
// A band that fits is tuned to the table's centre where that keeps the whole
// band and its margin inside the span, which is the menu's behaviour before
// this view existed; otherwise to the nearest centre that does. No DC offset
// is applied: the tune path never avoided DC and the band menu follows it.
[[nodiscard]] inline BandViewPlan plan_band_view(double band_low, double band_high, double centre,
                                                 double span_hz, double tune_low, double tune_high)
{
    BandViewPlan plan;
    const double width = band_high - band_low;
    const double margin = width * kBandViewMargin;
    const double half = span_hz / 2.0;

    if (span_hz > 0.0 && width > 0.0 && width + 2.0 * margin <= span_hz) {
        // Any centre in [high + margin - half, low - margin + half] keeps
        // the padded band inside the span.
        plan.tune_hz = std::clamp(centre, band_high + margin - half, band_low - margin + half);
        plan.zoom = true;
        plan.view_low_hz = band_low - margin;
        plan.view_high_hz = band_high + margin;
    } else if (span_hz > 0.0) {
        plan.tune_hz = band_low - kBandStartMargin * span_hz + half;
    } else {
        plan.tune_hz = centre;
    }

    if (tune_high > tune_low) {
        plan.tune_hz = std::clamp(plan.tune_hz, tune_low, tune_high);
    }
    return plan;
}

}  // namespace revenant::ui
