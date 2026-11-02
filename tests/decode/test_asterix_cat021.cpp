// ASTERIX Category 021: data blocks assembled by hand from the item clauses
// of EUROCONTROL-SPEC-0149-12 Edition 2.7, and what the parser makes of them.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/decode/asterix_cat021.h"

using namespace revenant;

namespace {

// I021/170's eight characters, six bits each, most significant first, from
// the IA-5 subset: A to Z are 1 to 26, space 32, digits 48 to 57.
std::vector<std::uint8_t> ident(std::string_view text) {
    std::uint64_t all = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const char c = i < text.size() ? text[i] : ' ';
        const auto v = static_cast<std::uint64_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 1 : c);
        all = (all << 6U) | (v & 0x3FU);
    }
    std::vector<std::uint8_t> out(6);
    for (std::size_t i = 0; i < 6; ++i) {
        out[i] = static_cast<std::uint8_t>(all >> (40U - 8U * i));
    }
    return out;
}

std::vector<std::uint8_t> block_of(std::vector<std::uint8_t> records) {
    std::vector<std::uint8_t> b = {21, 0, 0};
    b.insert(b.end(), records.begin(), records.end());
    b[1] = static_cast<std::uint8_t>(b.size() >> 8U);
    b[2] = static_cast<std::uint8_t>(b.size());
    return b;
}

std::vector<std::uint8_t> three_records() {
    std::vector<std::uint8_t> r = {
        // FSPEC: FRN 1, 2, 6 | 11 | 16, 21 | 26 | 29.
        0xC5, 0x11, 0x43, 0x09, 0x80,
        // I021/010: SAC, SIC.
        0x19, 0xC9,
        // I021/040: one octet, FX clear.
        0x20,
        // I021/130: latitude 2400000, longitude -50000, LSB 180/2^23.
        0x24, 0x9F, 0x00, 0xFF, 0x3C, 0xB0,
        // I021/080.
        0x4C, 0xA1, 0xB2,
        // I021/140: 1600 * 6.25 ft.
        0x06, 0x40,
        // I021/145: 900 quarter flight levels.
        0x03, 0x84,
        // I021/160: 2048 * 2^-14 NM/s, track 0x4000 * 360/2^16.
        0x08, 0x00, 0x40, 0x00,
    };
    const auto id = ident("BAW123");
    r.insert(r.end(), id.begin(), id.end());
    // Second record: I021/010 alone.
    for (const std::uint8_t v : std::vector<std::uint8_t>{0x80, 0x01, 0x02}) {
        r.push_back(v);
    }
    // Third: I021/010 and the REF, FRN 48, the sixth bit of the seventh
    // FSPEC octet, three octets long by its own length octet.
    for (const std::uint8_t v :
         std::vector<std::uint8_t>{0x81, 0x01, 0x01, 0x01, 0x01, 0x01, 0x04, 0x03, 0x04, 0x03, 0xAA, 0xBB}) {
        r.push_back(v);
    }
    return block_of(r);
}

}  // namespace

TEST_CASE("Cat 021 items decode at the scales clause 5.2 gives", "[decode][asterix]") {
    const auto block = three_records();
    auto parsed = decode::asterix_cat021_parse(block);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 3);
    const decode::Cat021Record& r = (*parsed)[0];
    CHECK(r.sac == 0x19);
    CHECK(r.sic == 0xC9);
    REQUIRE(r.latitude.has_value());
    CHECK(std::abs(*r.latitude - 2400000.0 * 180.0 / 8388608.0) < 1e-12);
    CHECK(std::abs(*r.longitude + 50000.0 * 180.0 / 8388608.0) < 1e-12);
    CHECK(r.target_address == 0x4CA1B2U);
    CHECK(r.geometric_height_ft == 10000.0);
    CHECK(r.flight_level == 225.0);
    CHECK(r.ground_speed_kt == 450.0);
    CHECK(r.track_angle_deg == 90.0);
    CHECK(r.target_identification == "BAW123");
    REQUIRE(r.items.size() == 8);
    CHECK(r.items[1].item == 40);
    CHECK(r.items[1].octets == std::vector<std::uint8_t>{0x20});

    CHECK((*parsed)[1].sac == 1);
    CHECK((*parsed)[1].items.size() == 1);
    const decode::Cat021Record& t = (*parsed)[2];
    REQUIRE(t.items.size() == 2);
    CHECK(t.items[1].frn == 48);
    CHECK(t.items[1].octets == std::vector<std::uint8_t>{0x03, 0xAA, 0xBB});
}

TEST_CASE("Cat 021 refuses blocks clause 4.4 does not allow", "[decode][asterix]") {
    auto good = three_records();
    auto wrong_cat = good;
    wrong_cat[0] = 48;
    CHECK_FALSE(decode::asterix_cat021_parse(wrong_cat).has_value());
    auto short_len = good;
    short_len.pop_back();
    CHECK_FALSE(decode::asterix_cat021_parse(short_len).has_value());
    // FRN 43 is "Not Used" in Table 1: bit 8 of the seventh FSPEC octet.
    const auto unused = block_of({0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x80});
    CHECK_FALSE(decode::asterix_cat021_parse(unused).has_value());
    // I021/130 announced and cut short.
    const auto cut = block_of({0x04, 0x00, 0x00});
    CHECK_FALSE(decode::asterix_cat021_parse(cut).has_value());
}

TEST_CASE("Cat 021 compound items are delimited by their primary subfields",
          "[decode][asterix]") {
    // FSPEC to FRN 31, I021/220: octets 1 to 4 extend, the fifth has bit 6
    // (FRN 31 is the third of 29 to 35). Then I021/220 with WS and TRB, and
    // I021/010 must not be needed after it to find the record's end.
    const auto block = block_of({0x01, 0x01, 0x01, 0x01, 0x20, 0x90, 0x00, 0x2A, 0x05});
    auto parsed = decode::asterix_cat021_parse(block);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
    REQUIRE((*parsed)[0].items.size() == 1);
    CHECK((*parsed)[0].items[0].item == 220);
    CHECK((*parsed)[0].items[0].octets.size() == 4);
}
