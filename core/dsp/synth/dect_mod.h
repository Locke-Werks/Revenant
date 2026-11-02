// DECT synthetic transmitter for physical packet P32, written from the same
// documents as core/decode/dect.h: ETSI EN 300 175-2 V2.9.1 and EN 300 175-3
// V2.9.1. See that header for the clauses.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/dect.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

// EN 300 175-3 clauses 7.1 and 6.2.5.2: header, 40-bit tail and R-CRC as a
// 64-bit A-field, a0 in bit 63.
[[nodiscard]] std::uint64_t dect_a_field(unsigned ta, bool q1, unsigned ba, bool q2, std::uint64_t tail40);

// EN 300 175-2 clauses 4.6, 4.7.2 and 4.8: S-field, D-field (A-field, the
// 320 B-field bits given, the 4 X-field bits given) and the Z-field, 424
// symbols, s0 first.
[[nodiscard]] std::vector<std::uint8_t> dect_p32_bits(decode::DectPolarity polarity, std::uint64_t a_field,
                                                      std::span<const std::uint8_t> b_field,
                                                      std::span<const std::uint8_t, 4> x_field);

struct DectTxConfig {
    dsp::SampleRate sample_rate = 4'608'000;
    double frequency_offset_hz = 0.0;
    double amplitude = 1.0;
    // EN 300 175-2 clause 5.4.3 allows 70 % to 140 % of nominal.
    double deviation_hz = decode::kDectDeviationHz;
};

// GFSK, BT 0,5 (clause 5.4.1), "1" at +deviation (clause 5.4.2), the burst
// alone with no ramp.
[[nodiscard]] Expected<std::vector<dsp::Complex32>> dect_modulate(std::span<const std::uint8_t> bits,
                                                                  const DectTxConfig& config);

}  // namespace revenant::siggen
