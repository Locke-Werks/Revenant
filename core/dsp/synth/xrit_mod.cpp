#include "core/dsp/synth/xrit_mod.h"

#include <algorithm>
#include <array>

#include "core/dsp/synth/ccsds_downlink_mod.h"
#include "core/dsp/synth/ccsds_mod.h"
#include "core/dsp/synth/psk_mod.h"

namespace revenant::siggen {

namespace {

void put(std::vector<std::uint8_t>& v, std::uint64_t x, std::size_t n) {
    for (std::size_t i = n; i-- > 0;) {
        v.push_back(static_cast<std::uint8_t>(x >> (8U * i)));
    }
}

}  // namespace

std::vector<std::uint8_t> xrit_file_bytes(std::uint8_t file_type, const std::vector<decode::XritHeaderRecord>& records,
                                          const std::vector<std::uint8_t>& data) {
    std::size_t total = 16;
    for (const auto& r : records) {
        total += 3 + r.data.size();
    }
    std::vector<std::uint8_t> f;
    f.push_back(0);
    put(f, 16, 2);
    f.push_back(file_type);
    put(f, total, 4);
    put(f, static_cast<std::uint64_t>(data.size()) * 8U, 8);
    for (const auto& r : records) {
        f.push_back(r.type);
        put(f, 3 + r.data.size(), 2);
        f.insert(f.end(), r.data.begin(), r.data.end());
    }
    f.insert(f.end(), data.begin(), data.end());
    return f;
}

std::vector<std::vector<std::uint8_t>> xrit_packets(std::uint16_t apid, std::uint16_t file_counter,
                                                    const std::vector<std::uint8_t>& s_pdu, std::uint16_t first_count) {
    std::vector<std::uint8_t> tp;
    put(tp, file_counter, 2);
    put(tp, static_cast<std::uint64_t>(s_pdu.size()) * 8U, 8);
    tp.insert(tp.end(), s_pdu.begin(), s_pdu.end());
    std::vector<std::vector<std::uint8_t>> out;
    const std::size_t blocks = (tp.size() + decode::kXritBlockOctets - 1) / decode::kXritBlockOctets;
    for (std::size_t i = 0; i < blocks; ++i) {
        const std::size_t off = i * decode::kXritBlockOctets;
        const std::size_t n = std::min(decode::kXritBlockOctets, tp.size() - off);
        std::vector<std::uint8_t> seg(tp.begin() + static_cast<std::ptrdiff_t>(off),
                                      tp.begin() + static_cast<std::ptrdiff_t>(off + n));
        const auto crc = decode::ccsds_fecf(seg);
        put(seg, crc, 2);
        decode::SpacePacketHeader h;
        h.apid = apid;
        h.secondary_header = i == 0;
        h.sequence_flags = blocks == 1 ? 3 : (i == 0 ? 1 : (i + 1 == blocks ? 2 : 0));
        h.sequence_count = static_cast<std::uint16_t>((first_count + i) & 0x3FFFU);
        out.push_back(ccsds_space_packet(h, seg));
    }
    return out;
}

Expected<std::vector<dsp::Complex32>> xrit_bpsk_baseband(const XritBpskSpec& spec, const std::vector<XritSend>& sends) {
    std::vector<std::vector<std::uint8_t>> frames;
    std::uint32_t fill = 0;
    std::array<std::uint32_t, 64> counts{};
    for (const auto& s : sends) {
        AosPackConfig pc;
        pc.spacecraft_id = spec.spacecraft_id;
        pc.virtual_channel = s.virtual_channel;
        pc.first_count = counts[s.virtual_channel];
        const auto f = aos_pack_mpdu(pc, s.packets);
        counts[s.virtual_channel] += static_cast<std::uint32_t>(f.size());
        frames.insert(frames.end(), f.begin(), f.end());
        frames.push_back(aos_fill_frame(spec.spacecraft_id, fill++));
    }
    DownlinkTxConfig tx;
    tx.modulation = decode::PskModulation::Bpsk;
    tx.g2_inverted = false;
    auto symbols = downlink_symbols(tx, frames);
    if (!symbols) {
        return std::unexpected(symbols.error());
    }
    PskModConfig mc;
    mc.rate = static_cast<dsp::SampleRate>(spec.sample_rate);
    mc.symbol_rate = spec.symbol_rate;
    mc.modulation = tx.modulation;
    mc.roll_off = spec.roll_off;
    mc.carrier_offset_hz = spec.carrier_offset_hz;
    mc.initial_phase = spec.initial_phase;
    mc.timing_offset_samples = spec.timing_offset_samples;
    mc.clock_error = spec.clock_error;
    return psk_render(mc, *symbols);
}

}  // namespace revenant::siggen
