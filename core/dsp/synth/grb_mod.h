// The transmitting end of core/decode/grb.h: Space Packets into AOS M_PDU
// frames of 2044 octets with their FECF, CADUs, a continuous generic stream
// of BBFRAMEs, BCH and LDPC, the 8PSK interleaver, PLFRAMEs with physical
// layer scrambling, and root raised cosine complex baseband.
//
// Written from the documents core/decode/grb.h and core/decode/dvbs2.h
// name. Choices the documents leave to the transmitter: the frame version
// number is 732.0's 0b01; the last BBFRAME's data field carries only what is
// left of the CADU stream and is zero padded (PUG 4.3.2, EN 302 307 5.2.1);
// dummy PLFRAMEs (EN 302 307 5.5.1) lead and trail the burst, standing in
// for the continuous carrier a satellite sends.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/grb.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct GrbTxSpec {
    double sample_rate = 20.0e6;
    decode::GrbModulation modulation = decode::GrbModulation::Qpsk910;
    // Zero means the PUG rate.
    double symbol_rate = 0.0;
    bool pilots = false;
    std::uint32_t gold_code = 0;
    std::uint8_t spacecraft_id = 0x10;
    std::uint8_t virtual_channel = decode::kGrbVcRhcp;
    std::size_t leading_dummy_frames = 2;
    std::size_t trailing_dummy_frames = 1;
    double carrier_offset_hz = 0.0;
    double initial_phase = 0.0;
    double timing_offset_samples = 0.0;
    double clock_error = 0.0;
};

// The CADU stream for `packets`, one virtual channel, 2048 octets each.
[[nodiscard]] std::vector<std::uint8_t> grb_cadus(const GrbTxSpec& spec,
                                                  std::span<const std::vector<std::uint8_t>> packets);

// PLFRAME symbols as 8PSK grid indices (decode::psk_point(Psk8, k)).
[[nodiscard]] Expected<std::vector<std::uint8_t>> grb_symbols(const GrbTxSpec& spec,
                                                              std::span<const std::uint8_t> cadus);

[[nodiscard]] Expected<std::vector<dsp::Complex32>> grb_baseband(
    const GrbTxSpec& spec, std::span<const std::vector<std::uint8_t>> packets);

}  // namespace revenant::siggen
