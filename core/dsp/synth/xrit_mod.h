// The sending side of core/decode/xrit.h: xRIT files from header records and
// a data field, and the transport file cut into source packets, per CGMS
// LRIT/HRIT Global Specification 2.8 sections 4.2, 6.2.2 and 7.1. Plus the
// BPSK downlink both GK-2A LRIT and GOES HRIT use, for their tests.

#pragma once

#include <cstdint>
#include <vector>

#include "core/decode/xrit.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

// Primary header first, built from `file_type`, the records and the data.
[[nodiscard]] std::vector<std::uint8_t> xrit_file_bytes(std::uint8_t file_type,
                                                        const std::vector<decode::XritHeaderRecord>& records,
                                                        const std::vector<std::uint8_t>& data);

// The transport file split into source packets on `apid`, sequence count
// starting at `first_count`.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> xrit_packets(std::uint16_t apid, std::uint16_t file_counter,
                                                                  const std::vector<std::uint8_t>& s_pdu,
                                                                  std::uint16_t first_count = 0);

struct XritSend {
    std::uint8_t virtual_channel = 0;
    std::vector<std::vector<std::uint8_t>> packets;
};

struct XritBpskSpec {
    double sample_rate = 0.0;
    double symbol_rate = 0.0;
    double roll_off = 0.5;
    std::uint8_t spacecraft_id = 0;
    double carrier_offset_hz = 0.0;
    double initial_phase = 0.0;
    double timing_offset_samples = 0.0;
    double clock_error = 0.0;
};

// AOS frames per VC, fill frames between, r = 1/2 K = 7 without the G2
// inversion, BPSK.
[[nodiscard]] Expected<std::vector<dsp::Complex32>> xrit_bpsk_baseband(const XritBpskSpec& spec,
                                                                       const std::vector<XritSend>& sends);

}  // namespace revenant::siggen
