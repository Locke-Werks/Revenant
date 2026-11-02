#include "core/dsp/synth/mfsk_mod.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace revenant::siggen {

Expected<std::vector<float>> mfsk_render(const MfskModConfig& config,
                                         std::span<const int> tone_sequence) {
    if (config.rate <= 0 || !(config.symbol_rate > 0.0) || !(config.tone_spacing_hz > 0.0)) {
        return fail("MFSK modulator: rate, symbol rate and tone spacing must be positive");
    }
    if (config.tones < 2 || config.tones > 256) {
        return fail("MFSK modulator: tone count " + std::to_string(config.tones) +
                    " is outside 2 to 256");
    }
    if (config.gaussian_bt < 0.0 || config.lead_in_samples < 0.0 || config.tail_samples < 0.0) {
        return fail("MFSK modulator: BT, lead-in and tail must not be negative");
    }
    for (const int t : tone_sequence) {
        if (t < 0 || t >= config.tones) {
            return fail("MFSK modulator: tone index " + std::to_string(t) + " out of range");
        }
    }

    const double rate = static_cast<double>(config.rate);
    const double period = rate / config.symbol_rate;
    const double body = period * static_cast<double>(tone_sequence.size());
    const auto total = static_cast<std::size_t>(
        std::ceil(config.lead_in_samples + body + config.tail_samples));
    std::vector<float> out(total, 0.0F);
    if (tone_sequence.empty()) {
        return out;
    }

    // Instantaneous frequency per sample, as a tone index, held at the first
    // and last tone outside the body so the filter has nothing to ring on.
    std::vector<double> index(total);
    for (std::size_t n = 0; n < total; ++n) {
        const double t = (static_cast<double>(n) - config.lead_in_samples) / period;
        const auto s = static_cast<std::ptrdiff_t>(std::floor(t));
        const auto clamped = std::clamp<std::ptrdiff_t>(
            s, 0, static_cast<std::ptrdiff_t>(tone_sequence.size()) - 1);
        index[n] = tone_sequence[static_cast<std::size_t>(clamped)];
    }

    if (config.gaussian_bt > 0.0) {
        // A Gaussian with -3 dB bandwidth B has sigma = sqrt(ln 2) / (2 pi B)
        // seconds; with B = BT / T that is in symbol periods.
        const double sigma =
            std::sqrt(std::numbers::ln2) / (2.0 * std::numbers::pi * config.gaussian_bt) * period;
        const auto half = static_cast<std::ptrdiff_t>(std::ceil(3.0 * sigma));
        std::vector<double> kernel(static_cast<std::size_t>(2 * half + 1));
        double sum = 0.0;
        for (std::ptrdiff_t k = -half; k <= half; ++k) {
            const double v = std::exp(-0.5 * (static_cast<double>(k) * static_cast<double>(k)) /
                                      (sigma * sigma));
            kernel[static_cast<std::size_t>(k + half)] = v;
            sum += v;
        }
        std::vector<double> smoothed(total);
        const auto last = static_cast<std::ptrdiff_t>(total) - 1;
        for (std::ptrdiff_t n = 0; n <= last; ++n) {
            double acc = 0.0;
            for (std::ptrdiff_t k = -half; k <= half; ++k) {
                const auto m = std::clamp<std::ptrdiff_t>(n + k, 0, last);
                acc += kernel[static_cast<std::size_t>(k + half)] * index[static_cast<std::size_t>(m)];
            }
            smoothed[static_cast<std::size_t>(n)] = acc / sum;
        }
        index.swap(smoothed);
    }

    double phase = 0.0;
    for (std::size_t n = 0; n < total; ++n) {
        const double elapsed = std::max(0.0, static_cast<double>(n) - config.lead_in_samples) / rate;
        const double hz = config.base_hz + config.drift_hz_per_s * elapsed +
                          config.tone_spacing_hz * index[n];
        const double t = static_cast<double>(n) - config.lead_in_samples;
        if (t >= 0.0 && t < body) {
            out[n] = static_cast<float>(config.amplitude * std::cos(phase));
        }
        phase += 2.0 * std::numbers::pi * hz / rate;
        if (phase > 2.0 * std::numbers::pi) {
            phase -= 2.0 * std::numbers::pi;
        }
    }
    return out;
}

}  // namespace revenant::siggen
