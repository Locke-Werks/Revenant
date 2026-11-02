// Meteor-M LRPT transmitter for tests: Space Packets on one or more virtual
// channels to QPSK or OQPSK complex baseband, with the framing and codes of
// core/decode/lrpt.h and the same caveat: the parameters are the Blue Book
// defaults that header lists, not checked against a recording.

#pragma once

#include <cstdint>
#include <vector>

#include "core/decode/lrpt.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct LrptChannel {
    std::uint8_t virtual_channel = 0;
    std::vector<std::vector<std::uint8_t>> packets;
};

struct LrptTxSpec {
    double sample_rate = 288000.0;
    double symbol_rate = decode::kLrptSymbolRate;
    bool oqpsk = false;
    std::uint8_t spacecraft_id = 0;
    double carrier_offset_hz = 0.0;
    double initial_phase = 0.0;
    double timing_offset_samples = 0.0;
    double clock_error = 0.0;
    // Fill VCDUs between channels, as an idle downlink sends.
    std::size_t fill_frames = 2;
};

// The VCDUs, channel after channel with fill frames between.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> lrpt_frames(const LrptTxSpec& spec,
                                                                 const std::vector<LrptChannel>& channels);

[[nodiscard]] Expected<std::vector<dsp::Complex32>> lrpt_baseband(const LrptTxSpec& spec,
                                                                  const std::vector<LrptChannel>& channels);

}  // namespace revenant::siggen
