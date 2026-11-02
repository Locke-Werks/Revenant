// ETSI EN 302 307-1 annex B tables used by core/decode/dvbs2.h. See
// dvbs2_tables.cpp for the provenance and the rights note.
//
// Each table is flattened: a row is its entry count followed by that many
// parity bit accumulator addresses, one row per group of 360 information bits.

#pragma once

#include <cstdint>
#include <span>

namespace revenant::decode::dvbs2_tables {

// Table B.6, rate 2/3, nldpc = 64 800.
[[nodiscard]] std::span<const std::uint16_t> table_b6();
// Table B.11, rate 9/10, nldpc = 64 800.
[[nodiscard]] std::span<const std::uint16_t> table_b11();

}  // namespace revenant::decode::dvbs2_tables
