// The generic CRC against the published catalogue check values, and against
// the two hand-written CRCs already in the tree.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <vector>

#include "core/decode/ax25.h"
#include "core/decode/crc.h"
#include "core/decode/m17.h"

using namespace revenant::decode;

namespace {

constexpr std::uint8_t kCheck[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

std::uint64_t check_value(const CrcParams& params) {
    auto crc = Crc::create(params);
    REQUIRE(crc.has_value());
    return crc->compute(kCheck);
}

std::vector<std::uint8_t> random_octets(std::mt19937& rng, std::size_t count) {
    std::vector<std::uint8_t> out(count);
    for (auto& o : out) {
        o = static_cast<std::uint8_t>(rng());
    }
    return out;
}

}  // namespace

TEST_CASE("CRC catalogue check values", "[decode][crc]") {
    using namespace crc_catalogue;
    CHECK(check_value(kCrc16CcittFalse) == 0x29B1);
    CHECK(check_value(kCrc16X25) == 0x906E);
    CHECK(check_value(kCrc16Kermit) == 0x2189);
    CHECK(check_value(kCrc32) == 0xCBF43926);
    CHECK(check_value(kCrc24OpenPgp) == 0x21CF02);
    CHECK(check_value(kCrc14Darc) == 0x082D);
    CHECK(check_value(kCrc14Gsm) == 0x30AE);
    CHECK(check_value(kCrc12Dect) == 0xF5B);
    CHECK(check_value(kCrc12Umts) == 0xDAF);
    CHECK(check_value(kCrc12Cdma2000) == 0xD4D);
    CHECK(check_value(kCrc12Gsm) == 0xB34);
    CHECK(check_value(kCrc3Gsm) == 0x4);
    CHECK(check_value(kCrc5Usb) == 0x19);
    CHECK(check_value(kCrc64Xz) == 0x995DC9BBDF1939FAULL);
}

TEST_CASE("CRC is usable in a constant expression", "[decode][crc]") {
    constexpr Crc crc(crc_catalogue::kCrc16CcittFalse);
    constexpr std::uint8_t octets[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    static_assert(crc.compute(octets) == 0x29B1);
}

TEST_CASE("CRC reproduces ax25_fcs and m17_crc", "[decode][crc]") {
    const Crc x25(crc_catalogue::kCrc16X25);
    const Crc m17(CrcParams{16, kM17CrcPolynomial, kM17CrcInitial, false, false, 0});
    std::mt19937 rng(17);
    for (std::size_t length = 0; length < 64; ++length) {
        const auto octets = random_octets(rng, length);
        CHECK(x25.compute(octets) == ax25_fcs(octets));
        CHECK(m17.compute(octets) == m17_crc(octets));
    }
}

TEST_CASE("CRC bit entry point matches octets and spans split points", "[decode][crc]") {
    using namespace crc_catalogue;
    std::mt19937 rng(5);
    for (const CrcParams& params : {kCrc16CcittFalse, kCrc16X25, kCrc32, kCrc24OpenPgp, kCrc14Darc,
                                    kCrc12Umts, kCrc3Gsm, kCrc5Usb, kCrc64Xz}) {
        const Crc crc(params);
        const auto octets = random_octets(rng, 11);
        std::vector<std::uint8_t> bits;
        for (const std::uint8_t o : octets) {
            for (unsigned i = 0; i < 8; ++i) {
                const unsigned shift = params.reflect_in ? i : 7U - i;
                bits.push_back(static_cast<std::uint8_t>((o >> shift) & 1U));
            }
        }
        const std::uint64_t whole = crc.compute(octets);
        CHECK(crc.compute_bits(bits) == whole);
        // Bits for a non-octet prefix then octets for the rest.
        auto state = crc.update_bits(crc.start(), std::span(bits).first(24));
        state = crc.update(state, std::span(octets).subspan(3));
        CHECK(crc.finish(state) == whole);
    }
}

TEST_CASE("CRC create refuses bad parameters", "[decode][crc]") {
    CHECK_FALSE(Crc::create(CrcParams{0, 0, 0, false, false, 0}).has_value());
    CHECK_FALSE(Crc::create(CrcParams{65, 0, 0, false, false, 0}).has_value());
    CHECK_FALSE(Crc::create(CrcParams{12, 0x1080F, 0, false, false, 0}).has_value());
    CHECK_FALSE(Crc::create(CrcParams{12, 0x80F, 0x1000, false, false, 0}).has_value());
}
