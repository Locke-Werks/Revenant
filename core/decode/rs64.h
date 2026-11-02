// The JT65 Reed-Solomon code: RS(63, 12) over GF(64), systematic, with an
// errors-and-erasures decoder.
//
// SOURCES
//
// J. Taylor K1JT, "The JT65 Communications Protocol", QEX September/October
// 2005 pp. 3-12, section 5, and S. Franke K9AN and J. Taylor K1JT, "Open
// Source Soft-Decision Decoder for the JT65 (63,12) Reed-Solomon Code", QEX
// May/June 2016 pp. 8-17, section 2: the code is RS(63, 12) over a 64-symbol
// alphabet, systematic, minimum distance 52. Neither paper prints the field
// polynomial, the generator's roots or where the message sits in the code
// word; the 2005 paper defers them to source code this project does not
// read.
//
// DERIVED FROM THE PAPER'S EXAMPLES, NOT STATED IN IT: of every primitive
// polynomial of degree 6 and every placement, only GF(64) built on
// x^6 + x + 1, with the generator's 51 roots at alpha^3 to alpha^53 and the
// code word as the coefficients of x^0 to x^62 (parity in 0 to 50, the 12
// message symbols in 51 to 62 in order), makes the three Figure 2 channel
// listings code words. The tests encode all three and compare.
//
// The decoder is textbook: syndromes, Berlekamp-Massey started from the
// erasure locator, Chien search and Forney's formula (Lin and Costello,
// "Error Control Coding", 2nd ed., chapter 7). Nothing here is from any
// implementation.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace revenant::decode {

inline constexpr int kRs64N = 63;
inline constexpr int kRs64K = 12;
inline constexpr int kRs64Roots = kRs64N - kRs64K;  // 51
inline constexpr int kRs64FirstRoot = 3;

using Rs64Word = std::array<std::uint8_t, kRs64N>;
using Rs64Message = std::array<std::uint8_t, kRs64K>;
using Rs64Syndromes = std::array<std::uint8_t, kRs64Roots>;

[[nodiscard]] Rs64Word rs64_encode(const Rs64Message& message);

// All zero for a code word.
[[nodiscard]] Rs64Syndromes rs64_syndromes(const Rs64Word& received);

// Corrects `received` given its syndromes and up to 51 erased positions
// (code word indices). Succeeds when the erasures plus twice the errors
// outside them are at most 51 and the result is a code word.
[[nodiscard]] std::optional<Rs64Word> rs64_decode(const Rs64Word& received, const Rs64Syndromes& syndromes,
                                                  std::span<const int> erasures);

}  // namespace revenant::decode
