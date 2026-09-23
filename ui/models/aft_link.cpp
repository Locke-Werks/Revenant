// EngineLink's side of automatic frequency tracking: feeding the loop in
// models/aft.h one passband frame at a time, and moving the receiver when it
// says to. The rules are all in that header; this file is plumbing.

#include "models/engine_link.h"

#include <cmath>
#include <string_view>

namespace revenant::ui {

namespace {

[[nodiscard]] double seconds(const QElapsedTimer& clock)
{
    return static_cast<double>(clock.elapsed()) / 1000.0;
}

}  // namespace

void EngineLink::setAftEnabled(bool on)
{
    if (on == aft_.enabled()) {
        return;
    }
    aft_.set_enabled(on);
    aft_step_ = {};
    aft_step_.state = on ? AftState::NoSignal : AftState::Off;
    emit aftChanged();
}

bool EngineLink::aftOffered() const
{
    return aft_rule_for(receiverDemod().toStdString()) != AftRule::Hold;
}

QString EngineLink::aftState() const
{
    const std::string_view label = aft_state_label(aft_step_.state);
    return QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
}

void EngineLink::aft_yield()
{
    // Emitted even when off: a change of mode arrives here too, and
    // aftOffered follows the mode.
    if (!aft_.enabled()) {
        emit aftChanged();
        return;
    }
    aft_.operator_tuned(seconds(scroll_clock_));
    aft_step_ = {};
    aft_step_.state = AftState::Yielding;
    emit aftChanged();
}

void EngineLink::run_aft()
{
    if (!aft_.enabled()) {
        return;
    }
    const rpc::PassbandFrame& frame = passband_display_;
    if (frame.power_db.empty()) {
        return;
    }
    if (frame.vrx != aft_vrx_) {
        aft_vrx_ = frame.vrx;
        aft_.forget();
    }

    // The frame's own axis, in absolute hertz: bin centres sit half a bin in
    // from the outer edges passbandFrequencyAtFraction reports. The bin width
    // is the geometry's, which is the display rate over the transform, rather
    // than anything derived from the receiver's demodulation rate.
    const rpc::PassbandGeometry& geometry = frame.geometry;
    const double low = passbandFrequencyAtFraction(0.0);
    const double bin_hz = geometry.bin_width.denominator == 0
                              ? 0.0
                              : static_cast<double>(geometry.bin_width.numerator) /
                                    static_cast<double>(geometry.bin_width.denominator);
    const double centre = receiverCenterHz();

    AftFrame measured;
    measured.power_db = frame.power_db;
    measured.first_bin_hz = low + 0.5 * bin_hz;
    measured.bin_hz = bin_hz;

    // The frame's own percentile, not the smoothed floor: see models/aft.h.
    measured.percentile_low_db = frame.percentile_low_db;
    measured.window_s = geometry.rate == 0 ? 0.0
                                           : static_cast<double>(geometry.transform) /
                                                 static_cast<double>(geometry.rate);

    // The filter the engine granted, which is what the audio passes. Before
    // the first grant the edges asked for, and with neither the whole frame.
    int edge_low = receiverGrantedLow();
    int edge_high = receiverGrantedHigh();
    if (edge_high <= edge_low) {
        edge_low = receiverPassbandLow();
        edge_high = receiverPassbandHigh();
    }
    if (edge_high > edge_low) {
        measured.search_low_hz = centre + static_cast<double>(edge_low);
        measured.search_high_hz = centre + static_cast<double>(edge_high);
    } else {
        measured.search_low_hz = low;
        measured.search_high_hz = passbandFrequencyAtFraction(1.0);
    }

    const AftStep step = aft_.step(seconds(scroll_clock_), centre,
                                   aft_rule_for(receiverDemod().toStdString()), measured);
    // Compared to the hertz, so a readout of the error is not repainted for
    // the measurement's own sub-hertz jitter thirty times a second.
    const bool changed = step.state != aft_step_.state || step.have_error != aft_step_.have_error ||
                         std::lround(step.error_hz) != std::lround(aft_step_.error_hz);
    aft_step_ = step;
    if (step.move) {
        moveReceiverCentre(step.centre_hz);
    }
    if (changed) {
        emit aftChanged();
    }
}

}  // namespace revenant::ui
