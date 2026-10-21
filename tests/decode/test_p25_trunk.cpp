// P25 voice grants resolved to frequencies, from TSBKs built field by field
// from the TIA-102.AABC-B figures: Figure 4.2.1-1 (GRP_V_CH_GRANT, opcode
// $00), Figure 4.2.2-1 (GRP_V_CH_GRANT_UPDT, $02), Figure 4.2.3-1
// (GRP_V_CH_GRANT_UPDT_EXP, $03) and Figure 6.2.9-1 (IDEN_UP, $3D), with the
// frequency arithmetic of clause 2.3.9.2.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

#include "core/decode/p25_trunk.h"
#include "core/decode/p25_tsbk.h"

using namespace revenant;

namespace {

using Fields = std::array<std::uint8_t, 10>;

decode::P25Tsbk tsbk_of(const Fields& fields) {
    return decode::p25_tsbk_from_octets(decode::p25_tsbk_with_crc(fields));
}

std::uint16_t channel(std::uint8_t iden, std::uint16_t number) {
    return static_cast<std::uint16_t>((iden << 12U) | (number & 0x0FFFU));
}

Fields grant(std::uint8_t options, std::uint16_t ch, std::uint16_t group, std::uint32_t source) {
    return Fields{0x00, 0x00, options,
                  static_cast<std::uint8_t>(ch >> 8U), static_cast<std::uint8_t>(ch),
                  static_cast<std::uint8_t>(group >> 8U), static_cast<std::uint8_t>(group),
                  static_cast<std::uint8_t>(source >> 16U),
                  static_cast<std::uint8_t>(source >> 8U), static_cast<std::uint8_t>(source)};
}

Fields grant_update(std::uint16_t ch1, std::uint16_t g1, std::uint16_t ch2, std::uint16_t g2) {
    return Fields{0x02, 0x00,
                  static_cast<std::uint8_t>(ch1 >> 8U), static_cast<std::uint8_t>(ch1),
                  static_cast<std::uint8_t>(g1 >> 8U), static_cast<std::uint8_t>(g1),
                  static_cast<std::uint8_t>(ch2 >> 8U), static_cast<std::uint8_t>(ch2),
                  static_cast<std::uint8_t>(g2 >> 8U), static_cast<std::uint8_t>(g2)};
}

Fields grant_update_explicit(std::uint8_t options, std::uint16_t tx, std::uint16_t rx,
                             std::uint16_t group) {
    return Fields{0x03, 0x00, options, 0x00,
                  static_cast<std::uint8_t>(tx >> 8U), static_cast<std::uint8_t>(tx),
                  static_cast<std::uint8_t>(rx >> 8U), static_cast<std::uint8_t>(rx),
                  static_cast<std::uint8_t>(group >> 8U), static_cast<std::uint8_t>(group)};
}

// Figure 6.2.9-1: Identifier 4, BW 9, Transmit Offset 9, Channel Spacing 10,
// Base Frequency 32 bits.
Fields identifier_update(std::uint8_t iden, std::uint16_t bw, std::uint16_t offset,
                         std::uint16_t spacing, std::uint32_t base) {
    std::uint64_t bits = iden & 0xFU;
    bits = (bits << 9U) | (bw & 0x1FFU);
    bits = (bits << 9U) | (offset & 0x1FFU);
    bits = (bits << 10U) | (spacing & 0x3FFU);
    bits = (bits << 32U) | base;
    Fields f{0x3D, 0x00};
    for (std::size_t i = 0; i < 8; ++i) {
        f[2 + i] = static_cast<std::uint8_t>(bits >> (56U - 8U * i));
    }
    return f;
}

// An 800 MHz band plan: base 851.0125 MHz in 5 Hz units, 12.5 kHz spacing
// (100 units of 0.125 kHz), 12.5 kHz bandwidth.
constexpr std::uint32_t kBase = 851'012'500 / 5;
constexpr std::uint64_t kBaseHz = 851'012'500;

}  // namespace

TEST_CASE("p25 trunk: a grant before its identifier has no frequency", "[p25_trunk]") {
    decode::P25TrunkReader reader;
    const auto grants = reader.read(tsbk_of(grant(0x00, channel(1, 10), 3344, 1234567)));
    REQUIRE(grants.size() == 1);
    CHECK(grants[0].group == 3344);
    CHECK(grants[0].source == 1234567U);
    CHECK_FALSE(grants[0].frequency_hz.has_value());
    CHECK(grants[0].initial);
}

TEST_CASE("p25 trunk: identifier update resolves later grants", "[p25_trunk]") {
    decode::P25TrunkReader reader;
    CHECK(reader.read(tsbk_of(identifier_update(1, 100, 0, 100, kBase))).empty());
    REQUIRE(reader.table().knows(1));

    // Service Options bit 7 emergency, bit 6 protected, bits 2-0 priority
    // (AABC-B clause 2.3.24).
    const auto grants = reader.read(tsbk_of(grant(0xC5, channel(1, 163), 501, 42)));
    REQUIRE(grants.size() == 1);
    CHECK(grants[0].frequency_hz == kBaseHz + 163ULL * 12'500ULL);
    CHECK(grants[0].options_known);
    CHECK(grants[0].encrypted);
    CHECK(grants[0].emergency);
    CHECK(grants[0].priority == 5);
}

TEST_CASE("p25 trunk: grant update carries two calls and no options", "[p25_trunk]") {
    decode::P25TrunkReader reader;
    static_cast<void>(reader.read(tsbk_of(identifier_update(2, 100, 0, 100, kBase))));
    const auto grants =
        reader.read(tsbk_of(grant_update(channel(2, 1), 100, channel(2, 2), 200)));
    REQUIRE(grants.size() == 2);
    CHECK(grants[0].group == 100);
    CHECK(grants[0].frequency_hz == kBaseHz + 12'500ULL);
    CHECK(grants[1].group == 200);
    CHECK(grants[1].frequency_hz == kBaseHz + 25'000ULL);
    CHECK_FALSE(grants[0].options_known);
    CHECK_FALSE(grants[0].initial);
    CHECK_FALSE(grants[0].source.has_value());

    const auto repeated =
        reader.read(tsbk_of(grant_update(channel(2, 1), 100, channel(2, 1), 100)));
    CHECK(repeated.size() == 1);
}

TEST_CASE("p25 trunk: explicit update follows Channel (R)", "[p25_trunk]") {
    decode::P25TrunkReader reader;
    static_cast<void>(reader.read(tsbk_of(identifier_update(3, 100, 0, 100, kBase))));
    const auto grants =
        reader.read(tsbk_of(grant_update_explicit(0x00, channel(3, 7), channel(3, 9), 77)));
    REQUIRE(grants.size() == 1);
    CHECK(grants[0].channel == decode::P25Channel{3, 9});
    CHECK(grants[0].frequency_hz == kBaseHz + 9ULL * 12'500ULL);
    CHECK(grants[0].options_known);
    CHECK_FALSE(grants[0].encrypted);
}

TEST_CASE("p25 trunk: a failed CRC or reset yields nothing", "[p25_trunk]") {
    decode::P25TrunkReader reader;
    static_cast<void>(reader.read(tsbk_of(identifier_update(1, 100, 0, 100, kBase))));
    auto octets = decode::p25_tsbk_with_crc(grant(0x00, channel(1, 1), 1, 1));
    octets[5] ^= 0x01U;
    CHECK(reader.read(decode::p25_tsbk_from_octets(octets)).empty());

    reader.reset();
    CHECK_FALSE(reader.table().knows(1));
}
