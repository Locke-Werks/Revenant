#include "core/dsp/synth/fsq_mod.h"

namespace revenant::siggen {

std::vector<int> fsq_symbol_stream(const FsqModConfig& config, std::string_view text) {
    std::vector<int> out;
    const auto idle = decode::fsq_symbols(0);
    for (int i = 0; i < config.idle_before; ++i) {
        out.insert(out.end(), idle.begin(), idle.end());
    }
    for (const char c : text) {
        const auto s = decode::fsq_symbols(static_cast<std::uint8_t>(c));
        out.insert(out.end(), s.begin(), s.end());
    }
    for (int i = 0; i < config.idle_after; ++i) {
        out.insert(out.end(), idle.begin(), idle.end());
    }
    return out;
}

Expected<std::vector<float>> fsq_render(const FsqModConfig& config, std::string_view text) {
    const auto symbols = fsq_symbol_stream(config, text);
    std::vector<int> tones =
        decode::ifk_encode(symbols, decode::kFsqTones, decode::kFsqIfkOffset, 0);
    tones.insert(tones.begin(), 0);
    if (config.lower_sideband) {
        for (int& t : tones) {
            t = decode::kFsqTones - 1 - t;
        }
    }
    MfskModConfig mc;
    mc.rate = config.rate;
    mc.tones = decode::kFsqTones;
    mc.tone_spacing_hz = decode::kFsqToneSpacingHz;
    mc.symbol_rate = config.symbol_rate;
    mc.base_hz = config.base_hz;
    mc.amplitude = config.amplitude;
    mc.drift_hz_per_s = config.drift_hz_per_s;
    mc.lead_in_samples = config.lead_in_samples;
    mc.tail_samples = config.tail_samples;
    return mfsk_render(mc, tones);
}

}  // namespace revenant::siggen
