#include "core/dsp/synth/ccsds_mod.h"

#include <algorithm>
#include <array>

#include "core/decode/dv_codes.h"
#include "core/decode/viterbi_stream.h"

namespace revenant::siggen {

using namespace revenant::decode;

Expected<std::vector<std::uint8_t>> ccsds_channel_bits(const CcsdsTxConfig& config,
                                                       std::span<const std::vector<std::uint8_t>> frames,
                                                       std::size_t tail_bits) {
    std::optional<CcsdsReedSolomon> rs;
    if (config.reed_solomon) {
        auto r = CcsdsReedSolomon::create(config.rs);
        if (!r) {
            return std::unexpected(r.error());
        }
        rs = std::move(*r);
    }
    std::vector<std::uint8_t> bits;
    for (const auto& frame : frames) {
        if (rs && frame.size() != rs->data_octets()) {
            return fail("ccsds_channel_bits: frame length is not the Reed-Solomon data length");
        }
        if (frame.size() != frames.front().size()) {
            return fail("ccsds_channel_bits: frames differ in length");
        }
        std::vector<std::uint8_t> block = rs ? rs->encode(frame) : frame;
        ccsds_randomize(block, config.randomizer);
        for (unsigned i = 0; i < kCcsdsAsmBits; ++i) {
            bits.push_back(static_cast<std::uint8_t>((kCcsdsAsm >> (31U - i)) & 1U));
        }
        for (const std::uint8_t octet : block) {
            for (unsigned i = 0; i < 8; ++i) {
                bits.push_back(static_cast<std::uint8_t>((octet >> (7U - i)) & 1U));
            }
        }
    }
    if (config.nrzm) {
        // NRZ-M: a one changes the level.
        std::uint8_t level = 0;
        for (auto& b : bits) {
            level ^= b;
            b = level;
        }
    }
    if (!config.convolutional) {
        return bits;
    }
    bits.insert(bits.end(), tail_bits, 0);
    static constexpr std::array<std::uint32_t, 2> kGenerators{kCcsdsK7G1, kCcsdsK7G2};
    auto encoded = convolutional_encode({kCcsdsK7Memory, kGenerators}, bits);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    // 131.0 3.3.1 (5): the G2 output is inverted.
    for (std::size_t i = 1; i < encoded->size(); i += 2) {
        (*encoded)[i] ^= 1U;
    }
    return std::move(*encoded);
}

std::vector<std::uint8_t> ccsds_space_packet(SpacePacketHeader header, std::span<const std::uint8_t> data) {
    header.data_length = static_cast<std::uint16_t>(data.empty() ? 0 : data.size() - 1);
    std::vector<std::uint8_t> out(kSpacePacketHeaderOctets);
    write_space_packet_header(header, out);
    out.insert(out.end(), data.begin(), data.end());
    if (data.empty()) {
        out.push_back(0);  // 133.0 4.1.4.1.2: the data field is at least one octet
    }
    return out;
}

std::vector<std::vector<std::uint8_t>> ccsds_pack_tm_frames(const TmFrameHeader& first, std::size_t frame_octets,
                                                            bool fecf,
                                                            std::span<const std::vector<std::uint8_t>> packets) {
    const std::size_t zone = frame_octets - kTmFrameHeaderOctets - (fecf ? 2U : 0U);
    // The stream, with where each packet starts in it.
    std::vector<std::uint8_t> stream;
    std::vector<std::size_t> starts;
    for (const auto& p : packets) {
        starts.push_back(stream.size());
        stream.insert(stream.end(), p.begin(), p.end());
    }
    const std::size_t frames = (stream.size() + zone - 1) / zone;
    const std::size_t room = frames * zone - stream.size();
    if (room >= kSpacePacketHeaderOctets + 1) {
        SpacePacketHeader idle;
        idle.apid = kIdleApid;
        const std::vector<std::uint8_t> fill(room - kSpacePacketHeaderOctets, 0x55);
        const auto p = ccsds_space_packet(idle, fill);
        starts.push_back(stream.size());
        stream.insert(stream.end(), p.begin(), p.end());
    } else {
        stream.insert(stream.end(), room, 0x55);
    }

    std::vector<std::vector<std::uint8_t>> out;
    TmFrameHeader h = first;
    std::size_t next_start = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        const std::size_t lo = f * zone;
        const std::size_t hi = lo + zone;
        while (next_start < starts.size() && starts[next_start] < lo) {
            ++next_start;
        }
        h.first_header_pointer = (next_start < starts.size() && starts[next_start] < hi)
                                     ? static_cast<std::uint16_t>(starts[next_start] - lo)
                                     : kCcsdsNoPacketStarts;
        std::vector<std::uint8_t> frame(frame_octets, 0);
        write_tm_frame_header(h, frame);
        std::copy(stream.begin() + static_cast<std::ptrdiff_t>(lo), stream.begin() + static_cast<std::ptrdiff_t>(hi),
                  frame.begin() + kTmFrameHeaderOctets);
        if (fecf) {
            const auto crc = ccsds_fecf(std::span(frame).first(frame_octets - 2));
            frame[frame_octets - 2] = static_cast<std::uint8_t>(crc >> 8U);
            frame[frame_octets - 1] = static_cast<std::uint8_t>(crc & 0xFFU);
        }
        out.push_back(std::move(frame));
        h.master_frame_count = static_cast<std::uint8_t>(h.master_frame_count + 1);
        h.vc_frame_count = static_cast<std::uint8_t>(h.vc_frame_count + 1);
    }
    return out;
}

}  // namespace revenant::siggen
