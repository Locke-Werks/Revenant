// A synthetic Z-Wave transmitter, from ITU-T G.9959 (01/2015) alone, the
// same document core/decode/zwave.h cites, for the round trip in
// tests/decode/test_zwave.cpp.
//
// It renders one PPDU per call (7.1.3): preamble bytes of 0x55, the SOF, the
// MPDU it is handed, and at R1 the end of frame delimiter of 7.1.3.5. R1 and
// R2 are plain FSK with rectangular frequency steps; R3 passes the steps
// through a Gaussian filter with BT = 0.6, Table 7-4. The phase is continuous
// throughout.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/zwave.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;

struct ZWaveModConfig {
    dsp::SampleRate sample_rate = 2'400'000;
    decode::ZWaveRate rate = decode::ZWaveRate::R2;
    // Where the channel centre lands in the output.
    double channel_offset_hz = 0.0;
    // Zero takes Table 7-10's minimum for the rate.
    std::size_t preamble_bytes = 0;
    float amplitude = 1.0F;
};

// `mpdu` is the complete MPDU, FCS included, as zwave_build_mpdu returns it.
[[nodiscard]] Expected<std::vector<Complex32>> zwave_render(const ZWaveModConfig& config,
                                                            std::span<const std::uint8_t> mpdu);

}  // namespace revenant::siggen
