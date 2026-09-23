#include "core/source/corrected_source.h"

#include <algorithm>
#include <format>
#include <utility>

#include "core/source/frequency_correction.h"

namespace revenant::source {

CorrectedSource::CorrectedSource(std::unique_ptr<Source> inner, std::int64_t ppb)
    : inner_(std::move(inner)),
      ppb_(std::clamp(ppb, -kMaxCorrectionPpb, kMaxCorrectionPpb)) {}

Status CorrectedSource::set_correction_ppb(std::int64_t ppb) {
    if (!correction_in_range(ppb)) {
        return fail(std::format(
            "a correction of {} ppb is past the {} ppb either way that a crystal error can "
            "be. A figure that large is a different radio, or a carrier that was not the one "
            "it was taken for.",
            ppb, kMaxCorrectionPpb));
    }
    ppb_.store(ppb, std::memory_order_release);
    return {};
}

Expected<dsp::Hertz> CorrectedSource::tune(dsp::Hertz center) {
    // Read once, so the request and the answer are converted with the same
    // correction even if one is being set on another thread.
    const std::int64_t ppb = ppb_.load(std::memory_order_acquire);
    auto landed = inner_->tune(true_to_device(center, ppb));
    if (!landed) {
        return std::unexpected(landed.error());
    }
    return device_to_true(*landed, ppb);
}

dsp::Hertz CorrectedSource::center() const {
    return device_to_true(inner_->center(), ppb_.load(std::memory_order_acquire));
}

ClockQuality CorrectedSource::clock() const {
    ClockQuality quality = inner_->clock();
    const std::int64_t ppb = ppb_.load(std::memory_order_acquire);
    if (quality.ppm_error == 0.0 && ppb != 0) {
        quality.ppm_error = static_cast<double>(ppb) / 1000.0;
    }
    return quality;
}

}  // namespace revenant::source
