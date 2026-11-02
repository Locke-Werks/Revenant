// LDPC: the generic decoder and encoder of core/decode/ldpc.h, on a (7,4)
// Hamming code written as an LDPC code and on a random (3,6)-regular code of
// length 600 built here with a fixed seed.
//
// Error rates are measured and printed and the assertions around them are
// loose, as everywhere in this directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "core/decode/ldpc.h"

using namespace revenant;

namespace {

using Rows = std::vector<std::vector<std::uint32_t>>;
using Dense = std::vector<std::vector<std::uint8_t>>;

// Parity bits 4, 5 and 6 cover data bits {0,1,2}, {0,1,3} and {0,2,3}.
Rows hamming_checks() { return {{0, 1, 2, 4}, {0, 1, 3, 5}, {0, 2, 3, 6}}; }

Dense hamming_generator() {
    return {
        {1, 0, 0, 0, 1, 1, 1},
        {0, 1, 0, 0, 1, 1, 0},
        {0, 0, 1, 0, 1, 0, 1},
        {0, 0, 0, 1, 0, 1, 1},
    };
}

struct Code {
    Rows checks;
    Dense generator;
    std::size_t n = 0;
};

// A (3,6)-regular H by shuffling edge sockets until no check meets a variable
// twice, then reduced over GF(2) to find a systematic generator. The columns
// are relabelled so the information positions come first, which describes the
// same code with its bits in a different order.
Code regular_code(std::size_t n, std::uint32_t seed) {
    const std::size_t m = n / 2;
    std::mt19937 rng(seed);
    std::vector<std::uint32_t> sockets(3 * n);
    for (std::size_t i = 0; i < sockets.size(); ++i) {
        sockets[i] = static_cast<std::uint32_t>(i / 3);
    }
    Rows checks;
    for (;;) {
        std::shuffle(sockets.begin(), sockets.end(), rng);
        checks.assign(m, {});
        bool ok = true;
        for (std::size_t c = 0; c < m && ok; ++c) {
            std::set<std::uint32_t> seen(sockets.begin() + static_cast<std::ptrdiff_t>(6 * c),
                                         sockets.begin() + static_cast<std::ptrdiff_t>(6 * c + 6));
            ok = seen.size() == 6;
            checks[c].assign(seen.begin(), seen.end());
        }
        if (ok) {
            break;
        }
    }

    Dense h(m, std::vector<std::uint8_t>(n, 0));
    for (std::size_t c = 0; c < m; ++c) {
        for (const std::uint32_t v : checks[c]) {
            h[c][v] = 1;
        }
    }
    // Reduced row echelon form.
    std::vector<std::size_t> pivots;
    std::size_t row = 0;
    for (std::size_t col = 0; col < n && row < m; ++col) {
        std::size_t r = row;
        while (r < m && h[r][col] == 0U) {
            ++r;
        }
        if (r == m) {
            continue;
        }
        std::swap(h[r], h[row]);
        for (std::size_t o = 0; o < m; ++o) {
            if (o != row && h[o][col] != 0U) {
                for (std::size_t j = 0; j < n; ++j) {
                    h[o][j] ^= h[row][j];
                }
            }
        }
        pivots.push_back(col);
        ++row;
    }
    std::vector<std::size_t> free;
    for (std::size_t col = 0, p = 0; col < n; ++col) {
        if (p < pivots.size() && pivots[p] == col) {
            ++p;
        } else {
            free.push_back(col);
        }
    }
    const std::size_t k = free.size();
    std::vector<std::uint32_t> position(n);
    for (std::size_t i = 0; i < k; ++i) {
        position[free[i]] = static_cast<std::uint32_t>(i);
    }
    for (std::size_t i = 0; i < pivots.size(); ++i) {
        position[pivots[i]] = static_cast<std::uint32_t>(k + i);
    }

    Code code;
    code.n = n;
    for (auto& check : checks) {
        for (auto& v : check) {
            v = position[v];
        }
    }
    code.checks = std::move(checks);
    // Setting free column f to one forces pivot r to h[r][f].
    code.generator.assign(k, std::vector<std::uint8_t>(n, 0));
    for (std::size_t i = 0; i < k; ++i) {
        code.generator[i][i] = 1;
        for (std::size_t r = 0; r < pivots.size(); ++r) {
            code.generator[i][k + r] = h[r][free[i]];
        }
    }
    return code;
}

std::vector<std::uint8_t> random_bits(std::size_t count, std::mt19937& rng) {
    std::vector<std::uint8_t> out(count);
    for (auto& b : out) {
        b = static_cast<std::uint8_t>(rng() & 1U);
    }
    return out;
}

}  // namespace

TEST_CASE("LDPC: Hamming code words pass the checks and decode in zero iterations",
          "[decode][ldpc]") {
    auto decoder = decode::LdpcDecoder::create({.variables = 7, .checks = hamming_checks()});
    REQUIRE(decoder.has_value());
    auto encoder = decode::LdpcEncoder::create(hamming_generator());
    REQUIRE(encoder.has_value());

    for (unsigned word = 0; word < 16; ++word) {
        std::vector<std::uint8_t> info(4);
        for (unsigned i = 0; i < 4; ++i) {
            info[i] = static_cast<std::uint8_t>((word >> i) & 1U);
        }
        std::vector<std::uint8_t> cw(7);
        encoder->encode(info, cw);
        REQUIRE(decoder->syndrome_zero(cw));
        std::vector<float> llr(7);
        for (std::size_t i = 0; i < 7; ++i) {
            llr[i] = cw[i] != 0U ? -3.0F : 3.0F;
        }
        std::vector<std::uint8_t> out(7);
        const auto result = decoder->decode(llr, out);
        CHECK(result.converged);
        CHECK(result.iterations == 0);
        CHECK(out == cw);
    }
}

TEST_CASE("LDPC: a weak wrong bit is corrected in every Hamming position",
          "[decode][ldpc]") {
    for (const auto rule :
         {decode::LdpcCheckRule::NormalisedMinSum, decode::LdpcCheckRule::OffsetMinSum}) {
        auto decoder = decode::LdpcDecoder::create(
            {.variables = 7, .checks = hamming_checks(), .rule = rule});
        REQUIRE(decoder.has_value());
        auto encoder = decode::LdpcEncoder::create(hamming_generator());
        REQUIRE(encoder.has_value());
        const std::vector<std::uint8_t> info{1, 0, 1, 1};
        std::vector<std::uint8_t> cw(7);
        encoder->encode(info, cw);
        for (std::size_t flip = 0; flip < 7; ++flip) {
            std::vector<float> llr(7);
            for (std::size_t i = 0; i < 7; ++i) {
                const float mag = i == flip ? -1.0F : 4.0F;
                llr[i] = cw[i] != 0U ? -mag : mag;
            }
            std::vector<std::uint8_t> out(7);
            const auto result = decoder->decode(llr, out);
            CHECK(result.converged);
            CHECK(result.iterations >= 1);
            CHECK(out == cw);
        }
    }
}

TEST_CASE("LDPC: hard errors at known positions in the (3,6) code are corrected",
          "[decode][ldpc]") {
    const Code code = regular_code(600, 0x1D9C'0001U);
    auto decoder = decode::LdpcDecoder::create({.variables = code.n, .checks = code.checks});
    REQUIRE(decoder.has_value());
    auto encoder = decode::LdpcEncoder::create(code.generator);
    REQUIRE(encoder.has_value());

    std::mt19937 rng(7);
    const auto info = random_bits(encoder->information_bits(), rng);
    std::vector<std::uint8_t> cw(code.n);
    encoder->encode(info, cw);
    REQUIRE(decoder->syndrome_zero(cw));

    for (const std::size_t errors : {1U, 4U, 12U}) {
        std::vector<float> llr(code.n);
        for (std::size_t i = 0; i < code.n; ++i) {
            llr[i] = cw[i] != 0U ? -1.0F : 1.0F;
        }
        // Spread out, so no two errors share a check by construction alone.
        for (std::size_t e = 0; e < errors; ++e) {
            llr[(e * 47 + 11) % code.n] *= -1.0F;
        }
        std::vector<std::uint8_t> out(code.n);
        const auto result = decoder->decode(llr, out);
        CHECK(result.converged);
        CHECK(out == cw);
    }
}

TEST_CASE("LDPC: the configuration is validated", "[decode][ldpc]") {
    CHECK_FALSE(decode::LdpcDecoder::create({.variables = 7, .checks = {}}).has_value());
    CHECK_FALSE(decode::LdpcDecoder::create({.variables = 7, .checks = {{0, 7}}}).has_value());
    CHECK_FALSE(decode::LdpcDecoder::create({.variables = 7, .checks = {{1, 1}}}).has_value());
    CHECK_FALSE(decode::LdpcDecoder::create({.variables = 7, .checks = {{1}}}).has_value());
    CHECK_FALSE(decode::LdpcDecoder::create(
                    {.variables = 7, .checks = hamming_checks(), .max_iterations = 0})
                    .has_value());
    CHECK_FALSE(
        decode::LdpcDecoder::create({.variables = 7, .checks = hamming_checks(), .scale = 1.5F})
            .has_value());
    CHECK_FALSE(decode::LdpcEncoder::create({{1, 0, 1}, {1, 1, 0}}).has_value());
    CHECK_FALSE(decode::LdpcEncoder::create({{1, 0, 1}, {0, 1}}).has_value());
    CHECK_FALSE(decode::LdpcEncoder::create({{1, 0}, {0, 1}}).has_value());
}

TEST_CASE("LDPC: (3,6) code error rate falls with Eb/N0 over BPSK and AWGN",
          "[decode][ldpc]") {
    const Code code = regular_code(600, 0x1D9C'0001U);
    auto encoder = decode::LdpcEncoder::create(code.generator);
    REQUIRE(encoder.has_value());
    const std::size_t k = encoder->information_bits();
    const double rate = static_cast<double>(k) / static_cast<double>(code.n);

    for (const auto rule :
         {decode::LdpcCheckRule::NormalisedMinSum, decode::LdpcCheckRule::OffsetMinSum}) {
        auto decoder = decode::LdpcDecoder::create(
            {.variables = code.n, .checks = code.checks, .max_iterations = 50, .rule = rule});
        REQUIRE(decoder.has_value());

        std::vector<double> bers;
        std::vector<double> raw;
        for (const double ebn0_db : {1.0, 2.0, 3.0}) {
            std::mt19937 rng(0xA5A5U);
            std::normal_distribution<float> noise(0.0F, 1.0F);
            const double ebn0 = std::pow(10.0, ebn0_db / 10.0);
            const float sigma = static_cast<float>(std::sqrt(1.0 / (2.0 * rate * ebn0)));
            const float to_llr = 2.0F / (sigma * sigma);

            constexpr std::size_t kFrames = 60;
            std::size_t bit_errors = 0;
            std::size_t raw_errors = 0;
            std::size_t converged = 0;
            for (std::size_t f = 0; f < kFrames; ++f) {
                const auto info = random_bits(k, rng);
                std::vector<std::uint8_t> cw(code.n);
                encoder->encode(info, cw);
                std::vector<float> llr(code.n);
                for (std::size_t i = 0; i < code.n; ++i) {
                    const float y = (cw[i] != 0U ? -1.0F : 1.0F) + sigma * noise(rng);
                    llr[i] = to_llr * y;
                    raw_errors += (y < 0.0F) != (cw[i] != 0U) ? 1U : 0U;
                }
                std::vector<std::uint8_t> out(code.n);
                const auto result = decoder->decode(llr, out);
                for (std::size_t i = 0; i < k; ++i) {
                    bit_errors += out[i] != info[i] ? 1U : 0U;
                }
                if (result.converged) {
                    ++converged;
                    // A converged frame of this length decoding to the wrong
                    // code word would be a far rarer event than any this
                    // test can produce.
                    CHECK(out == cw);
                }
            }
            const double ber = static_cast<double>(bit_errors) / static_cast<double>(kFrames * k);
            const double raw_ber =
                static_cast<double>(raw_errors) / static_cast<double>(kFrames * code.n);
            std::printf("ldpc %s Eb/N0 %.1f dB: channel BER %.4f, decoded BER %.6f, "
                        "%zu/%zu frames converged\n",
                        rule == decode::LdpcCheckRule::NormalisedMinSum ? "normalised"
                                                                        : "offset",
                        ebn0_db, raw_ber, ber, converged, kFrames);
            bers.push_back(ber);
            raw.push_back(raw_ber);
        }
        CHECK(bers[1] <= bers[0]);
        CHECK(bers[2] <= bers[1]);
        CHECK(bers[2] < 1e-3);
        CHECK(bers[2] < raw[2]);
    }
}
