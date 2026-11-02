// A Cospas-Sarsat 406 MHz first-generation beacon transmitter, for the
// tests: message builders and the burst waveform.
//
// SPECIFICATION
//
// C/S T.001 Issue 4 Rev. 7, as core/decode/cospas_sarsat.h cites it: the
// burst of 2.2.6 to 2.2.8, biphase-L at +-1.1 rad per 2.3.5, 2.3.6 and
// figure 2.5, with the 150 us rise and fall of 2.3.6 drawn as a linear phase
// ramp; the standard location protocol of A3.3.5 with the position rounding
// of A3.3.1; and the user-location protocol of A3.3.4.
//
// CLEAN ROOM
//
// Written from T.001. No beacon encoder was read.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct CsBurstConfig {
    dsp::SampleRate rate = 16000;
    double bit_rate = 400.0;       // 2.2.8 allows +-1%
    double rise_seconds = 150e-6;  // 2.3.6
    double amplitude = 1.0;
    double initial_phase = 0.0;
    // Silence either side of the burst.
    double lead_seconds = 0.3;
    double tail_seconds = 0.3;
};

// One burst carrying `bits`, 112 or 144 of them, bit 1 first.
[[nodiscard]] Expected<std::vector<dsp::Complex32>> cs_render_burst(const CsBurstConfig& config,
                                                                    std::span<const std::uint8_t> bits);

// A long standard location message (A3.3.5), BCH fields filled. `protocol`
// is the 4-bit code of Table A2-B, `identity` the 24 bits of 41-64.
[[nodiscard]] std::vector<std::uint8_t> cs_standard_location(unsigned country, unsigned protocol,
                                                             std::uint32_t identity, double latitude,
                                                             double longitude, bool internal_source,
                                                             bool homing_121_5, bool self_test = false);

// A long user-location message (A3.3.4) on the serial user protocol with a
// 20-bit serial number, BCH fields filled.
[[nodiscard]] std::vector<std::uint8_t> cs_serial_user_location(unsigned country, unsigned beacon_type,
                                                                std::uint32_t serial, double latitude,
                                                                double longitude);

}  // namespace revenant::siggen
