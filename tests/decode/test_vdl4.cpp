// VDL Mode 4: ETSI EN 301 842-1's GFSK through a transmitter, noise, a
// 25 kHz receiver's channel filter and discriminator, and back.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/decode/vdl4.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/vdl4_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 96'000;

double vdl4_channel(double hertz, const void*) {
    return std::abs(hertz) <= decode::kVdl4PassbandHz ? 1.0 : 0.0;
}

std::vector<float> receive(std::vector<dsp::Complex32> rf, double snr_2500_db, std::uint64_t seed) {
    double sum = 0.0;
    std::size_t on = 0;
    for (const dsp::Complex32& v : rf) {
        if (std::norm(v) > 0.0F) {
            sum += static_cast<double>(std::norm(v));
            ++on;
        }
    }
    REQUIRE(on > 0);
    auto noise = siggen::awgn_power_for(siggen::NoiseLevel::snr_in_2500_hz_db(snr_2500_db),
                                        sum / static_cast<double>(on), kRate);
    REQUIRE(noise.has_value());
    REQUIRE(siggen::add_awgn_at_power(rf, *noise, seed).has_value());
    auto taps = decode::design_from_response(kRate, 127, vdl4_channel, nullptr);
    REQUIRE(taps.has_value());
    std::vector<dsp::Complex32> filtered(rf.size());
    REQUIRE(decode::filter_complex(rf, *taps, filtered).has_value());
    std::vector<float> audio(rf.size());
    REQUIRE(decode::fm_discriminate(filtered, audio, kRate).has_value());
    return audio;
}

std::vector<decode::Vdl4Burst> decode_audio(const std::vector<float>& audio,
                                            std::size_t block = 0) {
    decode::Vdl4Config config;
    config.rate = kRate;
    auto decoder = decode::Vdl4Decoder::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::Vdl4Burst> out;
    if (block == 0) {
        decoder->process(audio, out);
    } else {
        for (std::size_t i = 0; i < audio.size(); i += block) {
            const std::size_t n = std::min(block, audio.size() - i);
            decoder->process(std::span<const float>(audio.data() + i, n), out);
        }
    }
    return out;
}

// Bursts of 11 to 120 octets, clause 3.1's shortest frame upward, random
// content.
std::vector<std::vector<std::uint8_t>> some_bursts(std::size_t count, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> length(9, 118);
    std::uniform_int_distribution<int> octet(0, 255);
    std::vector<std::vector<std::uint8_t>> out(count);
    for (auto& b : out) {
        b.resize(static_cast<std::size_t>(length(rng)));
        for (auto& v : b) {
            v = static_cast<std::uint8_t>(octet(rng));
        }
    }
    return out;
}

}  // namespace

TEST_CASE("VDL Mode 4 constants are clause 5.2's", "[decode][vdl4]") {
    CHECK(decode::kVdl4DeviationHz == 2400.0);
    CHECK(decode::kVdl4BitRate == 19200.0);
    // Clause 6.2: 16 slots in 212.08 ms at 19 200 symbol/s is 4072 bits,
    // inside the frame bound.
    CHECK(std::llround(0.21208 * decode::kVdl4BitRate) <=
          static_cast<long long>(decode::kVdl4MaximumFrameBits));
}

TEST_CASE("VDL Mode 4 round trips from RF, measured", "[decode][vdl4]") {
    const auto sent = some_bursts(60, 0xD14);
    siggen::Vdl4ModConfig mod;
    mod.rate = kRate;
    auto rf = siggen::vdl4_render(mod, sent);
    REQUIRE(rf.has_value());
    // Measured 2026-11-02, 60 bursts of 11 to 120 octets a point, seed
    // 0x4D4, at 96 kS/s through a 9 kHz passband: burst error rate 0 at 80
    // dB in 2500 Hz, 0.10 at 40, 0.37 at 35, 0.95 at 30, 1 at 24. Integrating
    // over half a bit gave 0.17, 0.58 and 0.95, over a whole bit 0.42, 0.87
    // and 0.97: at BT 0.28 the eye is closed enough by the transmit filter
    // that more smoothing costs more than it saves. A passband of 12.5 kHz
    // lost 0.97 at 30 dB with a one-bit boxcar, as 9 kHz did. This is far
    // short of clause 6.1.4's sensitivity; see core/decode/vdl4.h.
    for (const double snr : {40.0, 35.0, 30.0}) {
        const auto audio = receive(*rf, snr, 0x4D4ULL);
        const auto got = decode_audio(audio);
        std::size_t matched = 0;
        std::size_t j = 0;
        for (const auto& s : sent) {
            if (j < got.size() && got[j].octets == s) {
                ++matched;
                ++j;
            }
        }
        const double loss = 1.0 - static_cast<double>(matched) / static_cast<double>(sent.size());
        WARN("VDL4 SNR " << snr << " dB/2500 Hz: burst error rate " << loss);
        // Every burst reported is one that was sent.
        CHECK(got.size() == matched);
        if (snr >= 40.0) {
            CHECK(loss <= 0.15);
        }
    }
}

TEST_CASE("VDL Mode 4 output does not depend on how the audio is blocked", "[decode][vdl4]") {
    const auto sent = some_bursts(8, 0xB10C);
    siggen::Vdl4ModConfig mod;
    mod.rate = kRate;
    auto rf = siggen::vdl4_render(mod, sent);
    REQUIRE(rf.has_value());
    const auto audio = receive(std::move(*rf), 40.0, 0xB1ULL);
    const auto whole = decode_audio(audio);
    REQUIRE_FALSE(whole.empty());
    for (const std::size_t block : {std::size_t{1}, std::size_t{317}, std::size_t{4096}}) {
        const auto split = decode_audio(audio, block);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].octets == whole[i].octets);
            CHECK(split[i].first_sample == whole[i].first_sample);
            CHECK(split[i].last_sample == whole[i].last_sample);
        }
    }
}

TEST_CASE("the VDL Mode 4 decoder refuses fewer than three samples a bit", "[decode][vdl4]") {
    decode::Vdl4Config config;
    config.rate = 48000;
    CHECK_FALSE(decode::Vdl4Decoder::create(config).has_value());
    config.rate = decode::kVdl4MinimumRate;
    CHECK(decode::Vdl4Decoder::create(config).has_value());
}
