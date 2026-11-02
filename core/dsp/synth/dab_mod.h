// A DAB transmission mode I ensemble generator, for the round trip in
// tests/decode/test_dab.cpp.
//
// Written from the same clauses of ETSI EN 300 401 V2.1.1 as
// core/decode/dab.h, in the transmit direction: FIG and FIB assembly (5.2),
// packet mode and MSC data groups (5.3.2, 5.3.3), energy dispersal (10),
// convolutional coding (11), time interleaving (12), the CIF (13), block
// partitioning, QPSK, frequency interleaving and D-QPSK (14.4 to 14.7), and
// the null and phase reference symbols (14.3). MOT segments follow ETSI EN
// 301 234 V2.1.1 clauses 5.1.1 and 6.1. Every constant comes from the
// decoder's tables, so a misreading in one is a misreading in both; the
// tests that check the printed values directly (table 12, table 25) are what
// catches that.
//
// Output is complex baseband at 2.048 MS/s, the carrier set occupying
// +-768 kHz, with the OFDM symbols scaled to unit mean power per sample.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include "core/decode/dab.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;

// ---------------------------------------------------------------------------
// FIG builders: each returns one whole FIG, header byte included.
// ---------------------------------------------------------------------------

// FIG 0/0, clause 6.4.1, no change flags.
[[nodiscard]] std::vector<std::uint8_t> dab_fig_ensemble(std::uint16_t eid, unsigned cif_count);

// FIG 0/1, clause 6.2.1, long form sub-channels.
[[nodiscard]] std::vector<std::uint8_t> dab_fig_subchannels(
    std::span<const decode::DabSubchannel> subchannels);

// FIG 0/2, clause 6.3.1, one service with its components. 16-bit SId unless
// data_sid.
[[nodiscard]] std::vector<std::uint8_t> dab_fig_service(const decode::DabService& service);

// FIG 0/3, clause 6.3.2.
[[nodiscard]] std::vector<std::uint8_t> dab_fig_packet(const decode::DabPacketComponent& component);

// FIG 1/0 (ext 0, id = EId) or 1/1 (ext 1, id = SId), clause 5.2.2.2 and
// 8.1.13 to 8.1.14, charset 0, padded with spaces to 16.
[[nodiscard]] std::vector<std::uint8_t> dab_fig_label(unsigned ext, std::uint32_t id,
                                                      std::string_view text);

// Packs FIGs into the 30-byte data field of one FIB with an end marker and
// zero padding (clause 5.2.1). Fails if they do not fit.
[[nodiscard]] Expected<std::array<std::uint8_t, 30>> dab_pack_fib(
    std::span<const std::vector<std::uint8_t>> figs);

// ---------------------------------------------------------------------------
// Packet mode
// ---------------------------------------------------------------------------

struct DabDataGroupSpec {
    unsigned type = 0;
    unsigned continuity = 0;
    unsigned repetition = 0;
    bool segmented = false;
    bool last = false;
    unsigned segment = 0;
    bool has_transport_id = false;
    std::uint16_t transport_id = 0;
};

// One MSC data group with its CRC (clause 5.3.3).
[[nodiscard]] std::vector<std::uint8_t> dab_data_group(const DabDataGroupSpec& spec,
                                                       std::span<const std::uint8_t> data);

// Splits a data group into packets of one length (24, 48, 72 or 96 bytes,
// table 6) on `address`, continuity counting on from `continuity`, which is
// updated. Each packet carries its CRC (clause 5.3.2).
[[nodiscard]] std::vector<std::vector<std::uint8_t>> dab_packetize(
    std::span<const std::uint8_t> group, unsigned address, std::size_t packet_length,
    unsigned& continuity);

// A padding packet of the given length (address 0).
[[nodiscard]] std::vector<std::uint8_t> dab_padding_packet(std::size_t packet_length);

// A MOT header (EN 301 234 clause 6.1 core plus a ContentName parameter).
[[nodiscard]] std::vector<std::uint8_t> mot_header(std::size_t body_size, unsigned content_type,
                                                   unsigned content_subtype,
                                                   std::string_view content_name);

// Prepends the clause 5.1.1 segmentation header to one segment.
[[nodiscard]] std::vector<std::uint8_t> mot_segment(std::span<const std::uint8_t> segment,
                                                    unsigned repetition_count);

// ---------------------------------------------------------------------------
// The transmitter
// ---------------------------------------------------------------------------

class DabTransmitter {
public:
    // Every sub-channel must be long form (EEP).
    [[nodiscard]] static Expected<DabTransmitter> create(
        std::vector<decode::DabSubchannel> subchannels);

    // One 96 ms transmission frame. `fibs` holds the 12 FIB data fields of the
    // frame, CIF by CIF, three to a CIF; `frames[id]` the four logical frames
    // (I/8 bytes each) of sub-channel `id`. A sub-channel left out sends zeros.
    [[nodiscard]] Expected<std::vector<Complex32>> frame(
        std::span<const std::array<std::uint8_t, 30>> fibs,
        const std::map<unsigned, std::array<std::vector<std::uint8_t>, 4>>& frames);

    [[nodiscard]] std::size_t logical_frame_bytes(unsigned id) const;

private:
    struct Channel {
        decode::DabSubchannel config;
        decode::DabEepProfile profile;
        std::vector<std::vector<std::uint8_t>> history;  // last 16 codewords
        std::uint64_t written = 0;
    };
    DabTransmitter() = default;
    std::map<unsigned, Channel> channels_;
};

}  // namespace revenant::siggen
