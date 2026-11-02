// BeiDou B1I D1, transmitter side: subframes from fields, BCH(15,11,1) and
// the interleave of words 2-10, for the round trip in
// tests/decode/test_beidou_b1i.cpp. The same B1I ICD clauses
// core/decode/beidou_b1i.h cites.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/decode/beidou_b1i.h"

namespace revenant::siggen {

using B1iInfo = std::array<std::uint8_t, 301>;  // figure positions 1..300

void b1i_put(B1iInfo& bits, unsigned first, unsigned count, std::uint64_t value);

// Preamble, FraID and SOW.
[[nodiscard]] B1iInfo b1i_header(unsigned fra_id, std::uint32_t sow);

void b1i_put_clock_iono(B1iInfo& sf1, const decode::gnss::B1iClockIono& c);
void b1i_put_ephemeris(B1iInfo& sf2, B1iInfo& sf3, const decode::gnss::B1iEphemeris& e);

// 300 transmitted bits.
[[nodiscard]] std::vector<std::uint8_t> b1i_encode(const B1iInfo& bits);

}  // namespace revenant::siggen
