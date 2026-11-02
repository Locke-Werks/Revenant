// The generic turbo code: the CCSDS interleaver, the encoder's termination,
// and the iterative decoder over BPSK in Gaussian noise.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "core/decode/turbo.h"

using namespace revenant;

namespace {

constexpr std::array<std::uint32_t, 8> kPrimes{31, 37, 43, 47, 53, 59, 61, 67};

decode::TurboCode rate_third(std::size_t k1, std::size_t k2) {
    auto pi = decode::ccsds_turbo_permutation(k1, k2, kPrimes);
    REQUIRE(pi.has_value());
    decode::TurboCodeSpec spec;
    spec.permutation = *pi;
    // X, Y0, Y'0: rate 1/3.
    spec.data_pattern = {{1, 1, 0, 0, 1, 0}};
    spec.tail_pattern = {{{1, 1, 0, 0, 0, 0},
                          {1, 1, 0, 0, 0, 0},
                          {1, 1, 0, 0, 0, 0},
                          {0, 0, 0, 1, 1, 0},
                          {0, 0, 0, 1, 1, 0},
                          {0, 0, 0, 1, 1, 0}}};
    auto code = decode::TurboCode::create(spec);
    REQUIRE(code.has_value());
    return *code;
}

std::vector<float> bpsk_awgn(const std::vector<std::uint8_t>& bits, double sigma,
                             std::mt19937& rng) {
    std::normal_distribution<double> n(0.0, sigma);
    std::vector<float> llr;
    for (const std::uint8_t b : bits) {
        const double y = (b != 0 ? -1.0 : 1.0) + n(rng);
        llr.push_back(static_cast<float>(2.0 * y / (sigma * sigma)));
    }
    return llr;
}

}  // namespace

TEST_CASE("CCSDS turbo interleaver", "[turbo]") {
    // Every Table 4 shape of M.2092-2 used by VDES ASM is a permutation.
    for (const auto [k1, k2] : {std::pair{2U, 144U}, std::pair{2U, 336U}, std::pair{4U, 264U}}) {
        auto pi = decode::ccsds_turbo_permutation(k1, k2, kPrimes);
        REQUIRE(pi.has_value());
        CHECK(pi->size() == k1 * k2);
    }
    CHECK(!decode::ccsds_turbo_permutation(3, 10, kPrimes).has_value());
    CHECK(!decode::ccsds_turbo_permutation(2, 0, kPrimes).has_value());
}

TEST_CASE("Turbo code rejects malformed specifications", "[turbo]") {
    decode::TurboCodeSpec spec;
    CHECK(!decode::TurboCode::create(spec).has_value());
    spec.permutation = {0, 0};
    spec.data_pattern = {{1, 0, 0, 0, 0, 0}};
    CHECK(!decode::TurboCode::create(spec).has_value());
    spec.permutation = {1, 0};
    spec.tail_pattern[0] = {0, 0, 0, 1, 0, 0};
    CHECK(!decode::TurboCode::create(spec).has_value());
    spec.tail_pattern[0] = {4, 0, 0, 0, 0, 0};
    CHECK(!decode::TurboCode::create(spec).has_value());
    spec.tail_pattern[0] = {1, 0, 0, 0, 0, 0};
    CHECK(decode::TurboCode::create(spec).has_value());
}

TEST_CASE("Turbo encoder is linear and terminates", "[turbo]") {
    const auto code = rate_third(2, 144);
    CHECK(code.encoded_bits() == 3 * 288 + 12);
    std::vector<std::uint8_t> zeros(288, 0);
    const auto z = code.encode(zeros);
    CHECK(std::all_of(z.begin(), z.end(), [](std::uint8_t b) { return b == 0; }));
    std::mt19937 rng(1);
    std::vector<std::uint8_t> a(288), b(288), ab(288);
    for (std::size_t i = 0; i < 288; ++i) {
        a[i] = static_cast<std::uint8_t>(rng() & 1U);
        b[i] = static_cast<std::uint8_t>(rng() & 1U);
        ab[i] = a[i] ^ b[i];
    }
    const auto ea = code.encode(a);
    const auto eb = code.encode(b);
    const auto eab = code.encode(ab);
    for (std::size_t i = 0; i < eab.size(); ++i) {
        CHECK(eab[i] == (ea[i] ^ eb[i]));
    }
}

TEST_CASE("Turbo decoder over BPSK in noise", "[turbo]") {
    const auto code = rate_third(2, 336);
    std::mt19937 rng(42);
    constexpr int kBlocks = 40;
    // Eb/N0 1.5 dB at rate about 1/3: sigma^2 = 1 / (2 R Eb/N0).
    const double rate = 672.0 / static_cast<double>(code.encoded_bits());
    const double sigma = std::sqrt(1.0 / (2.0 * rate * std::pow(10.0, 1.5 / 10.0)));
    int failed = 0;
    std::size_t iterations = 0;
    for (int blk = 0; blk < kBlocks; ++blk) {
        std::vector<std::uint8_t> u(672);
        for (auto& v : u) {
            v = static_cast<std::uint8_t>(rng() & 1U);
        }
        const auto llr = bpsk_awgn(code.encode(u), sigma, rng);
        const auto got = code.decode(llr, 10, [&](std::span<const std::uint8_t> d) {
            return std::equal(d.begin(), d.end(), u.begin());
        });
        iterations += got.iterations;
        if (got.bits != u) {
            ++failed;
        }
    }
    UNSCOPED_INFO("block errors " << failed << " of " << kBlocks << ", mean iterations "
                                  << static_cast<double>(iterations) / kBlocks);
    CHECK(failed <= 2);
}
