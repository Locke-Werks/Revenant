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
#include <optional>
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

// Second generation, C/S T.018 Issue 1 Rev. 7 as core/decode/cospas_sarsat.h
// cites it: the 1 s DSSS-OQPSK burst of 2.2.3 to 2.2.7 and 2.3.3 with
// rectangular chips, and the main field of Table 3.1.

struct SgbBurstConfig {
    dsp::SampleRate rate = 153600;
    double chip_rate = 38400.0;  // 2.3.1.2 allows +-0.6 chip/s
    double amplitude = 1.0;
    double initial_phase = 0.0;
    double lead_seconds = 0.2;
    double tail_seconds = 0.2;
    bool self_test = false;  // the self-test PRN of Table 2.2
};

// One burst carrying 250 message bits, bit 1 first.
[[nodiscard]] Expected<std::vector<dsp::Complex32>> sgb_render_burst(
    const SgbBurstConfig& config, std::span<const std::uint8_t> bits);

struct SgbMessageSpec {
    unsigned tac = 0;
    unsigned serial = 0;
    unsigned country = 0;
    bool homing = true;
    bool rls = false;
    bool test_protocol = false;
    // Encoded per Table 3.1 and the rounding note under it; nothing sends
    // the Appendix C.3 defaults.
    std::optional<double> latitude;
    std::optional<double> longitude;
    unsigned vessel_id_type = 0;
    std::uint64_t vessel_id = 0;  // bits 94-137, 44 bits
    unsigned beacon_type = 0;
    unsigned rotating_field = 0;
    std::uint64_t rotating_bits = 0;  // bits 159-202, 44 bits
};

// 250 bits: the main field, the rotating field and the BCH field. Spare bits
// 141-154 are ones, or zeros for a cancellation message (rotating field 15).
[[nodiscard]] std::vector<std::uint8_t> sgb_message(const SgbMessageSpec& spec);

}  // namespace revenant::siggen
