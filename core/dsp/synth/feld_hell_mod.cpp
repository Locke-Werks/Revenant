#include "core/dsp/synth/feld_hell_mod.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::siggen {

Expected<std::vector<float>> feld_hell_render(const FeldHellModConfig& config,
                                              std::span<const std::uint16_t> columns) {
    if (config.rate <= 0 || config.carrier_hz <= 0.0 || config.carrier_hz >= config.rate / 2.0) {
        return fail("Feld-Hell modulator: carrier outside the audio band");
    }
    const double sps = static_cast<double>(config.rate) / decode::kFeldHellSlotRate;
    const std::size_t slots = columns.size() * decode::kFeldHellHalfPixelsPerColumn;
    const auto body = static_cast<std::size_t>(std::ceil(static_cast<double>(slots) * sps));
    const auto lead = static_cast<std::size_t>(std::floor(config.lead_in_samples));
    const double frac = config.lead_in_samples - static_cast<double>(lead);
    const auto total = lead + body + static_cast<std::size_t>(std::ceil(config.tail_samples)) + 1;

    // Key state per sample of the body, then smoothed by a raised-cosine
    // window one half-pixel slot long, normalised to unit area, which gives every
    // edge a raised-cosine ramp of that length.
    std::vector<double> key(total, 0.0);
    for (std::size_t s = 0; s < slots; ++s) {
        const std::size_t col = s / decode::kFeldHellHalfPixelsPerColumn;
        const std::size_t bit = s % decode::kFeldHellHalfPixelsPerColumn;
        if (((columns[col] >> bit) & 1U) == 0) {
            continue;
        }
        const double from = frac + static_cast<double>(s) * sps;
        const double to = frac + static_cast<double>(s + 1) * sps;
        for (auto i = static_cast<std::size_t>(std::ceil(from)); static_cast<double>(i) < to; ++i) {
            if (lead + i < total) {
                key[lead + i] = 1.0;
            }
        }
    }
    const auto width = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(sps)));
    std::vector<double> window(width);
    double area = 0.0;
    for (std::size_t i = 0; i < width; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * (static_cast<double>(i) + 0.5) /
                                         static_cast<double>(width));
        area += window[i];
    }
    std::vector<float> out(total, 0.0F);
    const std::size_t half = width / 2;
    for (std::size_t n = 0; n < total; ++n) {
        double env = 0.0;
        for (std::size_t i = 0; i < width; ++i) {
            const std::size_t at = n + half;
            if (at >= i && at - i < total) {
                env += key[at - i] * window[i];
            }
        }
        env /= area;
        const double phase = 2.0 * std::numbers::pi * config.carrier_hz * static_cast<double>(n) /
                             static_cast<double>(config.rate);
        out[n] = static_cast<float>(config.amplitude * env * std::sin(phase));
    }
    return out;
}

}  // namespace revenant::siggen
