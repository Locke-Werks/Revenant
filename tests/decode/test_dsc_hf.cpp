// DSC on MF and HF: core/decode/dsc.h's decoder at clause 1.3.1's tones and
// rate, against core/dsp/synth/dsc_hf_mod.h's J2B audio. The code, the
// formats and time diversity are tested once, in test_dsc.cpp; this file
// covers what changes with band.
//
// Error rates are measured and reported in WARN lines with loose assertions,
// for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "core/decode/dsc_hf.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dsc_hf_mod.h"
#include "core/dsp/synth/dsc_mod.h"
#include "core/dsp/synth/rds_mod.h"

using namespace revenant;

namespace {

double mean_power(const std::vector<float>& x) {
    double sum = 0.0;
    for (const float v : x) {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return x.empty() ? 0.0 : sum / static_cast<double>(x.size());
}

decode::DscFrequency hf_frequency(std::uint64_t hz) {
    decode::DscFrequency f;
    f.kind = decode::DscFrequency::Kind::Frequency;
    f.value = hz;
    return f;
}

std::vector<decode::DscCall> calls(std::uint64_t salt = 0) {
    std::vector<decode::DscCall> all;
    {
        // Table A1-4.1, an MF/HF distress alert.
        decode::DscCall c;
        c.format = decode::kDscFormatDistress;
        c.self_id = 232001000 + salt;
        c.nature_of_distress = 101;
        c.distress_position = decode::DscPosition{50.5, -1.25};
        c.utc_hhmm = 1234;
        c.subsequent_communications = 109;
        c.eos = decode::kDscEos;
        all.push_back(c);
    }
    {
        // An individual routine call to a ship, J3E on a working frequency
        // pair of Table A1-5's 100 Hz units.
        decode::DscCall c;
        c.format = decode::kDscFormatIndividual;
        c.address = 211234560;
        c.category = decode::kDscCategoryRoutine;
        c.self_id = 2320040 + salt;
        c.telecommand1 = 109;
        c.telecommand2 = 126;
        c.frequencies = {hf_frequency(8'291'000), hf_frequency(8'291'000)};
        c.eos = decode::kDscEosAcknowledgeRq;
        all.push_back(c);
    }
    {
        // An all ships safety call.
        decode::DscCall c;
        c.format = decode::kDscFormatAllShips;
        c.category = decode::kDscCategorySafety;
        c.self_id = 2190000 + salt;
        c.telecommand1 = 109;
        c.telecommand2 = 126;
        c.frequencies = {hf_frequency(4'125'000)};
        c.eos = decode::kDscEos;
        all.push_back(c);
    }
    return all;
}

std::vector<std::vector<std::uint8_t>> bits_of(const std::vector<decode::DscCall>& sent) {
    std::vector<std::vector<std::uint8_t>> out;
    for (const auto& c : sent) {
        auto information = siggen::dsc_encode(c);
        REQUIRE(information.has_value());
        out.push_back(siggen::dsc_hf_bits(*information, decode::dsc_hf_dot_bits(c, false)));
    }
    return out;
}

std::vector<decode::DscCall> decode_audio(const decode::DscConfig& config,
                                          const std::vector<float>& audio, std::size_t block = 0,
                                          decode::DscStats* stats = nullptr) {
    auto decoder = decode::DscDecoder::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::DscCall> out;
    const std::size_t step = block == 0 ? audio.size() : block;
    for (std::size_t i = 0; i < audio.size(); i += step) {
        decoder->process(std::span<const float>(audio.data() + i, std::min(step, audio.size() - i)),
                         out);
    }
    if (stats != nullptr) {
        *stats = decoder->stats();
    }
    return out;
}

std::size_t count_received(const std::vector<decode::DscCall>& got,
                           const std::vector<decode::DscCall>& sent) {
    std::size_t n = 0;
    for (const auto& s : sent) {
        auto info = siggen::dsc_encode(s);
        REQUIRE(info.has_value());
        n += static_cast<std::size_t>(std::ranges::any_of(
            got, [&](const decode::DscCall& g) { return g.symbols == *info; }));
    }
    return n;
}

}  // namespace

TEST_CASE("clause 1.3.1's tones and clause 3.4's dot patterns", "[decode][dsc_hf]") {
    CHECK(decode::kDscHfYHz == 1615);
    CHECK(decode::kDscHfBHz == 1785);
    const auto usb = decode::dsc_hf_config(48000, decode::DscHfSideband::Usb);
    const auto lsb = decode::dsc_hf_config(48000, decode::DscHfSideband::Lsb);
    CHECK(usb.y_hz < usb.b_hz);  // clause 1.4: B is the higher tone
    CHECK(lsb.y_hz > lsb.b_hz);
    CHECK(usb.bit_rate == 100.0);
    const auto c = calls();
    CHECK(decode::dsc_hf_dot_bits(c[0], true) == 200);   // distress alert
    CHECK(decode::dsc_hf_dot_bits(c[1], false) == 200);  // to a ship
    CHECK(decode::dsc_hf_dot_bits(c[1], true) == 20);    // to a coast station
    const auto info = siggen::dsc_encode(c[1]);
    REQUIRE(info.has_value());
    CHECK(siggen::dsc_hf_bits(*info, 200).size() ==
          200 + 10 * siggen::dsc_air_symbols(*info).size());
}

TEST_CASE("MF/HF DSC round trips on both sidebands and at two rates", "[decode][dsc_hf]") {
    const auto sent = calls();
    for (const auto sideband : {decode::DscHfSideband::Usb, decode::DscHfSideband::Lsb}) {
        for (const dsp::SampleRate rate : {48000, 11025}) {
            INFO("rate " << rate << (sideband == decode::DscHfSideband::Usb ? " USB" : " LSB"));
            siggen::DscHfModConfig mod;
            mod.rate = rate;
            mod.sideband = sideband;
            auto audio = siggen::dsc_hf_render_audio(mod, bits_of(sent));
            REQUIRE(audio.has_value());
            const auto got = decode_audio(decode::dsc_hf_config(rate, sideband), *audio);
            REQUIRE(got.size() == sent.size());
            CHECK(count_received(got, sent) == sent.size());
            CHECK(got[0].distress_position.has_value());
            CHECK(got[1].frequencies.size() == 2);
            CHECK(got[1].frequencies[0].value == 8'291'000);
        }
    }
}

TEST_CASE("MF/HF DSC tolerates clause 1.3.1's 30 ppm and 20-bit dot patterns",
          "[decode][dsc_hf]") {
    const auto sent = calls();
    std::vector<std::vector<std::uint8_t>> bits;
    for (const auto& c : sent) {
        auto information = siggen::dsc_encode(c);
        REQUIRE(information.has_value());
        bits.push_back(siggen::dsc_hf_bits(*information, decode::kDscHfShortDotBits));
    }
    for (const double ppm : {-30e-6, 30e-6}) {
        siggen::DscHfModConfig mod;
        mod.bit_rate_error = ppm;
        auto audio = siggen::dsc_hf_render_audio(mod, bits);
        REQUIRE(audio.has_value());
        CHECK(count_received(decode_audio(decode::dsc_hf_config(48000), *audio), sent) ==
              sent.size());
    }
}

TEST_CASE("MF/HF DSC output does not depend on how the audio is blocked", "[decode][dsc_hf]") {
    auto audio = siggen::dsc_hf_render_audio(siggen::DscHfModConfig{}, bits_of(calls()));
    REQUIRE(audio.has_value());
    const auto config = decode::dsc_hf_config(48000);
    const auto whole = decode_audio(config, *audio);
    REQUIRE(whole.size() == 3);
    for (const std::size_t block : {std::size_t{1}, std::size_t{333}, std::size_t{4800}}) {
        const auto split = decode_audio(config, *audio, block);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].symbols == whole[i].symbols);
            CHECK(split[i].first_sample == whole[i].first_sample);
            CHECK(split[i].last_sample == whole[i].last_sample);
        }
    }
}

TEST_CASE("MF/HF DSC call loss against noise, measured", "[decode][dsc_hf]") {
    std::vector<decode::DscCall> sent;
    for (std::uint64_t r = 0; r < 10; ++r) {
        for (const auto& c : calls(r * 7)) {
            sent.push_back(c);
        }
    }
    struct Point {
        double snr_2500_db;
        double allowed_loss;
    };
    // Measured 2026-11-02 at 48000 S/s, noise seed 0xD5CF, 30 calls a point;
    // no call lost at 6 or -3 dB in 2500 Hz (8 characters taken from RX at
    // -3), 20 of 30 lost at -6 dB. The allowances sit above the measurements.
    const Point points[] = {{6.0, 0.0}, {-3.0, 0.1}, {-6.0, 0.8}};
    for (const Point& p : points) {
        auto audio = siggen::dsc_hf_render_audio(siggen::DscHfModConfig{}, bits_of(sent));
        REQUIRE(audio.has_value());
        REQUIRE(siggen::add_real_awgn(*audio, mean_power(*audio),
                                      siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db), 48'000,
                                      0xD5CFULL)
                    .has_value());
        decode::DscStats stats;
        const auto got = decode_audio(decode::dsc_hf_config(48000), *audio, 0, &stats);
        const std::size_t good = count_received(got, sent);
        const double loss = 1.0 - static_cast<double>(good) / static_cast<double>(sent.size());
        int from_rx = 0;
        for (const auto& g : got) {
            from_rx += g.from_rx;
        }
        CHECK(loss <= p.allowed_loss);
        CHECK(got.size() == good);
        WARN("DSC MF/HF SNR " << p.snr_2500_db << " dB/2500 Hz: call loss " << loss << " of "
                              << sent.size() << ", " << from_rx << " characters from RX, "
                              << stats.ecc_failures << " ECC failures");
    }
}
