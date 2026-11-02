#include "core/dsp/synth/mfsk16_mod.h"

#include <bit>
#include <cstdint>

namespace revenant::siggen {

std::vector<int> mfsk16_tones(const Mfsk16ModConfig& config, std::string_view text) {
    const decode::Mfsk16Geometry g = decode::mfsk16_geometry(config.mode);
    const int n = g.bits_per_symbol;

    std::vector<std::uint8_t> data;
    decode::mfsk_varicode_append(text, data);
    // Spec C.11: idle is NUL then zeros. The NUL's leading one is what
    // closes the last character of the text, and the zeros after it, which
    // the receiver rejects, also terminate the code.
    decode::mfsk_varicode_append(std::string_view("\0", 1), data);
    const int flush = config.tail_bits + decode::kMfskInterleaverStages * n * (n - 1) / 2 + 8;
    data.insert(data.end(), static_cast<std::size_t>(flush), 0);

    std::vector<std::uint8_t> coded;
    std::uint32_t reg = 0;
    for (const std::uint8_t b : data) {
        reg = ((reg << 1U) | b) & 0x7FU;
        for (const std::uint32_t gen : decode::kMfskFecGenerators) {
            coded.push_back(static_cast<std::uint8_t>(std::popcount(reg & gen) & 1));
        }
    }
    while (coded.size() % static_cast<std::size_t>(n) != 0) {
        coded.push_back(0);
    }

    std::vector<int> tones(decode::kMfskPreambleSymbols, 0);
    decode::MfskDiagonalDelay<std::uint8_t> interleave(n, false);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(n));
    for (std::size_t at = 0; at < coded.size(); at += static_cast<std::size_t>(n)) {
        interleave.push(std::span<const std::uint8_t>(coded.data() + at, static_cast<std::size_t>(n)),
                        out);
        int weight = 0;
        for (const std::uint8_t b : out) {
            weight = (weight << 1) | b;  // first arriving bit, maximum weight
        }
        tones.push_back(decode::mfsk_gray_inverse(weight));
    }
    if (config.lower_sideband) {
        for (int& t : tones) {
            t = g.tones - 1 - t;
        }
    }
    return tones;
}

Expected<std::vector<float>> mfsk16_render(const Mfsk16ModConfig& config, std::string_view text) {
    const decode::Mfsk16Geometry g = decode::mfsk16_geometry(config.mode);
    MfskModConfig mc;
    mc.rate = config.rate;
    mc.tones = g.tones;
    mc.tone_spacing_hz = g.symbol_rate;
    mc.symbol_rate = g.symbol_rate;
    mc.base_hz = config.base_hz;
    mc.amplitude = config.amplitude;
    mc.drift_hz_per_s = config.drift_hz_per_s;
    mc.lead_in_samples = config.lead_in_samples;
    mc.tail_samples = config.tail_samples;
    const std::vector<int> tones = mfsk16_tones(config, text);
    return mfsk_render(mc, tones);
}

}  // namespace revenant::siggen
