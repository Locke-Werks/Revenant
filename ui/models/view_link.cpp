// EngineLink's view: the part of the capture span the spectrum and the
// waterfall draw. The arithmetic is models/span_view.h; this is the state, the
// three ways it changes (a band pick, the operator's zoom and pan, the span
// moving under it) and the one mapping the displays read.
//
// ONE VIEW FOR BOTH DISPLAYS AND BOTH WAYS OF SETTING IT. The band menu and
// the zoom controls write the same window, so there is never a question of
// which zoom is in force. What differs is who owns it: a band view belongs to
// the band and lets go when the band does, a manual view belongs to the
// operator and stays until they change it.

#include "models/engine_link.h"

#include <cmath>

namespace revenant::ui {

double EngineLink::viewFrequencyAtFraction(double fraction) const
{
    const auto bins = static_cast<std::size_t>(info_.spectrum.bins);
    return frequencyAtFraction(span_fraction_at(view_bins(view_, bins), bins, fraction));
}

void EngineLink::set_view(SpanWindow view, ViewMode mode)
{
    const bool moved = view.low != view_.low || view.high != view_.high || mode != view_mode_;
    view_ = view;
    view_mode_ = mode;
    if (mode != ViewMode::Band) {
        band_view_armed_ = false;
    }
    if (moved) {
        emit viewChanged();
    }
}

void EngineLink::pickBand(double low_hz, double high_hz, double centre_hz)
{
    const double span_hz = spanHighHz() - spanLowHz();
    const BandViewPlan plan = plan_band_view(low_hz, high_hz, centre_hz, span_hz,
                                             sourceTuneLowHz(), sourceTuneHighHz());

    if (plan.zoom) {
        // Not narrowed yet: the front end is still where it was, and the band
        // is probably not in the span. follow_span_for_view narrows it when
        // the retune lands and the band is inside.
        band_view_low_hz_ = plan.view_low_hz;
        band_view_high_hz_ = plan.view_high_hz;
        set_view({}, ViewMode::Band);
        band_view_armed_ = false;
        follow_span_for_view();
    } else {
        set_view({}, ViewMode::Full);
    }

    // Retuned even when the centre would not move, so the menu still answers
    // a pick on a source that is already there with the same sentence path.
    tuneSourceHz(plan.tune_hz);
}

void EngineLink::zoomView(double factor, double pointer)
{
    const double min_width = min_view_width(static_cast<std::size_t>(info_.spectrum.bins));
    const SpanWindow next = zoom_about(view_, pointer, factor, min_width);
    set_view(next, next.full() ? ViewMode::Full : ViewMode::Manual);
}

void EngineLink::panView(double delta)
{
    if (view_.full()) {
        return;
    }
    const double min_width = min_view_width(static_cast<std::size_t>(info_.spectrum.bins));
    set_view(pan_view(view_, delta, min_width), ViewMode::Manual);
}

void EngineLink::fullSpan()
{
    set_view({}, ViewMode::Full);
}

void EngineLink::follow_span_for_view()
{
    const double low = spanLowHz();
    const double high = spanHighHz();

    if (!connected() || !(high > low)) {
        return;
    }

    if (view_mode_ == ViewMode::Band) {
        const bool inside = hz_window_inside(band_view_low_hz_, band_view_high_hz_, low, high);
        if (inside) {
            band_view_armed_ = true;
            const double min_width =
                min_view_width(static_cast<std::size_t>(info_.spectrum.bins));
            set_view(clamp_view(window_for_hz(band_view_low_hz_, band_view_high_hz_, low, high),
                                min_width),
                     ViewMode::Band);
        } else if (band_view_armed_) {
            // The band was on screen and the front end has moved it off: the
            // operator tuned away, so the whole span comes back.
            set_view({}, ViewMode::Full);
        }
        emit viewChanged();
        return;
    }

    if (view_mode_ == ViewMode::Manual) {
        // Fractions ride the retune. A new engine may have fewer bins, which
        // can make the window narrower than the minimum, hence the clamp.
        const double min_width = min_view_width(static_cast<std::size_t>(info_.spectrum.bins));
        set_view(clamp_view(view_, min_width), ViewMode::Manual);
    }

    // The view's hertz moved with the span even where its fractions did not,
    // and viewLowHz and viewHighHz notify on this signal alone.
    emit viewChanged();
}

void EngineLink::check_receiver_in_view()
{
    if (view_mode_ != ViewMode::Band || !band_view_armed_ || receiver_id_ == 0) {
        return;
    }
    const auto hz = static_cast<double>(receiver_absolute_hz_);
    if (hz < band_view_low_hz_ || hz > band_view_high_hz_) {
        set_view({}, ViewMode::Full);
    }
}

}  // namespace revenant::ui
