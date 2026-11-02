#include "core/dsp/synth/dominoex_mod.h"

namespace revenant::siggen {

std::vector<int> domino_nibble_stream(const DominoModConfig& config, std::string_view text) {
    std::vector<int> out;
    auto add = [&out](std::string_view s, bool secondary) {
        for (const char c : s) {
            for (const auto n : decode::domino_nibbles(static_cast<std::uint8_t>(c), secondary)) {
                out.push_back(n);
            }
        }
    };
    add(config.idle_before, true);
    add(text, false);
    add(config.idle_after, true);
    return out;
}

Expected<std::vector<float>> domino_render(const DominoModConfig& config, std::string_view text) {
    const decode::DominoGeometry g = decode::domino_geometry(config.speed);
    const std::vector<int> nibbles = domino_nibble_stream(config, text);
    std::vector<int> tones = decode::ifk_encode(nibbles, decode::kDominoTones,
                                                decode::kDominoIfkOffset, 0);
    tones.insert(tones.begin(), 0);
    if (config.lower_sideband) {
        for (int& t : tones) {
            t = decode::kDominoTones - 1 - t;
        }
    }
    MfskModConfig mc;
    mc.rate = config.rate;
    mc.tones = decode::kDominoTones;
    mc.tone_spacing_hz = g.tone_spacing_hz;
    mc.symbol_rate = g.symbol_rate;
    mc.base_hz = config.base_hz;
    mc.amplitude = config.amplitude;
    mc.drift_hz_per_s = config.drift_hz_per_s;
    mc.lead_in_samples = config.lead_in_samples;
    mc.tail_samples = config.tail_samples;
    return mfsk_render(mc, tones);
}

}  // namespace revenant::siggen
