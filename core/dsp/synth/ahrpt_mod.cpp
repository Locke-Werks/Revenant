#include "core/dsp/synth/ahrpt_mod.h"

#include "core/dsp/synth/ccsds_downlink_mod.h"
#include "core/dsp/synth/psk_mod.h"

namespace revenant::siggen {

Expected<std::vector<dsp::Complex32>> ahrpt_baseband(const AhrptTxSpec& spec,
                                                     const std::vector<AhrptChannel>& channels) {
    std::vector<std::vector<std::uint8_t>> frames;
    std::uint32_t fill = 0;
    for (const auto& ch : channels) {
        AosPackConfig pc;
        pc.spacecraft_id = spec.spacecraft_id;
        pc.virtual_channel = ch.virtual_channel;
        const auto f = aos_pack_mpdu(pc, ch.packets);
        frames.insert(frames.end(), f.begin(), f.end());
        frames.push_back(aos_fill_frame(spec.spacecraft_id, fill++));
    }
    DownlinkTxConfig tx;
    tx.modulation = decode::PskModulation::Qpsk;
    tx.puncture = decode::ccsds_puncture_r34();
    tx.g2_inverted = false;
    tx.preamble_symbols = 4096;
    auto symbols = downlink_symbols(tx, frames);
    if (!symbols) {
        return std::unexpected(symbols.error());
    }
    PskModConfig mc;
    mc.rate = static_cast<dsp::SampleRate>(spec.sample_rate);
    mc.symbol_rate = decode::kAhrptSymbolRate;
    mc.modulation = tx.modulation;
    mc.roll_off = 0.5;
    mc.carrier_offset_hz = spec.carrier_offset_hz;
    mc.initial_phase = spec.initial_phase;
    mc.timing_offset_samples = spec.timing_offset_samples;
    mc.clock_error = spec.clock_error;
    return psk_render(mc, *symbols);
}

}  // namespace revenant::siggen
