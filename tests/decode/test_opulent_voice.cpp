// Opulent Voice: the base-40 station identifier against the test cases of
// OPV 2.3, the coding chain against OPV 8.4 to 8.6's statements of it, and the
// deframer against core/dsp/synth/opulent_voice_mod.h through an antipodal
// channel. No MSK; see core/decode/opulent_voice.h.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/decode/opulent_voice.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/opulent_voice_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kBitRate = 54200;  // 2168 bits per 40 ms, OPV 8.1 and 8.7

std::vector<std::array<std::uint8_t, decode::kOpvFrameBytes>> sample_frames(std::size_t count) {
    std::vector<std::array<std::uint8_t, decode::kOpvFrameBytes>> out;
    std::uint64_t state = 0x0F0F;
    for (std::size_t f = 0; f < count; ++f) {
        std::vector<std::uint8_t> payload(decode::kOpvPayloadBytes);
        for (std::uint8_t& byte : payload) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            byte = static_cast<std::uint8_t>(state >> 45U);
        }
        auto frame = siggen::opv_make_frame("KB5MU-11", payload);
        REQUIRE(frame.has_value());
        out.push_back(*frame);
    }
    return out;
}

std::vector<float> soft_of(std::span<const siggen::Complex32> samples) {
    std::vector<float> out;
    out.reserve(samples.size());
    for (const auto& s : samples) {
        out.push_back(s.real());
    }
    return out;
}

std::vector<decode::OpvFrame> deframe(std::span<const float> soft, std::size_t block) {
    auto deframer = decode::OpvDeframer::create(decode::OpvConfig{});
    REQUIRE(deframer.has_value());
    std::vector<decode::OpvFrame> frames;
    for (std::size_t i = 0; i < soft.size(); i += block) {
        REQUIRE(deframer->process(soft.subspan(i, std::min(block, soft.size() - i)), frames)
                    .has_value());
    }
    return frames;
}

std::vector<float> noise_soft(std::size_t bits, std::uint64_t seed) {
    std::vector<siggen::Complex32> noise(bits, siggen::Complex32{0.0F, 0.0F});
    REQUIRE(siggen::add_awgn_at_power(noise, 1.0, seed).has_value());
    return soft_of(noise);
}

}  // namespace

TEST_CASE("OPV station identifiers are the test cases of OPV 2.3", "[decode][opulent_voice]") {
    struct Case {
        const char* station;
        std::uint64_t value;
    };
    const std::array<Case, 8> cases = {{
        {"W1AW", 0x0000001680b7ULL},
        {"KB5MU-11", 0x0447b6864a5bULL},
        {"W5NYV.NCS", 0x71c06f55a697ULL},
        {"VE7ABC/W1", 0xaa764d576f5eULL},
        {"W3/G1ABC", 0x007463900847ULL},
        {"K0K", 0x000000004903ULL},
        {"A", 0x000000000001ULL},
        {"OFD4BS.-BA", 0xffffffffffffULL},
    }};
    for (const Case& c : cases) {
        const auto value = decode::opv_encode_station(c.station);
        REQUIRE(value.has_value());
        CHECK(*value == c.value);
        CHECK(decode::opv_decode_station(c.value) == c.station);
    }
    CHECK_FALSE(decode::opv_encode_station("W1AW!").has_value());
    CHECK_FALSE(decode::opv_encode_station("OFD4BS.-BB").has_value());
}

TEST_CASE("the OPV randomiser, interleaver and sync follow OPV 8.2 to 8.6",
          "[decode][opulent_voice]") {
    // 8.4: period 255 bits, reset per frame, self-inverse.
    std::vector<std::uint8_t> zeros(255 * 2, 0);
    decode::opv_randomize(zeros);
    CHECK(zeros[0] == 0xFF);  // the seed comes out first, MSB first
    std::vector<std::uint8_t> bits;
    for (const std::uint8_t byte : zeros) {
        for (int i = 7; i >= 0; --i) {
            bits.push_back(static_cast<std::uint8_t>((byte >> i) & 1U));
        }
    }
    for (std::size_t i = 0; i + 255 < bits.size(); ++i) {
        CHECK(bits[i] == bits[i + 255]);
    }
    // 8.6: "input bits 0, 1, 2, 3... appear at output positions 0, 67, 134, 201".
    CHECK(decode::opv_interleave_position(0) == 0);
    CHECK(decode::opv_interleave_position(1) == 67);
    CHECK(decode::opv_interleave_position(2) == 134);
    CHECK(decode::opv_interleave_position(3) == 201);
    std::set<std::size_t> seen;
    for (std::size_t p = 0; p < decode::kOpvEncodedBits; ++p) {
        seen.insert(decode::opv_interleave_position(p));
    }
    CHECK(seen.size() == decode::kOpvEncodedBits);
    CHECK(*seen.rbegin() == decode::kOpvEncodedBits - 1);
    // 8.2 calls 0x02B8DB "balanced"; it has eleven ones and thirteen zeros.
    // The value is what is implemented.
    CHECK(std::popcount(decode::kOpvSync) == 11);
    // 8.7: 2168 bits, sync first.
    const auto frames = sample_frames(1);
    const auto encoded = decode::opv_encode_frame(frames[0]);
    REQUIRE(encoded.has_value());
    CHECK(encoded->size() == decode::kOpvFrameBits);
    CHECK(decode::pack_bits_msb(*encoded, 0, decode::kOpvSyncBits) == decode::kOpvSync);
}

TEST_CASE("OPV frames round trip through an antipodal channel", "[decode][opulent_voice]") {
    const auto frames = sample_frames(10);
    auto bits = siggen::opv_frame_bits(frames);
    REQUIRE(bits.has_value());
    // Noise either side so the deframer has to find the first sync.
    auto lead = noise_soft(1000, 1);
    auto samples = siggen::opv_render_antipodal(*bits);
    SECTION("clean") {
        std::vector<float> soft = lead;
        const auto body = soft_of(samples);
        soft.insert(soft.end(), body.begin(), body.end());
        const auto out = deframe(soft, soft.size());
        REQUIRE(out.size() == frames.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            CHECK(out[i].bytes == frames[i]);
            CHECK(out[i].header.station == "KB5MU-11");
            CHECK(out[i].header.token == decode::kOpvDefaultToken);
            CHECK(out[i].first_bit == 1000 + i * decode::kOpvFrameBits);
            CHECK_FALSE(out[i].inverted);
        }
    }
    SECTION("inverted") {
        std::vector<float> soft = lead;
        for (const auto& s : samples) {
            soft.push_back(-s.real());
        }
        const auto out = deframe(soft, 333);
        REQUIRE(out.size() == frames.size());
        CHECK(out.front().inverted);
        CHECK(out.back().bytes == frames.back());
    }
}

TEST_CASE("the OPV deframer does not depend on how its input is blocked",
          "[decode][opulent_voice]") {
    const auto frames = sample_frames(6);
    auto bits = siggen::opv_frame_bits(frames);
    REQUIRE(bits.has_value());
    auto samples = siggen::opv_render_antipodal(*bits);
    REQUIRE(siggen::add_awgn(samples, siggen::NoiseLevel::snr_in_reference_bandwidth_db(3.0, kBitRate),
                             kBitRate, 0x0B10C)
                .has_value());
    const auto soft = soft_of(samples);
    const auto whole = deframe(soft, soft.size());
    const auto small = deframe(soft, 101);
    REQUIRE(whole.size() == small.size());
    for (std::size_t i = 0; i < whole.size(); ++i) {
        CHECK(whole[i].first_bit == small[i].first_bit);
        CHECK(whole[i].bytes == small[i].bytes);
    }
}

TEST_CASE("OPV frame error rates against noise, measured", "[decode][opulent_voice]") {
    const auto frames = sample_frames(40);
    auto bits = siggen::opv_frame_bits(frames);
    REQUIRE(bits.has_value());
    for (const double snr : {6.0, 3.0, 1.0, 0.0, -1.0}) {
        auto samples = siggen::opv_render_antipodal(*bits);
        REQUIRE(siggen::add_awgn(samples,
                                 siggen::NoiseLevel::snr_in_reference_bandwidth_db(snr, kBitRate),
                                 kBitRate, 0x0F7)
                    .has_value());
        const auto out = deframe(soft_of(samples), 4096);
        std::size_t good = 0;
        double least = 1.0;
        for (const auto& frame : out) {
            least = std::min(least, frame.agreement);
            good += std::find(frames.begin(), frames.end(), frame.bytes) != frames.end() ? 1U : 0U;
        }
        std::printf("OPV %4.1f dB per coded bit: %zu frames out, %zu of 40 exact, least "
                    "agreement %.3f\n",
                    snr, out.size(), good, least);
        if (snr >= 3.0) {
            CHECK(good == 40);
        }
        // No CRC in the frame (OPV 2.1): below about 1 dB a frame can pass
        // the agreement check with bit errors in it, and that is reported,
        // not hidden.
        if (snr >= 1.0) {
            CHECK(good + 1 >= out.size());
        }
    }
}

TEST_CASE("the OPV deframer does not start on noise", "[decode][opulent_voice]") {
    // 60 s at 54200 bit/s.
    const auto soft = noise_soft(54200 * 60, 0x40153);
    const auto out = deframe(soft, 65536);
    double highest = 0.0;
    for (const auto& frame : out) {
        highest = std::max(highest, frame.agreement);
    }
    std::printf("OPV on 60 s of noise: %zu frames, highest agreement %.3f\n", out.size(),
                highest);
    CHECK(out.empty());
}
