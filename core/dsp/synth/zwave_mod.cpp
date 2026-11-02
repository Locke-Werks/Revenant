#include "core/dsp/synth/zwave_mod.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::siggen {

using decode::ZWaveRate;

Expected<std::vector<Complex32>> zwave_render(const ZWaveModConfig& config,
                                              std::span<const std::uint8_t> mpdu) {
    if (config.sample_rate <= 0) {
        return fail("Z-Wave modulator sample rate must be positive");
    }
    const double fs = static_cast<double>(config.sample_rate);
    const double symbol_rate = decode::zwave_symbol_rate(config.rate);
    if (fs < 2.0 * symbol_rate) {
        return fail("Z-Wave modulator sample rate too low");
    }
    const std::size_t preamble =
        config.preamble_bytes != 0 ? config.preamble_bytes
        : config.rate == ZWaveRate::R3 ? decode::kZWavePreambleBytesR3
                                       : decode::kZWavePreambleBytesR1R2;

    std::vector<std::uint8_t> bytes(preamble, decode::kZWavePreambleByte);
    bytes.push_back(decode::kZWaveSof);
    bytes.insert(bytes.end(), mpdu.begin(), mpdu.end());

    // Symbol levels, +1 the upper tone. Table 7-5 for NRZ (0 upper), Table 7-6
    // for Manchester (1 upper then lower). Bits most significant first, 7.1.3.
    std::vector<float> levels;
    for (const std::uint8_t byte : bytes) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool one = ((byte >> bit) & 1U) != 0U;
            if (config.rate == ZWaveRate::R1) {
                levels.push_back(one ? 1.0F : -1.0F);
                levels.push_back(one ? -1.0F : 1.0F);
            } else {
                levels.push_back(one ? -1.0F : 1.0F);
            }
        }
    }
    if (config.rate == ZWaveRate::R1) {
        // 7.1.3.5: eight violations, each a whole bit with no transition.
        const float hold = levels.back();
        levels.insert(levels.end(), 16, hold);
    }

    const double sps = fs / symbol_rate;
    const auto count = static_cast<std::size_t>(std::ceil(static_cast<double>(levels.size()) * sps));
    std::vector<double> shaped(count);
    for (std::size_t n = 0; n < count; ++n) {
        const auto symbol = std::min(levels.size() - 1,
                                     static_cast<std::size_t>(static_cast<double>(n) / sps));
        shaped[n] = levels[symbol];
    }
    if (config.rate == ZWaveRate::R3) {
        // Gaussian frequency pulse, standard deviation sqrt(ln 2) / (2 pi BT)
        // symbols for the Table 7-4 BT of 0.6.
        const double sigma = std::sqrt(std::log(2.0)) /
                             (2.0 * std::numbers::pi * decode::kZWaveGaussianBt) * sps;
        const auto half = static_cast<std::ptrdiff_t>(std::ceil(3.0 * sigma));
        std::vector<double> kernel;
        double sum = 0.0;
        for (std::ptrdiff_t k = -half; k <= half; ++k) {
            const double v = std::exp(-0.5 * static_cast<double>(k * k) / (sigma * sigma));
            kernel.push_back(v);
            sum += v;
        }
        std::vector<double> out(count);
        for (std::size_t n = 0; n < count; ++n) {
            double acc = 0.0;
            for (std::ptrdiff_t k = -half; k <= half; ++k) {
                auto idx = static_cast<std::ptrdiff_t>(n) + k;
                idx = std::clamp<std::ptrdiff_t>(idx, 0, static_cast<std::ptrdiff_t>(count) - 1);
                acc += kernel[static_cast<std::size_t>(k + half)] *
                       shaped[static_cast<std::size_t>(idx)];
            }
            out[n] = acc / sum;
        }
        shaped = std::move(out);
    }

    const double deviation = decode::zwave_separation_hz(config.rate) / 2.0;
    const double carrier =
        config.channel_offset_hz + decode::zwave_frequency_offset_hz(config.rate);
    std::vector<Complex32> samples(count);
    double phase = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
        samples[n] = std::polar(config.amplitude, static_cast<float>(phase));
        phase += 2.0 * std::numbers::pi * (carrier + deviation * shaped[n]) / fs;
        phase = std::remainder(phase, 2.0 * std::numbers::pi);
    }
    return samples;
}

}  // namespace revenant::siggen
