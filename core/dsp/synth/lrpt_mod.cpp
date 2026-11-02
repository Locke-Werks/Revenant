#include "core/dsp/synth/lrpt_mod.h"

#include "core/dsp/synth/ccsds_downlink_mod.h"
#include "core/dsp/synth/psk_mod.h"

namespace revenant::siggen {

std::vector<std::vector<std::uint8_t>> lrpt_frames(const LrptTxSpec& spec, const std::vector<LrptChannel>& channels) {
    std::vector<std::vector<std::uint8_t>> frames;
    std::uint32_t fill_count = 0;
    for (const auto& ch : channels) {
        AosPackConfig pc;
        pc.spacecraft_id = spec.spacecraft_id;
        pc.virtual_channel = ch.virtual_channel;
        const auto f = aos_pack_mpdu(pc, ch.packets);
        frames.insert(frames.end(), f.begin(), f.end());
        for (std::size_t i = 0; i < spec.fill_frames; ++i) {
            frames.push_back(aos_fill_frame(spec.spacecraft_id, fill_count++));
        }
    }
    return frames;
}

Expected<std::vector<dsp::Complex32>> lrpt_baseband(const LrptTxSpec& spec, const std::vector<LrptChannel>& channels) {
    const auto frames = lrpt_frames(spec, channels);
    DownlinkTxConfig tx;
    tx.modulation = spec.oqpsk ? decode::PskModulation::Oqpsk : decode::PskModulation::Qpsk;
    tx.g2_inverted = true;
    auto symbols = downlink_symbols(tx, frames);
    if (!symbols) {
        return std::unexpected(symbols.error());
    }
    PskModConfig mc;
    mc.rate = static_cast<dsp::SampleRate>(spec.sample_rate);
    mc.symbol_rate = spec.symbol_rate;
    mc.modulation = tx.modulation;
    mc.roll_off = 0.6;
    mc.carrier_offset_hz = spec.carrier_offset_hz;
    mc.initial_phase = spec.initial_phase;
    mc.timing_offset_samples = spec.timing_offset_samples;
    mc.clock_error = spec.clock_error;
    return psk_render(mc, *symbols);
}

}  // namespace revenant::siggen
