// AX.25 over the G3RUH 9600 bit/s modem: the scrambler against Miller's
// description of it, and the decoder against core/dsp/synth/g3ruh_mod.h,
// from audio and through an FM receiver.
//
// Error rates are measured and reported in WARN lines with loose assertions,
// for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/decode/g3ruh.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/fsk_mod.h"
#include "core/dsp/synth/g3ruh_mod.h"
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

double nfm_channel(double hertz, const void*) {
    return std::abs(hertz) <= 8000.0 ? 1.0 : 0.0;
}

decode::Ax25Address addr(const char* call, std::uint8_t ssid) {
    decode::Ax25Address a;
    a.callsign = call;
    a.ssid = ssid;
    return a;
}

std::vector<std::vector<std::uint8_t>> frames(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    std::vector<std::vector<std::uint8_t>> out;
    for (std::size_t i = 0; i < count; ++i) {
        siggen::Ax25FrameSpec spec;
        spec.destination = addr("CQ", 0);
        spec.source = addr("G3RUH", static_cast<std::uint8_t>(i % 16));
        for (int k = 0; k < 64; ++k) {
            spec.information.push_back(static_cast<std::uint8_t>(engine()));
        }
        auto octets = siggen::ax25_frame_octets(spec);
        REQUIRE(octets.has_value());
        out.push_back(*octets);
    }
    return out;
}

std::vector<decode::Ax25Frame> decode_audio(const std::vector<float>& audio, dsp::SampleRate rate,
                                            std::size_t block = 0, decode::Ax25Stats* stats = nullptr) {
    decode::G3ruhConfig config;
    config.rate = rate;
    auto d = decode::G3ruhDecoder::create(config);
    REQUIRE(d.has_value());
    std::vector<decode::Ax25Frame> out;
    const std::size_t step = block == 0 ? audio.size() : block;
    for (std::size_t i = 0; i < audio.size(); i += step) {
        d->process(std::span<const float>(audio.data() + i, std::min(step, audio.size() - i)), out);
    }
    if (stats != nullptr) {
        *stats = d->stats();
    }
    return out;
}

std::size_t count_received(const std::vector<decode::Ax25Frame>& got,
                           const std::vector<std::vector<std::uint8_t>>& sent) {
    std::size_t n = 0;
    for (const auto& s : sent) {
        n += static_cast<std::size_t>(std::ranges::any_of(got, [&](const decode::Ax25Frame& g) {
            return g.information == std::vector<std::uint8_t>(s.end() - 64, s.end());
        }));
    }
    return n;
}

}  // namespace

TEST_CASE("the scrambler is Miller's 1 + X^12 + X^17", "[decode][g3ruh]") {
    // "the currently transmitted bit is the EXOR of the current data bit,
    // plus the bits that were transmitted 12 and 17 bits earlier."
    std::mt19937_64 engine(0x17ULL);
    decode::G3ruhScrambler scrambler;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> sent;
    for (int i = 0; i < 2000; ++i) {
        data.push_back(static_cast<std::uint8_t>(engine() & 1U));
        sent.push_back(scrambler.push(data.back()));
    }
    for (std::size_t i = 17; i < sent.size(); ++i) {
        REQUIRE(sent[i] == (data[i] ^ sent[i - 12] ^ sent[i - 17]));
    }
    // A maximal length register: all-ones data through a cleared register
    // repeats only after 2^17 - 1 bits.
    decode::G3ruhScrambler lfsr;
    std::vector<std::uint8_t> run;
    for (int i = 0; i < (1 << 17) + 200; ++i) {
        run.push_back(lfsr.push(1));
    }
    std::size_t period = 0;
    for (std::size_t p = 1; p + 80 <= run.size() && period == 0; ++p) {
        if (std::equal(run.begin() + 40, run.begin() + 80, run.begin() + 40 + static_cast<std::ptrdiff_t>(p))) {
            period = p;
        }
    }
    CHECK(period == (1U << 17) - 1);

    // "The unscrambler perforce requires 17 bits to synchronise", and is
    // indifferent to where it starts.
    decode::G3ruhUnscrambler un;
    for (int i = 0; i < 5; ++i) {
        (void)un.push(1);
    }
    for (std::size_t i = 0; i < sent.size(); ++i) {
        const std::uint8_t out = un.push(sent[i]);
        if (i >= 17) {
            REQUIRE(out == data[i]);
        }
    }
}

TEST_CASE("G3RUH round trips at two rates, either polarity", "[decode][g3ruh]") {
    const auto sent = frames(8, 1);
    for (const dsp::SampleRate rate : {48000, 96000}) {
        for (const double sign : {1.0, -1.0}) {
            INFO("rate " << rate << " polarity " << sign);
            siggen::G3ruhModConfig mod;
            mod.rate = rate;
            mod.amplitude = 0.5 * sign;
            auto audio = siggen::g3ruh_render_audio(mod, siggen::g3ruh_bits(mod, sent));
            REQUIRE(audio.has_value());
            decode::Ax25Stats stats;
            const auto got = decode_audio(*audio, rate, 0, &stats);
            CHECK(got.size() == sent.size());
            CHECK(count_received(got, sent) == sent.size());
            CHECK(stats.fcs_failures == 0);
        }
    }
}

TEST_CASE("G3RUH output does not depend on how the audio is blocked", "[decode][g3ruh]") {
    const auto sent = frames(4, 2);
    siggen::G3ruhModConfig mod;
    auto audio = siggen::g3ruh_render_audio(mod, siggen::g3ruh_bits(mod, sent));
    REQUIRE(audio.has_value());
    const auto whole = decode_audio(*audio, 48000);
    REQUIRE(whole.size() == sent.size());
    for (const std::size_t block : {std::size_t{1}, std::size_t{97}, std::size_t{4096}}) {
        const auto split = decode_audio(*audio, 48000, block);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].information == whole[i].information);
            CHECK(split[i].first_sample == whole[i].first_sample);
            CHECK(split[i].last_sample == whole[i].last_sample);
        }
    }
}

TEST_CASE("G3RUH frame loss against noise on the audio, measured", "[decode][g3ruh]") {
    const auto sent = frames(40, 3);
    struct Point {
        double snr_2500_db;
        double allowed_loss;
    };
    // Measured 2026-11-02 at 48000 S/s, noise seed 0x9600, 40 frames of 81
    // octets: none lost at 20 dB in 2500 Hz, 8 at 16 dB, 38 at 13 dB. The
    // bit error rate before the unscrambler was 0 at 20 dB and 3.2e-4 at
    // 16 dB, at 48000 and at 96000 S/s alike. The allowances sit above.
    const Point points[] = {{20.0, 0.0}, {16.0, 0.3}, {13.0, 1.0}};
    for (const Point& p : points) {
        siggen::G3ruhModConfig mod;
        auto audio = siggen::g3ruh_render_audio(mod, siggen::g3ruh_bits(mod, sent));
        REQUIRE(audio.has_value());
        REQUIRE(siggen::add_real_awgn(*audio, mean_power(*audio),
                                      siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db), 48'000,
                                      0x9600ULL)
                    .has_value());
        decode::Ax25Stats stats;
        const auto got = decode_audio(*audio, 48000, 0, &stats);
        const double loss =
            1.0 - static_cast<double>(count_received(got, sent)) / static_cast<double>(sent.size());
        CHECK(loss <= p.allowed_loss);
        WARN("G3RUH audio SNR " << p.snr_2500_db << " dB/2500 Hz: frame loss " << loss << " of "
                                << sent.size() << ", FCS failures " << stats.fcs_failures);
    }
}

TEST_CASE("G3RUH through an FM receiver, measured", "[decode][g3ruh]") {
    const auto sent = frames(40, 4);
    struct Point {
        double snr_2500_db;
        double allowed_loss;
    };
    // Measured 2026-11-02 as above, the FM signal at +/- 3 kHz with noise on
    // the complex baseband, an nfm receiver's 8 kHz either side and its
    // discriminator: none lost at 25 dB, 6 of 40 at 18 dB.
    const Point points[] = {{25.0, 0.0}, {18.0, 0.3}};
    for (const Point& p : points) {
        siggen::G3ruhModConfig mod;
        auto rf = siggen::g3ruh_render_baseband(mod, siggen::g3ruh_bits(mod, sent));
        REQUIRE(rf.has_value());
        REQUIRE(siggen::add_awgn(*rf, siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db), 48'000,
                                 0x9601ULL)
                    .has_value());
        auto taps = decode::design_from_response(48'000, 127, nfm_channel, nullptr);
        REQUIRE(taps.has_value());
        std::vector<dsp::Complex32> filtered(rf->size());
        REQUIRE(decode::filter_complex(*rf, *taps, filtered).has_value());
        std::vector<float> audio(rf->size());
        REQUIRE(decode::fm_discriminate(filtered, audio, 48'000).has_value());
        decode::Ax25Stats stats;
        const auto got = decode_audio(audio, 48000, 0, &stats);
        const double loss =
            1.0 - static_cast<double>(count_received(got, sent)) / static_cast<double>(sent.size());
        CHECK(loss <= p.allowed_loss);
        WARN("G3RUH FM SNR " << p.snr_2500_db << " dB/2500 Hz: frame loss " << loss << " of "
                             << sent.size() << ", FCS failures " << stats.fcs_failures);
    }
}

