#include "core/dsp/front_end_correction.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::dsp {
namespace {

// The averaging weight a block of n samples carries at time constant tau.
[[nodiscard]] double weight_of(std::uint64_t samples, double tau_s, SampleRate rate) {
    if (tau_s <= 0.0 || rate <= 0) {
        return 1.0;
    }
    return 1.0 - std::exp(-static_cast<double>(samples) / (tau_s * static_cast<double>(rate)));
}

}  // namespace

double FrontEndEstimate::dc_dbfs() const {
    const double power = dc_i * dc_i + dc_q * dc_q;
    return power > 0.0 ? 10.0 * std::log10(power) : -300.0;
}

double FrontEndEstimate::phase_error_deg() const {
    return std::asin(std::clamp(sin_phase, -1.0, 1.0)) * 180.0 / std::numbers::pi;
}

double FrontEndEstimate::image_rejection_db() const {
    const double cos_phase = std::sqrt(std::max(0.0, 1.0 - sin_phase * sin_phase));
    const double wanted = 1.0 + 2.0 * gain * cos_phase + gain * gain;
    const double image = 1.0 - 2.0 * gain * cos_phase + gain * gain;
    if (image <= 0.0) {
        return 300.0;
    }
    return 10.0 * std::log10(wanted / image);
}

IqMoments total_moments(ConstRealSpan chunk_sums, std::uint32_t count) {
    IqMoments out;
    out.samples = count;
    const std::size_t chunks = std::min<std::size_t>(iq_moments_chunks(count),
                                                     chunk_sums.size() / kIqMomentsPerChunk);
    for (std::size_t c = 0; c < chunks; ++c) {
        const std::size_t at = c * kIqMomentsPerChunk;
        out.sum_i += chunk_sums[at + 0];
        out.sum_q += chunk_sums[at + 1];
        out.sum_ii += chunk_sums[at + 2];
        out.sum_qq += chunk_sums[at + 3];
        out.sum_iq += chunk_sums[at + 4];
    }
    return out;
}

FrontEndCorrector::FrontEndCorrector(FrontEndCorrectionConfig config) : config_(config) {}

void FrontEndCorrector::reset() {
    estimate_ = FrontEndEstimate{};
    m_ii_ = 0.0;
    m_qq_ = 0.0;
    m_iq_ = 0.0;
}

void FrontEndCorrector::update(const IqMoments& moments, SampleRate rate) {
    if (moments.samples == 0) {
        return;
    }
    const double n = static_cast<double>(moments.samples);
    const double mean_i = moments.sum_i / n;
    const double mean_q = moments.sum_q / n;

    // About the block's own mean, so the imbalance estimate does not depend on
    // whether the DC estimate has converged yet.
    const double c_ii = moments.sum_ii / n - mean_i * mean_i;
    const double c_qq = moments.sum_qq / n - mean_q * mean_q;
    const double c_iq = moments.sum_iq / n - mean_i * mean_q;

    const bool first = !estimate_.measured;
    const double a_dc = first ? 1.0 : weight_of(moments.samples, config_.dc_time_constant_s, rate);
    const double a_iq = first ? 1.0 : weight_of(moments.samples, config_.iq_time_constant_s, rate);

    estimate_.dc_i += a_dc * (mean_i - estimate_.dc_i);
    estimate_.dc_q += a_dc * (mean_q - estimate_.dc_q);
    m_ii_ += a_iq * (c_ii - m_ii_);
    m_qq_ += a_iq * (c_qq - m_qq_);
    m_iq_ += a_iq * (c_iq - m_iq_);

    estimate_.measured = true;
    estimate_.samples_seen += moments.samples;

    if (m_ii_ > 0.0 && m_qq_ > 0.0) {
        estimate_.gain = std::sqrt(m_qq_ / m_ii_);
        estimate_.sin_phase = m_iq_ / std::sqrt(m_ii_ * m_qq_);
        const double gain_db = std::abs(20.0 * std::log10(estimate_.gain));
        const double phase_deg = std::abs(estimate_.phase_error_deg());
        estimate_.iq_plausible = std::isfinite(estimate_.gain) &&
                                 std::isfinite(estimate_.sin_phase) &&
                                 gain_db <= config_.max_gain_error_db &&
                                 phase_deg <= config_.max_phase_error_deg;
    } else {
        estimate_.gain = 1.0;
        estimate_.sin_phase = 0.0;
        estimate_.iq_plausible = false;
    }
}

IqCorrectParams FrontEndCorrector::correction(bool dc, bool iq) const {
    IqCorrectParams out;
    if (!estimate_.measured) {
        return out;
    }
    if (dc) {
        out.dc_i = static_cast<float>(estimate_.dc_i);
        out.dc_q = static_cast<float>(estimate_.dc_q);
    }
    if (iq && estimate_.iq_plausible) {
        const double cos_phase = std::sqrt(1.0 - estimate_.sin_phase * estimate_.sin_phase);
        out.cross = static_cast<float>(-estimate_.sin_phase / cos_phase);
        out.scale = static_cast<float>(1.0 / (estimate_.gain * cos_phase));
    }
    return out;
}

}  // namespace revenant::dsp
