#include "core/dsp/synth/vdl4_mod.h"

#include "core/dsp/synth/ais_mod.h"

namespace revenant::siggen {

Expected<std::vector<dsp::Complex32>> vdl4_render(
    const Vdl4ModConfig& config, std::span<const std::vector<std::uint8_t>> bursts) {
    std::vector<std::vector<std::uint8_t>> bits;
    bits.reserve(bursts.size());
    for (const auto& b : bursts) {
        bits.push_back(ais_packet_bits(b));
    }
    AisModConfig mod;
    mod.rate = config.rate;
    mod.amplitude = config.amplitude;
    mod.bit_rate = decode::kVdl4BitRate;
    mod.bandwidth_time = decode::kVdl4Bt;
    mod.deviation_hz = decode::kVdl4DeviationHz;
    mod.bit_rate_error = config.bit_rate_error;
    mod.carrier_offset_hz = config.carrier_offset_hz;
    return ais_render_packets(mod, bits);
}

}  // namespace revenant::siggen
