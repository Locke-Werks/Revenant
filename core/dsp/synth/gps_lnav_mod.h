// GPS L1 C/A LNAV transmitter side: subframes from fields, and the parity
// encoding that chains words, for the round trip in
// tests/decode/test_gps_lnav.cpp.
//
// SPECIFICATION
//
// IS-GPS-200N, the same sections core/decode/gps_lnav.h cites: Figure 20-1 for
// the layouts, Tables 20-I, 20-III, 20-VI, 20-IX and 20-X for the scale
// factors, and 20.3.5 with Table 20-XIV for parity, including the two
// non-information-bearing bits 23 and 24 of words 2 and 10 that are "solved
// for bits to preserve parity check with zeros in bits 29 and 30" (Figure
// 20-2).

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/decode/gps_lnav.h"

namespace revenant::siggen {

using LnavWords = std::array<std::uint32_t, 10>;

// Write `count` bits of `value` at IS-GPS-200N subframe bit `first`.
void lnav_put(LnavWords& words, unsigned first, unsigned count, std::uint64_t value);

// TLM and HOW for subframe `id` whose following subframe starts at
// `tow_count_next` (6-second units). Bits 23-24 of word 2 are left for the
// encoder to solve.
[[nodiscard]] LnavWords lnav_header(unsigned id, std::uint32_t tow_count_next);

void lnav_put_clock(LnavWords& w, const decode::gnss::LnavClock& c);
void lnav_put_ephemeris(LnavWords& sf2, LnavWords& sf3, const decode::gnss::LnavEphemeris& e);
void lnav_put_almanac(LnavWords& w, const decode::gnss::LnavAlmanac& a);  // also data ID, SV ID
void lnav_put_utc_iono(LnavWords& w, const decode::gnss::LnavUtcIono& x);  // also SV ID 56

// 300 transmitted bits. `d29`/`d30` carry the previous subframe's last two
// bits in and this one's out.
[[nodiscard]] std::vector<std::uint8_t> lnav_encode(const LnavWords& words, bool& d29, bool& d30);

}  // namespace revenant::siggen
