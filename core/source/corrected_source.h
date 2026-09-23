// Any source, with its crystal's frequency error taken out of the numbers.
//
// WHERE THE CORRECTION IS APPLIED, AND WHY NOT IN THE DEVICE. There were two
// places to put it. librtlsdr offers rtlsdr_set_freq_correction, which moves
// the tuner's synthesiser and the RTL2832U's resampler together, and the URI
// already reaches it as ppm=. It takes WHOLE ppm: one step is 100 Hz at
// 100 MHz and 1.7 kHz at the top of an R820T's range, so a dongle 0.4 ppm off
// cannot be corrected at all and one 1.6 ppm off is left 0.4 ppm off. It is
// also an RTL-SDR call, so a recording made by an uncalibrated dongle could
// not be corrected, and changing it on a streaming dongle is another control
// transfer through the I2C repeater that stalls, which costs a third of a
// second of samples every time an operator nudges it.
//
// So the engine corrects its own arithmetic instead, in parts per billion and
// integer hertz, and tells the device nothing. This wrapper is the whole of
// that: tune() asks the device for the frequency that lands the real
// oscillator on the one requested, and center() reports where the real
// oscillator is. EngineInfo::source_center reads center(), and every absolute
// frequency the engine publishes, a detection's centre and a receiver's place
// among them, is that centre plus a baseband offset.
//
// WHAT IT DOES NOT CORRECT, which is the cost of not using the device's call.
// The baseband offsets are counted on the device's sample clock, which runs
// off the same crystal and is just as wrong. A carrier at offset d from the
// centre is really at d * (1 + e), so after correction its label is still off
// by d * e: at 1 ppm and the edge of a 2.4 MS/s span, 1.2 Hz; at 20 ppm, 24
// Hz. The centre's error at 100 MHz is 100 Hz per ppm, so the centre is the
// term worth correcting and this corrects it exactly. Correcting the offsets
// too means scaling the grid's rational axis and every receiver's placement,
// which is the engine's frame rather than the source's, and it is recorded as
// the open item in docs/calibration.md rather than half done here.
//
// A CHANGE OF CORRECTION MOVES THE LABELS AND NOT THE RADIO. set_correction
// does not retune, so the device keeps listening where it was, every receiver
// stays on the signal it was on, and the numbers the engine reports move to
// what they should have been. A retune after that asks for the corrected
// frequency.
//
// THE TUNE RANGES ARE THE DEVICE'S, NOT CORRECTED. They are an envelope, not
// a promise (engine::SourceTuning says the same), and a thousand ppm at the
// top of an R820T's range is the only place the difference is more than a
// rounding error. capabilities() is a reference handed out once, so it cannot
// follow a correction that changes while it is held.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/source.h"

namespace revenant::source {

class CorrectedSource final : public Source {
public:
    // `ppb` must be inside correction_in_range; the engine checks before it
    // gets here and a value outside it is clamped rather than trusted.
    CorrectedSource(std::unique_ptr<Source> inner, std::int64_t ppb);

    // Applies from the next call that reads it. No retune; see the header.
    [[nodiscard]] Status set_correction_ppb(std::int64_t ppb);
    [[nodiscard]] std::int64_t correction_ppb() const {
        return ppb_.load(std::memory_order_acquire);
    }

    // Where the device itself is tuned, on its own crystal's scale. For a
    // message that has to say both numbers.
    [[nodiscard]] dsp::Hertz device_center() const { return inner_->center(); }

    [[nodiscard]] const SourceCapabilities& capabilities() const override {
        return inner_->capabilities();
    }

    [[nodiscard]] Expected<dsp::Hertz> tune(dsp::Hertz center) override;
    [[nodiscard]] dsp::Hertz center() const override;

    [[nodiscard]] Expected<dsp::SampleRate> set_sample_rate(dsp::SampleRate rate) override {
        return inner_->set_sample_rate(rate);
    }
    [[nodiscard]] dsp::SampleRate sample_rate() const override { return inner_->sample_rate(); }
    [[nodiscard]] Expected<double> set_gain(std::string_view stage, double db) override {
        return inner_->set_gain(stage, db);
    }
    [[nodiscard]] Status set_gain_auto(std::string_view stage, bool on) override {
        return inner_->set_gain_auto(stage, on);
    }
    [[nodiscard]] Status start(const StreamOptions& options, BlockSink sink) override {
        return inner_->start(options, std::move(sink));
    }
    [[nodiscard]] Status stop() override { return inner_->stop(); }
    [[nodiscard]] bool running() const override { return inner_->running(); }
    [[nodiscard]] Status seek(dsp::SampleIndex index) override { return inner_->seek(index); }
    [[nodiscard]] SourceStats stats() const override { return inner_->stats(); }

    // The inner source's, with ppm_error filled from the correction when the
    // source had nothing better to say. A measured correction is a statement
    // about the sample clock as much as the tuner: both run off the crystal.
    [[nodiscard]] ClockQuality clock() const override;

private:
    std::unique_ptr<Source> inner_;
    std::atomic<std::int64_t> ppb_{0};
};

}  // namespace revenant::source
