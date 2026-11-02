// The Fano sequential decoder on WSPR-length blocks: 50 data bits and a
// 31-bit flush through the K = 32 rate 1/2 code whose generators G4JNT's
// "WSPR Coding Process" publishes.
//
// Error rates are measured and printed; the assertions around them are loose.

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "core/decode/fano.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

constexpr std::uint32_t kG0 = 0xF2D05351U;
constexpr std::uint32_t kG1 = 0xE4613C47U;
constexpr std::size_t kData = 50;
constexpr std::size_t kTail = 31;

FanoConfig wspr_config() {
    FanoConfig config;
    config.generator0 = kG0;
    config.generator1 = kG1;
    config.tail_bits = kTail;
    return config;
}

std::vector<std::uint8_t> encode(const std::vector<std::uint8_t>& bits) {
    std::vector<std::uint8_t> out;
    std::uint32_t reg = 0;
    for (const auto b : bits) {
        reg = (reg << 1U) | b;
        out.push_back(static_cast<std::uint8_t>(std::popcount(reg & kG0) & 1));
        out.push_back(static_cast<std::uint8_t>(std::popcount(reg & kG1) & 1));
    }
    return out;
}

struct Trial {
    std::size_t decoded = 0;
    std::size_t correct = 0;
    std::size_t blocks = 0;
};

Trial run(double ebn0_db, std::size_t blocks, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    const double esn0 = std::pow(10.0, ebn0_db / 10.0) * 0.5;
    const double sigma = std::sqrt(1.0 / (2.0 * esn0));
    std::normal_distribution<double> gauss(0.0, sigma);
    Trial t;
    t.blocks = blocks;
    for (std::size_t k = 0; k < blocks; ++k) {
        std::vector<std::uint8_t> bits(kData + kTail, 0);
        for (std::size_t i = 0; i < kData; ++i) {
            bits[i] = static_cast<std::uint8_t>(engine() & 1U);
        }
        const auto coded = encode(bits);
        std::vector<float> llr(coded.size());
        for (std::size_t i = 0; i < coded.size(); ++i) {
            const double r = (coded[i] != 0 ? -1.0 : 1.0) + gauss(engine);
            llr[i] = static_cast<float>(2.0 * r / (sigma * sigma));
        }
        auto result = fano_decode(wspr_config(), llr);
        REQUIRE(result.has_value());
        if (result->decoded) {
            ++t.decoded;
            REQUIRE(result->bits.size() == bits.size());
            if (result->bits == bits) {
                ++t.correct;
            }
        } else {
            CHECK(result->cycles == wspr_config().max_cycles_per_bit * bits.size());
        }
    }
    return t;
}

}  // namespace

TEST_CASE("Fano decodes a clean WSPR-length block", "[decode][fano]") {
    std::vector<std::uint8_t> bits(kData + kTail, 0);
    for (std::size_t i = 0; i < kData; ++i) {
        bits[i] = static_cast<std::uint8_t>((i * 7 + 3) % 5 < 2);
    }
    const auto coded = encode(bits);
    std::vector<float> llr(coded.size());
    for (std::size_t i = 0; i < coded.size(); ++i) {
        llr[i] = coded[i] != 0 ? -4.0F : 4.0F;
    }
    auto result = fano_decode(wspr_config(), llr);
    REQUIRE(result.has_value());
    REQUIRE(result->decoded);
    CHECK(result->bits == bits);
    CHECK(result->cycles == bits.size());
}

TEST_CASE("Fano decodes WSPR-length blocks at moderate SNR", "[decode][fano]") {
    const auto t = run(4.0, 300, 0xFA40ULL);
    std::printf("Fano K=32, Eb/N0 4 dB: %zu/%zu decoded, %zu correct\n", t.decoded, t.blocks,
                t.correct);
    CHECK(t.correct >= t.blocks * 95 / 100);
}

TEST_CASE("Fano gives up gracefully at very low SNR", "[decode][fano]") {
    const auto t = run(-3.0, 40, 0xFA03ULL);
    std::printf("Fano K=32, Eb/N0 -3 dB: %zu/%zu decoded, %zu correct\n", t.decoded, t.blocks,
                t.correct);
    CHECK(t.decoded < t.blocks / 2);
}

TEST_CASE("Fano refuses malformed calls", "[decode][fano]") {
    std::vector<float> llr(5, 1.0F);
    CHECK_FALSE(fano_decode(wspr_config(), llr).has_value());
    auto bad = wspr_config();
    bad.delta = 0;
    std::vector<float> even(162, 1.0F);
    CHECK_FALSE(fano_decode(bad, even).has_value());
}
