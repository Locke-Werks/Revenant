// JT4: G4JNT's "The JT4 Coding Process" (July 2009), whose last page lists
// the channel symbols for " G4JNT IO90IV", and the receiver against the
// transmitter in core/dsp/synth/wsjt_slow_mod.h.
//
// Decode rates are measured and reported in WARN lines with loose
// assertions, for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/jt4.h"
#include "core/dsp/synth/wsjt_slow_mod.h"

using namespace revenant;

namespace {

void add_noise(std::vector<float>& x, double amplitude, double snr_db, std::uint32_t seed) {
    const double s = amplitude * amplitude / 2.0;
    const double n2500 = s / std::pow(10.0, snr_db / 10.0);
    const double sigma = std::sqrt(n2500 * 6000.0 / 2500.0);
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, sigma);
    for (auto& v : x) v += static_cast<float>(g(rng));
}

constexpr std::size_t kSlot = 60 * 12000;

}  // namespace

TEST_CASE("JT4 channel symbols match the document's listing", "[jt4]") {
    // "; Message data ' G4JNT IO90IV'", four symbols a byte, MSB first.
    const std::uint8_t listing[] = {
        0x20, 0xDA, 0x3E, 0x50, 0xCC, 0xAA, 0x2D, 0x20, 0x00, 0x82, 0x65, 0x34, 0xC5,
        0xD4, 0x4A, 0xE1, 0x25, 0xF4, 0x06, 0xC0, 0x75, 0x96, 0x18, 0x14, 0x6C, 0xEE,
        0x55, 0xC4, 0xC7, 0xBB, 0x37, 0x86, 0xF3, 0xF4, 0xA3, 0x45, 0x29, 0xD9, 0xD9,
        0xF2, 0x40, 0xF1, 0x63, 0x03, 0x5F, 0xCB, 0x48, 0x16, 0x8C, 0x71, 0x54, 0xCC,
    };
    std::vector<int> want;
    for (const auto b : listing) {
        for (int sh = 6; sh >= 0; sh -= 2) want.push_back((b >> sh) & 3);
    }
    want.resize(207);
    auto bits = decode::wsjt72_pack_text(" G4JNT IO90IV");
    REQUIRE(bits);
    CHECK(decode::jt4_channel_tones(*bits) == want);
    // "As a sanity-check, for JT4g ... Tone zero lies at 797.959Hz", "tone
    // spacing is 315Hz".
    CHECK(std::abs(decode::jt4_conventional_tone0_hz(6) - 797.959) < 0.001);
    CHECK(std::abs(decode::jt4_protocol(6).tone_spacing_hz - 315.0) < 1e-9);
    // "207 symbols takes around 47 seconds".
    const auto& p = decode::jt4_protocol(0);
    CHECK(std::abs(p.symbols * p.symbol_seconds - 47.3) < 0.1);
}

TEST_CASE("JT4A decodes several signals with offsets in one slot", "[jt4]") {
    struct Sig {
        const char* text;
        double f;
        double dt;
    };
    const Sig sigs[] = {
        {"G3LTF DL9KR JO40", 400.0, 1.0}, {"K1ABC W9XYZ EN37", 700.3, 0.4},
        {" G4JNT IO90IV", 1000.7, 2.0},   {"TNX BOB 73 GL", 1300.0, 1.5},
        {"PA9XYZ G4ABC IO91", 1600.0, 0.1}, {"HELLO WORLD", 1900.25, 1.2},
    };
    std::vector<float> slot(kSlot, 0.0F);
    for (const auto& s : sigs) {
        siggen::SlowModConfig c;
        c.frequency_hz = s.f;
        c.start_seconds = s.dt;
        c.amplitude = 0.05;
        REQUIRE(siggen::jt4_add(c, s.text, 0, slot));
    }
    add_noise(slot, 0.05, -18.0, 7);
    auto dec = decode::Jt4Decoder::create({});
    REQUIRE(dec);
    std::vector<decode::Wsjt72Decode> out;
    dec->decode_slot(slot, 0.0, 0, out);
    REQUIRE(out.size() == std::size(sigs));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto& s = sigs[i];
        INFO(s.text);
        CHECK(out[i].message.text == s.text);
        CHECK(std::abs(out[i].frequency_hz - s.f) < 1.0);
        CHECK(std::abs(out[i].dt_seconds - (s.dt - 1.0)) < 0.05);
        CHECK(std::abs(out[i].snr_db + 18.0) < 3.0);
    }
}

TEST_CASE("JT4G decodes with drift at its conventional frequency", "[jt4]") {
    std::vector<float> slot(kSlot, 0.0F);
    siggen::SlowModConfig c;
    c.frequency_hz = decode::jt4_conventional_tone0_hz(6);
    c.amplitude = 0.05;
    c.drift_hz = 3.0;
    REQUIRE(siggen::jt4_add(c, "G3LTF DL9KR JO40", 6, slot));
    add_noise(slot, 0.05, -14.0, 8);
    decode::Jt4Config cfg;
    cfg.submode = 6;
    cfg.max_drift_hz = 6.0;
    auto dec = decode::Jt4Decoder::create(cfg);
    REQUIRE(dec);
    std::vector<decode::Wsjt72Decode> out;
    dec->decode_slot(slot, 0.0, 0, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].message.text == "G3LTF DL9KR JO40");
    CHECK(std::abs(out[0].frequency_hz - c.frequency_hz) < 2.0);
}

TEST_CASE("JT4 stream at 48 kHz is chunk independent", "[jt4]") {
    const double t0 = 1'700'000'040.0 + 25.0;
    const int rate = 48000;
    const std::size_t lead = static_cast<std::size_t>(35.0 * rate);
    std::vector<float> audio(lead + 60 * rate + rate, 0.0F);
    siggen::SlowModConfig c;
    c.rate = rate;
    c.amplitude = 0.1;
    c.frequency_hz = 1200.0;
    c.start_seconds = 35.0 + 1.0;
    REQUIRE(siggen::jt4_add(c, "G3LTF DL9KR JO40", 0, audio));
    add_noise(audio, 0.1 * 2.0, -16.0, 12);
    const auto run = [&](std::uint32_t seed) {
        decode::Jt4Config cfg;
        cfg.rate = rate;
        cfg.utc_of_sample = [t0](dsp::SampleIndex) { return t0; };
        auto dec = decode::Jt4Decoder::create(cfg);
        REQUIRE(dec);
        std::vector<decode::Wsjt72Decode> out;
        std::mt19937 rng(seed);
        std::size_t pos = 0;
        while (pos < audio.size()) {
            const std::size_t n = std::min<std::size_t>(audio.size() - pos, 1 + rng() % 100000);
            REQUIRE(dec->process(std::span<const float>(audio.data() + pos, n), out));
            pos += n;
        }
        return out;
    };
    const auto a = run(1);
    const auto b = run(2);
    REQUIRE(a.size() == 1);
    CHECK(a[0].message.text == "G3LTF DL9KR JO40");
    CHECK(a[0].slot_start_sample == static_cast<long long>(lead));
    REQUIRE(b.size() == 1);
    CHECK(a[0].frequency_hz == b[0].frequency_hz);
    CHECK(a[0].snr_db == b[0].snr_db);
}

TEST_CASE("JT4A decode threshold in AWGN", "[jt4][threshold]") {
    auto dec = decode::Jt4Decoder::create({});
    REQUIRE(dec);
    const char* msgs[] = {"G3LTF DL9KR JO40", "K1ABC W9XYZ EN37", "W9XYZ K1ABC FN42", "PA9XYZ G4ABC IO91",
                          "TNX BOB 73 GL",    "G4ABC PA9XYZ JO22", "HELLO WORLD",     "VK2ABC JA1XYZ PM95"};
    double threshold = 0.0;
    for (int snr = -18; snr >= -25; --snr) {
        int ok = 0;
        int total = 0;
        for (int trial = 0; trial < 2; ++trial) {
            std::vector<float> slot(kSlot, 0.0F);
            for (int i = 0; i < 8; ++i) {
                siggen::SlowModConfig c;
                c.amplitude = 0.05;
                c.frequency_hz = 300.0 + 270.0 * i + 7.0 * trial;
                c.start_seconds = 0.4 + 0.25 * i;
                REQUIRE(siggen::jt4_add(c, msgs[i], 0, slot));
            }
            add_noise(slot, 0.05, snr, static_cast<std::uint32_t>(1300 + snr * 7 + trial));
            std::vector<decode::Wsjt72Decode> out;
            dec->decode_slot(slot, 0.0, 0, out);
            std::set<std::string> got;
            for (const auto& d : out) got.insert(d.message.text);
            for (const char* m : msgs) ok += got.contains(m) ? 1 : 0;
            total += 8;
            for (const auto& g : got) {
                CHECK(std::find_if(std::begin(msgs), std::end(msgs), [&](const char* m) { return g == m; }) != std::end(msgs));
            }
        }
        const double rate = static_cast<double>(ok) / total;
        WARN("JT4A AWGN SNR " << snr << " dB: " << ok << "/" << total << " decoded");
        if (rate >= 0.5) threshold = snr;
        if (snr >= -18) CHECK(rate >= 0.9);
    }
    WARN("JT4A lowest SNR with at least half decoded: " << threshold << " dB");
    CHECK(threshold <= -20.0);
}
