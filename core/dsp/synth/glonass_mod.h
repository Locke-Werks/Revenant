// GLONASS L1OF navigation message, transmitter side: strings from fields,
// the Hamming check bits, relative code, meander and time mark, as 10 ms
// symbols for core/dsp/synth/gnss_mod.h.
//
// ICD 5.1 sections 3.3.2.2, 4.3, 4.7 and Tables 4.5 and 4.6, the same
// clauses as core/decode/glonass_l1.h, with the same two readings of the
// relative code and meander phase that header records.

#pragma once

#include <cstdint>
#include <vector>

#include "core/decode/glonass_l1.h"

namespace revenant::siggen {

// Write `value` into string bits `hi` down to `lo`, MSB first.
void glonass_put(decode::gnss::GlonassStringBits& bits, unsigned hi, unsigned lo,
                 std::uint32_t value);

// Sign and magnitude, ICD 5.1 4.4.
[[nodiscard]] std::uint32_t glonass_signmag(double value, double lsb, unsigned bits);

// Strings 1 to 4 carrying `e`, string numbers in bits 81-84.
[[nodiscard]] std::vector<decode::gnss::GlonassStringBits> glonass_immediate_strings(
    const decode::gnss::GlonassEphemeris& e);

// A string with only its number set, for filler.
[[nodiscard]] decode::gnss::GlonassStringBits glonass_empty_string(unsigned number);

// 200 symbols of 10 ms: 170 for the data in relative code and meander, 30
// for the time mark. Fills the check bits first.
[[nodiscard]] std::vector<std::uint8_t> glonass_string_symbols(decode::gnss::GlonassStringBits bits);

}  // namespace revenant::siggen
