#include "core/dsp/synth/ccsds_downlink_mod.h"

#include <array>

#include "core/decode/dv_codes.h"
#include "core/decode/viterbi_stream.h"
#include "core/dsp/synth/ccsds_mod.h"

namespace revenant::siggen {

namespace {

constexpr std::size_t kMpduHeader = 2;

std::vector<std::uint8_t> fill_packet(std::size_t total_octets) {
    decode::SpacePacketHeader h;
    h.apid = decode::kIdleApid;
    h.sequence_flags = 3;
    const std::vector<std::uint8_t> data(total_octets - decode::kSpacePacketHeaderOctets, 0);
    return ccsds_space_packet(h, data);
}

}  // namespace

std::vector<std::vector<std::uint8_t>> aos_pack_mpdu(const AosPackConfig& config,
                                                     std::span<const std::vector<std::uint8_t>> packets) {
    const std::size_t zone = config.frame_octets - decode::kAosFrameHeaderOctets - config.insert_zone_octets -
                             kMpduHeader;
    std::vector<std::uint8_t> stream;
    std::vector<std::size_t> starts;
    for (const auto& p : packets) {
        starts.push_back(stream.size());
        stream.insert(stream.end(), p.begin(), p.end());
    }
    std::size_t gap = (zone - stream.size() % zone) % zone;
    if (gap != 0 && gap < 7) {
        gap += zone;
    }
    if (gap != 0) {
        starts.push_back(stream.size());
        const auto fill = fill_packet(gap);
        stream.insert(stream.end(), fill.begin(), fill.end());
    }
    std::vector<std::vector<std::uint8_t>> frames;
    std::size_t next_start = 0;
    for (std::size_t off = 0, n = 0; off < stream.size(); off += zone, ++n) {
        std::vector<std::uint8_t> f(config.frame_octets, 0);
        decode::AosFrameHeader h;
        h.spacecraft_id = config.spacecraft_id;
        h.virtual_channel = config.virtual_channel;
        h.vc_frame_count = (config.first_count + static_cast<std::uint32_t>(n)) & 0xFFFFFFU;
        decode::write_aos_frame_header(h, f);
        while (next_start < starts.size() && starts[next_start] < off) {
            ++next_start;
        }
        std::uint16_t fhp = decode::kCcsdsNoPacketStarts;
        if (next_start < starts.size() && starts[next_start] < off + zone) {
            fhp = static_cast<std::uint16_t>(starts[next_start] - off);
        }
        const std::size_t m = decode::kAosFrameHeaderOctets + config.insert_zone_octets;
        f[m] = static_cast<std::uint8_t>(fhp >> 8U);
        f[m + 1] = static_cast<std::uint8_t>(fhp & 0xFFU);
        std::copy_n(stream.begin() + static_cast<std::ptrdiff_t>(off), zone, f.begin() + static_cast<std::ptrdiff_t>(m + kMpduHeader));
        frames.push_back(std::move(f));
    }
    return frames;
}

std::vector<std::uint8_t> aos_fill_frame(std::uint8_t spacecraft_id, std::uint32_t count, std::size_t frame_octets) {
    std::vector<std::uint8_t> f(frame_octets, 0);
    decode::AosFrameHeader h;
    h.spacecraft_id = spacecraft_id;
    h.virtual_channel = 63;
    h.vc_frame_count = count & 0xFFFFFFU;
    decode::write_aos_frame_header(h, f);
    return f;
}

Expected<std::vector<std::uint8_t>> downlink_symbols(const DownlinkTxConfig& config,
                                                     std::span<const std::vector<std::uint8_t>> frames) {
    CcsdsTxConfig tx;
    tx.reed_solomon = true;
    tx.rs = {16, config.rs_interleave, 0};
    tx.randomizer = config.randomizer;
    tx.convolutional = false;
    auto bits = ccsds_channel_bits(tx, frames, 0);
    if (!bits) {
        return std::unexpected(bits.error());
    }
    // Flush the encoder so the receiver's traceback releases the last frame.
    bits->insert(bits->end(), 64, 0);
    static constexpr std::array<std::uint32_t, 2> kGens{decode::kCcsdsK7G1, decode::kCcsdsK7G2};
    decode::ConvolutionalCode code{decode::kCcsdsK7Memory, kGens};
    auto coded = decode::convolutional_encode(code, *bits);
    if (!coded) {
        return std::unexpected(coded.error());
    }
    if (config.g2_inverted) {
        for (std::size_t i = 1; i < coded->size(); i += 2) {
            (*coded)[i] ^= 1U;
        }
    }
    std::vector<std::uint8_t> channel =
        config.puncture.empty() ? std::move(*coded) : decode::puncture_bits(*coded, config.puncture);

    std::vector<std::uint8_t> symbols;
    std::uint32_t lfsr = 0x1234567U;
    const unsigned order = decode::psk_order(config.modulation);
    for (std::size_t i = 0; i < config.preamble_symbols; ++i) {
        lfsr = lfsr * 1664525U + 1013904223U;
        symbols.push_back(static_cast<std::uint8_t>((lfsr >> 24U) % order));
    }
    if (config.modulation == decode::PskModulation::Bpsk) {
        // Point 0 is +1, which the receiver's SoftSpan reads as a zero.
        symbols.insert(symbols.end(), channel.begin(), channel.end());
        return symbols;
    }
    if (channel.size() % 2 != 0) {
        channel.push_back(0);
    }
    // Points sit at pi/4 + k pi/2: k = 0 (+,+), 1 (-,+), 2 (-,-), 3 (+,-).
    static constexpr std::array<std::uint8_t, 4> kIndex{0, 3, 1, 2};  // [c1 * 2 + c2]
    for (std::size_t i = 0; i < channel.size(); i += 2) {
        symbols.push_back(kIndex[(channel[i] << 1U) | channel[i + 1]]);
    }
    return symbols;
}

}  // namespace revenant::siggen
