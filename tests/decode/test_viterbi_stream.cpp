// The streaming Viterbi decoder against the block encoder in dv_codes.h, and
// its bit error rate against the textbook curve for the K = 7 rate 1/2 code.
//
// Error rates are measured and printed; the assertions around them are loose.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/decode/viterbi_stream.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

StreamingViterbiConfig ccsds_config(std::size_t traceback = 0) {
    StreamingViterbiConfig config;
    config.memory = kCcsdsK7Memory;
    config.generators = {kCcsdsK7G1, kCcsdsK7G2};
    config.traceback = traceback;
    return config;
}

std::vector<std::uint8_t> random_bits(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    std::vector<std::uint8_t> bits(count);
    for (auto& b : bits) {
        b = static_cast<std::uint8_t>(engine() & 1U);
    }
    return bits;
}

std::vector<std::uint8_t> encode(std::span<const std::uint8_t> bits) {
    const std::uint32_t gens[] = {kCcsdsK7G1, kCcsdsK7G2};
    auto encoded = convolutional_encode({kCcsdsK7Memory, gens}, bits);
    REQUIRE(encoded.has_value());
    return *encoded;
}

// BPSK at unit amplitude, zero sent as +1, over AWGN at the given Eb/N0 for
// code rate `rate`.
std::vector<float> channel(std::span<const std::uint8_t> coded, double ebn0_db, double rate,
                           std::uint64_t seed) {
    const double esn0 = std::pow(10.0, ebn0_db / 10.0) * rate;
    const double sigma = std::sqrt(1.0 / (2.0 * esn0));
    std::mt19937_64 engine(seed);
    std::normal_distribution<double> gauss(0.0, sigma);
    std::vector<float> soft(coded.size());
    for (std::size_t i = 0; i < coded.size(); ++i) {
        soft[i] = static_cast<float>((coded[i] != 0 ? -1.0 : 1.0) + gauss(engine));
    }
    return soft;
}

std::vector<float> noiseless(std::span<const std::uint8_t> coded) {
    std::vector<float> soft(coded.size());
    for (std::size_t i = 0; i < coded.size(); ++i) {
        soft[i] = coded[i] != 0 ? -1.0F : 1.0F;
    }
    return soft;
}

std::size_t errors(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    std::size_t e = 0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        e += a[i] != b[i] ? 1U : 0U;
    }
    return e;
}

std::vector<std::uint8_t> decode_all(StreamingViterbiConfig config, std::span<const float> soft,
                                     std::size_t chunk) {
    auto v = StreamingViterbi::create(std::move(config));
    REQUIRE(v.has_value());
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < soft.size(); i += chunk) {
        v->push(soft.subspan(i, std::min(chunk, soft.size() - i)), out);
    }
    v->flush(false, out);
    return out;
}

}  // namespace

TEST_CASE("streaming Viterbi round-trips a clean stream", "[decode][viterbi]") {
    const auto bits = random_bits(2000, 1);
    const auto soft = noiseless(encode(bits));
    const auto out = decode_all(ccsds_config(), soft, 4096);
    REQUIRE(out == bits);
}

TEST_CASE("streaming Viterbi releases each bit exactly traceback steps late",
          "[decode][viterbi]") {
    for (const std::size_t traceback : {std::size_t{12}, std::size_t{35}, std::size_t{96}}) {
        auto v = StreamingViterbi::create(ccsds_config(traceback));
        REQUIRE(v.has_value());
        REQUIRE(v->latency() == traceback);
        const auto bits = random_bits(500, 2);
        const auto soft = noiseless(encode(bits));
        std::vector<std::uint8_t> out;
        for (std::size_t step = 0; step < bits.size(); ++step) {
            v->push(std::span<const float>(soft).subspan(step * 2, 2), out);
            const std::size_t expect = step + 1 > traceback ? step + 1 - traceback : 0;
            REQUIRE(out.size() == expect);
        }
        v->flush(false, out);
        REQUIRE(out == bits);
    }
    auto def = StreamingViterbi::create(ccsds_config());
    REQUIRE(def.has_value());
    CHECK(def->latency() == 35);
}

TEST_CASE("streaming Viterbi output does not depend on chunking", "[decode][viterbi]") {
    const auto bits = random_bits(5000, 3);
    const auto soft = channel(encode(bits), 2.0, 0.5, 33);
    const auto whole = decode_all(ccsds_config(), soft, soft.size());
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7}, std::size_t{128}}) {
        CHECK(decode_all(ccsds_config(), soft, chunk) == whole);
    }
}

TEST_CASE("streaming Viterbi honours CCSDS G2 inversion", "[decode][viterbi]") {
    const auto bits = random_bits(1000, 4);
    auto coded = encode(bits);
    for (std::size_t i = 1; i < coded.size(); i += 2) {
        coded[i] ^= 1U;
    }
    auto config = ccsds_config();
    config.invert_mask = kCcsdsK7InvertMask;
    REQUIRE(decode_all(config, noiseless(coded), 100) == bits);
}

TEST_CASE("streaming Viterbi decodes punctured rate 2/3 and 3/4", "[decode][viterbi]") {
    // Textbook puncturing of the K = 7 mother code: rate 2/3 keeps 3 of 4
    // coded bits, rate 3/4 keeps 4 of 6.
    struct Case {
        std::vector<std::uint8_t> pattern;
        double rate;
    };
    const Case cases[] = {{{1, 1, 0, 1}, 2.0 / 3.0}, {{1, 1, 0, 1, 1, 0}, 3.0 / 4.0}};
    for (const auto& c : cases) {
        const auto bits = random_bits(20000, 5);
        const auto sent = puncture_bits(encode(bits), c.pattern);
        auto config = ccsds_config(96);
        config.puncture = c.pattern;
        CHECK(decode_all(config, noiseless(sent), 999) == bits);

        const auto soft = channel(sent, 6.0, c.rate, 55);
        const auto out = decode_all(config, soft, 777);
        REQUIRE(out.size() == bits.size());
        const double ber = static_cast<double>(errors(out, bits)) / static_cast<double>(bits.size());
        std::printf("punctured rate %.3f at Eb/N0 6 dB: BER %.2e\n", c.rate, ber);
        CHECK(ber < 1e-3);
    }
}

TEST_CASE("streaming Viterbi K=7 soft BER sits on the textbook curve", "[decode][viterbi]") {
    // Soft-decision K = 7 rate 1/2 is about 1e-5 near 4.5 dB and a few 1e-3
    // at 3 dB. Uncoded BPSK at 4.5 dB is 6e-3.
    struct Point {
        double ebn0;
        std::size_t bits;
        double ceiling;
    };
    const Point points[] = {{3.0, 200000, 1e-2}, {4.5, 2000000, 1e-4}};
    for (const auto& p : points) {
        const auto bits = random_bits(p.bits, 6);
        const auto soft = channel(encode(bits), p.ebn0, 0.5, 66);
        const auto out = decode_all(ccsds_config(), soft, 65536);
        REQUIRE(out.size() == bits.size());
        const double ber =
            static_cast<double>(errors(out, bits)) / static_cast<double>(bits.size());
        std::printf("K=7 r=1/2 soft, Eb/N0 %.1f dB: BER %.2e over %zu bits\n", p.ebn0, ber,
                    p.bits);
        CHECK(ber < p.ceiling);
    }
}

TEST_CASE("streaming Viterbi refuses malformed configurations", "[decode][viterbi]") {
    auto none = ccsds_config();
    none.generators.clear();
    CHECK_FALSE(StreamingViterbi::create(none).has_value());
    auto odd = ccsds_config();
    odd.puncture = {1, 1, 0};
    CHECK_FALSE(StreamingViterbi::create(odd).has_value());
    auto empty = ccsds_config();
    empty.puncture = {0, 0};
    CHECK_FALSE(StreamingViterbi::create(empty).has_value());
}
