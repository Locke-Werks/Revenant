// ASTERIX Category 021, ADS-B target reports: a parser of data blocks into
// records. No radio: this reads what a ground station sends on, so it is a
// pure function of the octets.
//
// SPECIFICATION
//
// EUROCONTROL-SPEC-0149-12, "EUROCONTROL Specification for Surveillance Data
// Exchange ASTERIX Part 12, Category 021: ADS-B Target Reports", Edition 2.7,
// 02/07/2025, read 2026-11-02 from
// https://www.eurocontrol.int/sites/default/files/2025-07/asterix-adsbtr-cat021-part12-v2-7.pdf.
// And EUROCONTROL-SPEC-0149 Part 1, "All Purpose Structured EUROCONTROL
// Surveillance Information Exchange", Edition 3.1, 23/11/2021, from
// https://www.eurocontrol.int/sites/default/files/2021-11/eurocontrol-specification-asterix-part1-ed-3-1.pdf.
// EUROCONTROL permits copying for non-commercial purposes with the source
// named; only clause-cited constants are transcribed here.
//
// Clauses used. Part 12: 4.4 (the data block: CAT, a two-octet LEN counting
// itself and CAT, then records), 4.5 (items in FRN order), 5.2 (each item's
// format and length, and the scales of the ones decoded below), 5.3 Table 1
// (the UAP). Part 1: 5.2.7 and Figure 13 (FSPEC, seven presence bits and an
// FX in the LSB of each octet), 5.2.5.1.3 (explicit length items, the length
// octet counting itself), 5.2.5.4 and 5.2.5.5 (SPF and REF are explicit
// length).
//
// Table 1 as printed in Edition 2.7 has its Information and Length columns
// misaligned against the FRN and Data Item columns; the FRN-to-item order
// here is the Data Item column read top to bottom, which agrees with the FX
// rows every eighth line, and each item's length comes from its own clause
// in 5.2, never from the table.
//
// One assumption. I021/170's eight six-bit characters are coded per ICAO
// Annex 10 Volume IV Table 3-8, which is not held. They are decoded with
// ITU-R M.1371's Table 47 (core/decode/ais.h ais_sixbit_char), the same
// six-bit subset of IA-5, and the raw item is kept beside them.
//
// WHERE THIS STOPS
//
// Every item is delimited, so a record with any mix of items parses and the
// next one is found. The fields below are decoded; every other item is kept
// as its octets under its FRN, and the REF and SPF contents are not
// interpreted (the REF is a separate document).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/error.h"

namespace revenant::decode {

// Part 12 clause 4.4.
inline constexpr std::uint8_t kAsterixCat021 = 21;

// Part 12 Table 1: FRN 1 to 42 are data items, 43 to 47 not used, 48 the
// REF and 49 the SPF.
inline constexpr unsigned kCat021LastFrn = 49;

struct Cat021Item {
    unsigned frn = 0;
    // The item number, 10 for I021/010; 0 for REF and SPF and unused FRNs.
    unsigned item = 0;
    std::vector<std::uint8_t> octets;
};

struct Cat021Record {
    // I021/010, clause 5.2.2.
    std::optional<std::uint8_t> sac;
    std::optional<std::uint8_t> sic;
    // I021/161, 5.2.31: bits 12/1.
    std::optional<std::uint16_t> track_number;
    // I021/015, 5.2.3.
    std::optional<std::uint8_t> service_id;
    // I021/071, 5.2.8: seconds since midnight UTC, LSB 1/128 s.
    std::optional<double> time_of_applicability_position;
    // I021/073, 5.2.10: the same scale.
    std::optional<double> time_of_reception_position;
    // I021/130, 5.2.18 (LSB 180/2^23 degrees) and I021/131, 5.2.19 (LSB
    // 180/2^30 degrees); the high resolution one wins when both are sent.
    std::optional<double> latitude;
    std::optional<double> longitude;
    // I021/080, 5.2.15.
    std::optional<std::uint32_t> target_address;
    // I021/140, 5.2.21: feet, LSB 6.25.
    std::optional<double> geometric_height_ft;
    // I021/145, 5.2.22: flight level, LSB 1/4.
    std::optional<double> flight_level;
    // I021/151, 5.2.26: knots, LSB 1, and its RE bit.
    std::optional<std::uint16_t> true_airspeed_kt;
    // I021/152, 5.2.27: degrees, LSB 360/2^16.
    std::optional<double> magnetic_heading_deg;
    // I021/155, 5.2.28, and I021/157, 5.2.29: feet a minute, LSB 6.25.
    std::optional<double> barometric_vertical_rate_fpm;
    std::optional<double> geometric_vertical_rate_fpm;
    // I021/160, 5.2.30: ground speed LSB 2^-14 NM/s, as knots; track angle
    // LSB 360/2^16 degrees.
    std::optional<double> ground_speed_kt;
    std::optional<double> track_angle_deg;
    // I021/070, 5.2.7: the Mode 3/A code, bits 12/1, as four octal digits.
    std::optional<std::uint16_t> mode_3a;
    // I021/170, 5.2.33. See SPECIFICATION.
    std::optional<std::string> target_identification;
    // I021/020, 5.2.5.
    std::optional<std::uint8_t> emitter_category;
    // I021/132, 5.2.20: dBm, two's complement.
    std::optional<std::int8_t> message_amplitude_dbm;

    // Every item present, in FRN order, with its octets.
    std::vector<Cat021Item> items;
};

// Parses one data block: CAT 21, LEN, then records until LEN is used up.
// Fails on a wrong category, a LEN that disagrees with the span, an FSPEC
// naming an FRN Table 1 does not use, or an item running past the block.
[[nodiscard]] Expected<std::vector<Cat021Record>> asterix_cat021_parse(
    std::span<const std::uint8_t> block);

}  // namespace revenant::decode
