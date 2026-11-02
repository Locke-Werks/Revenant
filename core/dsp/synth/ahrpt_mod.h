// Metop AHRPT transmitter for tests: source packets on several VCs to rate
// 3/4 punctured QPSK complex baseband, with the framing and the stated and
// assumed parameters of core/decode/ahrpt.h.

#pragma once

#include <cstdint>
#include <vector>

#include "core/decode/ahrpt.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct AhrptChannel {
    std::uint8_t virtual_channel = 0;
    std::vector<std::vector<std::uint8_t>> packets;
};

struct AhrptTxSpec {
    double sample_rate = 6.0e6;
    std::uint8_t spacecraft_id = 11;
    double carrier_offset_hz = 0.0;
    double initial_phase = 0.0;
    double timing_offset_samples = 0.0;
    double clock_error = 0.0;
};

[[nodiscard]] Expected<std::vector<dsp::Complex32>> ahrpt_baseband(const AhrptTxSpec& spec,
                                                                   const std::vector<AhrptChannel>& channels);

}  // namespace revenant::siggen
