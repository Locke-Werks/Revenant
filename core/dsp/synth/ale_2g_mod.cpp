#include "core/dsp/synth/ale_2g_mod.h"

#include "core/decode/ale_2g.h"
#include "core/dsp/synth/mfsk_mod.h"

namespace revenant::siggen {

std::vector<int> ale_2g_tones(std::span<const std::uint32_t> words) {
    std::vector<std::uint8_t> bits;
    bits.reserve(words.size() * decode::kAleTripleBits);
    for (const std::uint32_t w : words) {
        const auto one = decode::ale_word_bits(w);
        for (int copy = 0; copy < 3; ++copy) {
            bits.insert(bits.end(), one.begin(), one.end());
        }
    }
    // 147 bits per triple is 49 whole symbols, so this never pads.
    std::vector<int> tones;
    tones.reserve(bits.size() / 3);
    for (std::size_t i = 0; i + 2 < bits.size(); i += 3) {
        const auto tri = static_cast<std::uint8_t>((bits[i] << 2U) | (bits[i + 1] << 1U) | bits[i + 2]);
        for (int t = 0; t < decode::kAleTones; ++t) {
            if (decode::kAleToneBits[static_cast<std::size_t>(t)] == tri) {
                tones.push_back(t);
                break;
            }
        }
    }
    return tones;
}

Expected<std::vector<float>> ale_2g_render(const Ale2gModConfig& config,
                                           std::span<const std::uint32_t> words) {
    MfskModConfig mc;
    mc.rate = config.rate;
    mc.tones = decode::kAleTones;
    mc.tone_spacing_hz = decode::kAleToneSpacingHz;
    mc.symbol_rate = decode::kAleSymbolRate;
    mc.base_hz = decode::kAleToneBaseHz + config.offset_hz;
    mc.amplitude = config.amplitude;
    mc.lead_in_samples = config.lead_in_samples;
    mc.tail_samples = config.tail_samples;
    const auto tones = ale_2g_tones(words);
    return mfsk_render(mc, tones);
}

}  // namespace revenant::siggen
