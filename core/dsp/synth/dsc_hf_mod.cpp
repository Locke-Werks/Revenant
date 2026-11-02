#include "core/dsp/synth/dsc_hf_mod.h"

#include "core/dsp/synth/dsc_mod.h"

namespace revenant::siggen {

std::vector<std::uint8_t> dsc_hf_bits(std::span<const std::uint8_t> information,
                                      std::size_t dot_bits) {
    std::vector<std::uint8_t> bits;
    // Clause 3.4: "alternating B-Y or Y-B".
    for (std::size_t i = 0; i < dot_bits; ++i) {
        bits.push_back(static_cast<std::uint8_t>(i % 2));
    }
    for (const std::uint8_t symbol : dsc_air_symbols(information)) {
        const auto code = decode::dsc_encode_symbol(symbol);
        bits.insert(bits.end(), code.begin(), code.end());
    }
    return bits;
}

Expected<std::vector<float>> dsc_hf_render_audio(
    const DscHfModConfig& config, std::span<const std::vector<std::uint8_t>> calls_bits) {
    const decode::DscConfig tones = decode::dsc_hf_config(config.rate, config.sideband);
    DscModConfig m;
    m.rate = config.rate;
    m.y_hz = tones.y_hz;
    m.b_hz = tones.b_hz;
    m.bit_rate = tones.bit_rate;
    m.amplitude = config.amplitude;
    // J2B audio: no pre-emphasis, clause 1.3.2 is VHF only.
    m.preemphasis = false;
    m.lead_bits = config.lead_bits;
    m.gap_bits = config.gap_bits;
    m.tail_bits = config.tail_bits;
    m.bit_rate_error = config.bit_rate_error;
    return dsc_render_audio(m, calls_bits);
}

}  // namespace revenant::siggen
