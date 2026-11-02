// The transmitting end of core/decode/ccsds_downlink.h: Space Packets into
// AOS M_PDU frames, CADUs, the convolutional code (punctured or not, G2
// inverted or not), and channel symbols for core/dsp/synth/psk_mod.h.
//
// Written from the documents core/decode/ccsds_downlink.h names: CGMS LRIT/
// HRIT Global Specification 2.8 sections 8.2 and 8.3 for the M_PDU, its fill
// packet and the CVCDU, CCSDS 131.0-B-5 3.3 and 3.4 for the codes. The QPSK
// rail mapping, C1 on I and C2 on Q of each symbol after puncturing, is this
// transmitter's choice; the receiver does not depend on it (see the header
// comment there).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/ccsds.h"
#include "core/decode/psk_demod.h"
#include "core/error.h"

namespace revenant::siggen {

struct AosPackConfig {
    std::uint8_t spacecraft_id = 0;
    std::uint8_t virtual_channel = 0;
    std::uint32_t first_count = 0;
    std::size_t frame_octets = 892;
    std::size_t insert_zone_octets = 0;
};

// Packs packets back to back into the M_PDU packet zones of successive AOS
// frames of one virtual channel, First Header Pointer per CGMS 8.2.1, the
// last zone completed with a fill packet (APID 2047) and, when fewer than
// seven octets remain, one more whole zone with it (CGMS 8.2.1).
[[nodiscard]] std::vector<std::vector<std::uint8_t>> aos_pack_mpdu(
    const AosPackConfig& config, std::span<const std::vector<std::uint8_t>> packets);

// A fill VCDU, VC 63 and all zero data (CGMS 8.3.2.2).
[[nodiscard]] std::vector<std::uint8_t> aos_fill_frame(std::uint8_t spacecraft_id, std::uint32_t count,
                                                       std::size_t frame_octets = 892);

struct DownlinkTxConfig {
    decode::PskModulation modulation = decode::PskModulation::Qpsk;
    std::vector<std::uint8_t> puncture;
    bool g2_inverted = false;
    decode::CcsdsRandomizer randomizer = decode::CcsdsRandomizer::Legacy255;
    unsigned rs_interleave = 4;
    // Pseudo-random symbols sent before the first marker so the demodulator
    // acquires; a real downlink is a continuous carrier.
    std::size_t preamble_symbols = 2048;
};

// Constellation indices for decode::psk_point, preamble first. Frames must be
// 223 * rs_interleave octets.
[[nodiscard]] Expected<std::vector<std::uint8_t>> downlink_symbols(
    const DownlinkTxConfig& config, std::span<const std::vector<std::uint8_t>> frames);

}  // namespace revenant::siggen
