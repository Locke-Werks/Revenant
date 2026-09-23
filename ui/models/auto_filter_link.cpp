// EngineLink's side of the auto filter: averaging passband frames while a fit
// is due, asking models/auto_filter.h for the fit, and applying it once. The
// rules are all in that header; this file is when, and plumbing.
//
// WHEN A FIT IS DUE. A click-to-tune on a detection, and switching the toggle
// on. Nothing else arms it: a filter that refitted itself on every retune or
// every frame would be a second operator on the handles. Any tuning by hand
// while one is due drops it, and so does touching the filter: the operator's
// own hand always wins, and the fit is not held over to land after the drag.

#include "models/engine_link.h"

#include <string>

namespace revenant::ui {

namespace {

[[nodiscard]] double seconds(const QElapsedTimer& clock)
{
    return static_cast<double>(clock.elapsed()) / 1000.0;
}

}  // namespace

void EngineLink::setAutoFilterEnabled(bool on)
{
    if (on == auto_filter_enabled_) {
        return;
    }
    auto_filter_enabled_ = on;
    auto_filter_last_ = {};
    auto_filter_due_ = false;
    auto_filter_average_.reset();
    if (on) {
        // Switching it on is asking for a fit to the receiver as it stands.
        arm_auto_filter();
        return;
    }
    emit autoFilterChanged();
}

bool EngineLink::autoFilterOffered() const
{
    return auto_filter_rule_for(receiverDemod().toStdString()) != AutoFilterRule::None;
}

QString EngineLink::autoFilterState() const
{
    return QString::fromStdString(auto_filter_label(auto_filter_enabled_, auto_filter_due_,
                                                    auto_filter_last_,
                                                    receiverDemod().toStdString()));
}

void EngineLink::arm_auto_filter(bool labelled)
{
    // A click on a labelled detection fits once whether or not the toggle is
    // on: the owner's request of 2026-09-23 is that a label sets the receiver,
    // filter included, and this is the one fit the client has. The toggle
    // still decides for every other click. See models/label_tune.h.
    if (!auto_filter_enabled_ && !labelled) {
        return;
    }
    auto_filter_once_ = !auto_filter_enabled_;
    auto_filter_due_ = true;
    auto_filter_average_.reset();
    emit autoFilterChanged();
}

void EngineLink::cancel_auto_filter(AutoFilterOutcome why)
{
    if (!auto_filter_due_) {
        // Nothing measuring. The last fit's result still goes when the
        // receiver is somewhere new, or it is read as a statement about the
        // signal the receiver is on now: seen live as "fitted nfm" over a
        // filter that was an AM fit on another station.
        if (why == AutoFilterOutcome::Idle && auto_filter_last_.outcome != AutoFilterOutcome::Idle) {
            auto_filter_last_ = {};
            emit autoFilterChanged();
        }
        return;
    }
    auto_filter_due_ = false;
    auto_filter_average_.reset();
    auto_filter_last_ = {};
    auto_filter_last_.outcome = why;
    auto_filter_last_.low_hz = static_cast<int>(wanted_.passband_low);
    auto_filter_last_.high_hz = static_cast<int>(wanted_.passband_high);
    emit autoFilterChanged();
}

void EngineLink::run_auto_filter()
{
    if ((!auto_filter_enabled_ && !auto_filter_once_) || !auto_filter_due_ || receiver_id_ == 0) {
        return;
    }
    if (dragging_) {
        cancel_auto_filter(AutoFilterOutcome::Dragging);
        return;
    }
    const rpc::PassbandFrame& frame = passband_display_;
    if (frame.power_db.empty()) {
        return;
    }

    // Not until the engine has said the receiver is where this client put it.
    // Frames measured around the old centre can still be arriving for a block
    // or two after a retune, and a fit read off them would put the filter
    // where the signal was relative to a centre the receiver has left.
    if (receiver_status_.params.center != wanted_.center) {
        return;
    }
    if (frame.vrx != auto_filter_vrx_) {
        auto_filter_vrx_ = frame.vrx;
        auto_filter_average_.reset();
    }

    const rpc::PassbandGeometry& geometry = frame.geometry;
    const double bin_hz = geometry.bin_width.denominator == 0
                              ? 0.0
                              : static_cast<double>(geometry.bin_width.numerator) /
                                    static_cast<double>(geometry.bin_width.denominator);
    const double window_s = geometry.rate == 0 ? 0.0
                                               : static_cast<double>(geometry.transform) /
                                                     static_cast<double>(geometry.rate);
    auto_filter_average_.add(frame.power_db, passbandFrequencyAtFraction(0.0) + 0.5 * bin_hz,
                             bin_hz, frame.percentile_low_db, window_s,
                             seconds(scroll_clock_));

    const AutoFilterSettings settings;
    if (!auto_filter_average_.ready(settings)) {
        return;
    }

    const std::vector<float> averaged = auto_filter_average_.power_db();
    AutoFilterInput in;
    in.rule = auto_filter_rule_for(receiverDemod().toStdString());
    in.power_db = averaged;
    in.first_bin_hz = auto_filter_average_.first_bin_hz();
    in.bin_hz = auto_filter_average_.bin_hz();
    in.noise_db = auto_filter_average_.noise_db();
    in.centre_hz = receiverCenterHz();
    in.low_hz = static_cast<int>(wanted_.passband_low);
    in.high_hz = static_cast<int>(wanted_.passband_high);
    in.detection_width_hz = tuned_detection_bandwidth_;
    in.edge_limit_hz = receiver_edge_limit_;
    in.min_width_hz = kMinPassbandWidthHz;
    in.dragging = dragging_;

    const AutoFilterFit fit = fit_auto_filter(in, settings);
    if (fit.outcome == AutoFilterOutcome::Waiting) {
        // The edge limit or the mode's default has not come back yet. Still
        // due, and the average keeps growing meanwhile.
        return;
    }

    auto_filter_due_ = false;
    auto_filter_average_.reset();
    auto_filter_last_ = fit;
    if (fit.outcome == AutoFilterOutcome::Fitted) {
        const int from_low = static_cast<int>(wanted_.passband_low);
        const int from_high = static_cast<int>(wanted_.passband_high);

        // Written straight into the request rather than through
        // setReceiverPassband, for two reasons. That method is the operator's
        // hand: it holds AFT and marks the edges as touched, which would make
        // a later change of mode keep a fit made for the old one. And the fit
        // is already clamped to the same limits it applies.
        wanted_.passband_low = fit.low_hz;
        wanted_.passband_high = fit.high_hz;
        wanted_.bandwidth = 0;
        emit autoFilterFitted(from_low, from_high);
        post_receiver_request(false);
    }
    emit autoFilterChanged();
}

}  // namespace revenant::ui
