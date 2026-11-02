// The generic BCH codec: generators against the textbook's, exhaustive
// correction on small codes, random patterns up to t, refusal beyond it, and
// agreement with the two BCH decoders already in the tree.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "core/decode/bch.h"
#include "core/decode/dv_codes.h"
#include "core/decode/pocsag.h"

using namespace revenant::decode;

namespace {

BchCodec make(unsigned m, unsigned poly, unsigned d) {
    auto codec = BchCodec::create(BchParams{m, poly, d, {}});
    REQUIRE(codec.has_value());
    return *codec;
}

std::vector<std::uint8_t> codeword(const BchCodec& codec, std::size_t info_bits, std::mt19937& rng) {
    std::vector<std::uint8_t> word(info_bits + codec.check_bits());
    for (std::size_t i = 0; i < info_bits; ++i) {
        word[i] = static_cast<std::uint8_t>(rng() & 1U);
    }
    REQUIRE(codec.encode(std::span(word).first(info_bits), std::span(word).subspan(info_bits)));
    return word;
}

std::uint32_t pack(std::span<const std::uint8_t> bits) {
    std::uint32_t v = 0;
    for (const std::uint8_t b : bits) {
        v = (v << 1U) | b;
    }
    return v;
}

}  // namespace

TEST_CASE("BCH generators match Lin and Costello's table", "[decode][bch]") {
    // (15,7) t=2 over x^4+x+1: g = x^8+x^7+x^6+x^4+1.
    const auto c15 = make(4, 0x13, 5);
    CHECK(c15.k() == 7);
    CHECK(c15.generator() == std::vector<std::uint8_t>{1, 1, 1, 0, 1, 0, 0, 0, 1});
    // (15,5) t=3.
    CHECK(make(4, 0x13, 7).k() == 5);
    // (31,21) over x^5+x^2+1 is POCSAG's: g = 0x769.
    const auto c31 = make(5, 0x25, 5);
    CHECK(c31.k() == 21);
    CHECK(pack(c31.generator()) == 0x769U);
    // (63,16) t=11 and (1023,...) to reach m = 10.
    CHECK(make(6, 0x43, 23).k() == 16);
    CHECK(make(10, 0x409, 5).k() == 1003);
}

TEST_CASE("BCH explicit generator recovers t", "[decode][bch]") {
    auto codec = BchCodec::create(BchParams{5, 0x25, 0, {1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 1}});
    REQUIRE(codec.has_value());
    CHECK(codec->correctable() == 2);
    CHECK(codec->k() == 21);
    // Not a divisor of x^31 + 1.
    CHECK_FALSE(BchCodec::create(BchParams{5, 0x25, 0, {1, 0, 0, 1, 1}}).has_value());
    // Not primitive: x^4 + x^3 + x^2 + x + 1 has order 5.
    CHECK_FALSE(BchCodec::create(BchParams{4, 0x1F, 5, {}}).has_value());
}

TEST_CASE("BCH corrects every single and double error exhaustively", "[decode][bch]") {
    std::mt19937 rng(1);
    for (const auto& [m, poly] : {std::pair{4U, 0x13U}, std::pair{5U, 0x25U}}) {
        const auto codec = make(m, poly, 5);
        for (const std::size_t info : {codec.k(), std::size_t{3}}) {
            const auto clean = codeword(codec, info, rng);
            const std::size_t len = clean.size();
            for (std::size_t a = 0; a < len; ++a) {
                for (std::size_t b = a; b < len; ++b) {
                    auto word = clean;
                    word[a] ^= 1U;
                    if (b != a) {
                        word[b] ^= 1U;
                    }
                    const auto fixed = codec.decode(word);
                    REQUIRE(fixed.has_value());
                    CHECK(*fixed == (a == b ? 1U : 2U));
                    CHECK(word == clean);
                }
            }
        }
    }
}

TEST_CASE("BCH random errors up to t, and beyond t never miscorrect silently often",
          "[decode][bch]") {
    std::mt19937 rng(2);
    struct Case { unsigned m, poly, d; std::size_t info; };
    for (const Case& c : {Case{6, 0x43, 23, 16}, Case{8, 0x11D, 9, 100}, Case{10, 0x409, 11, 500},
                          Case{7, 0x89, 7, 50}}) {
        const auto codec = make(c.m, c.poly, c.d);
        const std::size_t t = codec.correctable();
        std::size_t refused = 0;
        std::size_t beyond_trials = 0;
        for (int trial = 0; trial < 200; ++trial) {
            const auto clean = codeword(codec, c.info, rng);
            std::vector<std::size_t> pos(clean.size());
            for (std::size_t i = 0; i < pos.size(); ++i) {
                pos[i] = i;
            }
            std::shuffle(pos.begin(), pos.end(), rng);
            const std::size_t errors = rng() % (t + 1);
            auto word = clean;
            for (std::size_t e = 0; e < errors; ++e) {
                word[pos[e]] ^= 1U;
            }
            const auto fixed = codec.decode(word);
            REQUIRE(fixed.has_value());
            CHECK(*fixed == errors);
            CHECK(word == clean);

            // t + 1 errors: either refused, untouched, or moved to some other
            // code word at distance at most t from the received word.
            auto bad = clean;
            for (std::size_t e = 0; e < t + 1; ++e) {
                bad[pos[e]] ^= 1U;
            }
            const auto received = bad;
            ++beyond_trials;
            const auto result = codec.decode(bad);
            if (!result) {
                ++refused;
                CHECK(bad == received);
            } else {
                CHECK(*result <= t);
                CHECK(bad != clean);
                std::vector<std::uint8_t> recheck = bad;
                CHECK(codec.decode(recheck) == std::optional<std::size_t>{0});
            }
        }
        // Shortened codes refuse nearly every word beyond t.
        CHECK(refused * 2 > beyond_trials);
    }
}

TEST_CASE("BCH(31,21) agrees with pocsag_encode and pocsag_correct", "[decode][bch]") {
    const auto codec = make(5, 0x25, 5);
    std::mt19937 rng(3);
    for (int trial = 0; trial < 20; ++trial) {
        const std::uint32_t info = rng() & 0x1FFFFFU;
        const std::uint32_t pocsag = pocsag_encode(info);
        std::vector<std::uint8_t> word(31);
        for (std::size_t i = 0; i < 21; ++i) {
            word[i] = static_cast<std::uint8_t>((info >> (20 - i)) & 1U);
        }
        REQUIRE(codec.encode(std::span(word).first(21), std::span(word).subspan(21)));
        REQUIRE(pack(word) == pocsag >> 1U);

        for (std::size_t a = 0; a < 31; ++a) {
            for (std::size_t b = a; b < 31; ++b) {
                std::uint32_t flip = 1U << (31U - a);
                if (b != a) {
                    flip |= 1U << (31U - b);
                }
                const PocsagCorrection reference = pocsag_correct(pocsag ^ flip);
                auto bits = word;
                bits[a] ^= 1U;
                if (b != a) {
                    bits[b] ^= 1U;
                }
                const auto fixed = codec.decode(bits);
                REQUIRE(reference.valid);
                REQUIRE(fixed.has_value());
                CHECK(pack(bits) == reference.word >> 1U);
                CHECK(static_cast<int>(*fixed) == reference.corrected_bits);
            }
        }
    }
}

TEST_CASE("BCH(63,16) reproduces the P25 NID code words", "[decode][bch]") {
    const auto codec = make(6, 0x43, 23);
    std::mt19937 rng(4);
    for (int trial = 0; trial < 32; ++trial) {
        const auto nac = static_cast<std::uint16_t>(rng() & 0xFFFU);
        const auto duid = static_cast<std::uint8_t>(rng() & 0xFU);
        std::vector<std::uint8_t> nid(64);
        REQUIRE(p25_nid_encode(nac, duid, nid));
        std::vector<std::uint8_t> word(63);
        REQUIRE(codec.encode(std::span(nid).first(16), std::span(word).subspan(16)));
        std::copy_n(nid.begin(), 16, word.begin());
        CHECK(std::equal(word.begin(), word.end(), nid.begin()));

        // Up to eleven errors back to the same NAC and DUID.
        for (std::size_t e = 0; e < 11; ++e) {
            word[(e * 5 + static_cast<std::size_t>(trial)) % 63] ^= 1U;
        }
        const auto fixed = codec.decode(word);
        REQUIRE(fixed.has_value());
        CHECK(*fixed == 11);
        CHECK(std::equal(word.begin(), word.end(), nid.begin()));
    }
}

// GF(2^16) is the largest field the codec takes, and with dvbs not wired only
// GRB reaches it in the tree. The field polynomial is DVB-S2's g1, EN 302
// 307-1 Table 6a, 1 + x^2 + x^3 + x^5 + x^16, and t = 12 gives the 192 check
// bits of its normal frames. Shortened to 2000 information bits to stay quick.
TEST_CASE("BCH over GF(2^16) corrects up to t = 12 on a shortened word", "[decode][bch]") {
    const auto codec = make(16, 0x1002D, 25);
    CHECK(codec.n() == 65535U);
    CHECK(codec.correctable() == 12U);
    CHECK(codec.check_bits() == 192U);
    std::mt19937 rng(16);
    for (int trial = 0; trial < 26; ++trial) {
        const auto clean = codeword(codec, 2000, rng);
        std::vector<std::size_t> pos(clean.size());
        for (std::size_t i = 0; i < pos.size(); ++i) {
            pos[i] = i;
        }
        std::shuffle(pos.begin(), pos.end(), rng);
        const auto errors = static_cast<std::size_t>(trial) % 13;
        auto word = clean;
        for (std::size_t e = 0; e < errors; ++e) {
            word[pos[e]] ^= 1U;
        }
        const auto fixed = codec.decode(word);
        REQUIRE(fixed.has_value());
        CHECK(*fixed == errors);
        CHECK(word == clean);
    }
}
