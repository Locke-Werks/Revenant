// GPS L1C tables transcribed from "IS-GPS-800 Revision J" (IS-GPS-800J,
// 1 August 2022), Navstar GPS Space Segment / User Segment L1C Interface,
// free from gps.gov. A US Government work, transcribed with citation.
//
//   - Table 3.2-2: Weil index and insertion index of the L1CP and L1CD
//     ranging codes for PRNs 1 to 63, with the initial and final 24 chips
//     the document prints for each, in octal.
//   - Table 3.2-3: the L1CO overlay S1 generator polynomial, initial and
//     final 11 bits for PRNs 1 to 63, in octal.
//   - Tables 6.2-2 to 6.2-13: the coordinates of the ones in the LDPC
//     submatrices A, B, C, D, E and T of subframes 2 and 3, rows and columns
//     counted from 1 as the document counts them.
//
// The .cpp was produced by extracting the text of the document's PDF and
// parsing the tables out of it. The extraction was checked against the
// document itself in two ways that do not depend on the parse: every row and
// every column weight of A and T for both subframes equals Tables 6.2-14 to
// 6.2-17, which give those weights separately; and the codes built from
// Table 3.2-2 and the Legendre sequence of section 3.2.2.1.1 reproduce the
// table's own first and last 24 chips for all 126 codes, which
// tests/decode/test_gps_l1c.cpp holds them to, as it holds the overlay
// generator to the final 11 bits of Table 3.2-3.

#pragma once

#include <array>
#include <cstdint>

namespace revenant::decode::gnss::l1c_tables {

struct RangingRow {
    std::uint16_t pilot_weil, pilot_insert, data_weil, data_insert;
    std::uint32_t pilot_first24, pilot_last24, data_first24, data_last24;
};

struct OverlayRow {
    std::uint16_t polynomial;  // octal 1, m10..m1, 1 as printed
    std::uint16_t initial;     // n11..n1
    std::uint16_t final_bits;  // the last 11 bits of the 1800
};

struct Coord {
    std::uint16_t row, col;
};

extern const std::array<RangingRow, 63> kRanging;
extern const std::array<OverlayRow, 63> kOverlay;

extern const std::array<Coord, 3573> kSf2A;
extern const std::array<Coord, 14> kSf2B;
extern const std::array<Coord, 1223> kSf2T;
extern const std::array<Coord, 5> kSf2C;
extern const std::array<Coord, 1> kSf2D;
extern const std::array<Coord, 2> kSf2E;
extern const std::array<Coord, 1483> kSf3A;
extern const std::array<Coord, 10> kSf3B;
extern const std::array<Coord, 571> kSf3T;
extern const std::array<Coord, 4> kSf3C;
extern const std::array<Coord, 1> kSf3D;
extern const std::array<Coord, 2> kSf3E;

}  // namespace revenant::decode::gnss::l1c_tables
