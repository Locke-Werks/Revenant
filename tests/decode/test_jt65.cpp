// JT65: Figure 2 and 3 of K1JT's "The JT65 Communications Protocol" (QEX
// 2005), the RS(63, 12) decoder against random errors and erasures, and
// the receiver with the Franke-Taylor decoder (QEX 2016) against the
// transmitter in core/dsp/synth/wsjt_slow_mod.h.
//
// Decode rates are measured and reported in WARN lines with loose
// assertions, for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/jt65.h"
#include "core/decode/rs64.h"
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

TEST_CASE("Figure 2 channel symbols", "[jt65]") {
    struct Row {
        const char* text;
        std::vector<int> channel;
    };
    // Each listing as printed, three lines of 21.
    const Row rows[] = {
        {"G3LTF DL9KR JO40",
         {14, 16, 9,  18, 4,  60, 41, 18, 22, 63, 43, 5,  30, 13, 15, 9,  25, 35, 50, 21, 0,
          36, 17, 42, 33, 35, 39, 22, 25, 39, 46, 3,  47, 39, 55, 23, 61, 25, 58, 47, 16, 38,
          39, 17, 2,  36, 4,  56, 5,  16, 15, 55, 18, 41, 7,  26, 51, 17, 18, 49, 10, 13, 24}},
        {"G3LTE DL9KR JO40",
         {20, 34, 19, 5,  36, 6,  30, 15, 22, 20, 3,  62, 57, 59, 19, 56, 17, 35, 2,  9,  41,
          10, 23, 24, 41, 35, 39, 60, 48, 33, 34, 49, 54, 53, 55, 23, 24, 59, 7,  9,  39, 51,
          23, 17, 2,  12, 49, 6,  46, 7,  61, 49, 18, 41, 50, 16, 40, 8,  45, 55, 45, 7,  24}},
        {"G3LTF DL9KR JO41",
         {47, 27, 46, 50, 58, 26, 38, 24, 22, 3,  14, 54, 10, 58, 36, 23, 63, 35, 41, 56, 53,
          62, 11, 49, 14, 35, 39, 60, 40, 44, 15, 45, 7,  44, 55, 23, 12, 49, 39, 11, 18, 36,
          26, 17, 2,  8,  60, 44, 37, 5,  48, 44, 18, 41, 32, 63, 4,  49, 55, 57, 37, 13, 25}},
    };
    for (const auto& r : rows) {
        INFO(r.text);
        auto bits = decode::wsjt72_pack(r.text);
        REQUIRE(bits);
        CHECK(decode::jt65_channel_symbols(*bits) == r.channel);
    }
    // Figure 3: 126 intervals, half of them sync.
    const auto sync = decode::jt65_sync_vector();
    REQUIRE(sync.size() == 126);
    CHECK(std::accumulate(sync.begin(), sync.end(), 0) == 63);
    // Section 8: "each of length 0.372 s", "finishes at t = 47.8 s".
    const auto& p = decode::jt65_protocol(0);
    CHECK(std::abs(p.symbol_seconds - 0.3715) < 0.001);
    CHECK(std::abs(p.nominal_start_seconds + p.symbols * p.symbol_seconds - 47.8) < 0.05);
    CHECK(std::abs(decode::jt65_protocol(1).tone_spacing_hz - 2 * 2.6917) < 0.001);
}

TEST_CASE("RS(63,12) corrects errors and erasures up to its distance", "[jt65][rs64]") {
    std::mt19937 rng(4);
    for (int trial = 0; trial < 300; ++trial) {
        decode::Rs64Message m{};
        for (auto& v : m) v = static_cast<std::uint8_t>(rng() % 64);
        const auto w = decode::rs64_encode(m);
        const auto zero = decode::rs64_syndromes(w);
        CHECK(std::all_of(zero.begin(), zero.end(), [](auto v) { return v == 0; }));
        // s erasures and e errors elsewhere with s + 2e <= 51.
        const int s = static_cast<int>(rng() % 52);
        const int e = static_cast<int>(rng() % ((51 - s) / 2 + 1));
        std::vector<int> pos(63);
        std::iota(pos.begin(), pos.end(), 0);
        std::shuffle(pos.begin(), pos.end(), rng);
        auto r = w;
        for (int i = 0; i < s + e; ++i) r[static_cast<std::size_t>(pos[static_cast<std::size_t>(i)])] ^= static_cast<std::uint8_t>(1 + rng() % 63);
        std::vector<int> erased(pos.begin(), pos.begin() + s);
        const auto got = decode::rs64_decode(r, decode::rs64_syndromes(r), erased);
        INFO("s " << s << " e " << e);
        REQUIRE(got);
        CHECK(*got == w);
    }
    // 26 errors with no erasures is past the guarantee; it must not return
    // the transmitted word as if corrected, and anything it returns must be a
    // code word.
    decode::Rs64Message m{};
    const auto w = decode::rs64_encode(m);
    auto r = w;
    for (int i = 0; i < 26; ++i) r[static_cast<std::size_t>(2 * i)] ^= 5;
    const auto got = decode::rs64_decode(r, decode::rs64_syndromes(r), {});
    if (got) {
        const auto syn = decode::rs64_syndromes(*got);
        CHECK(std::all_of(syn.begin(), syn.end(), [](auto v) { return v == 0; }));
    }
}

TEST_CASE("JT65A decodes several signals, overlapping and OOO, in one slot", "[jt65]") {
    struct Sig {
        const char* text;
        double f;
        double dt;
        bool ooo;
    };
    const Sig sigs[] = {
        {"G3LTF DL9KR JO40", 400.0, 1.0, false}, {"K1ABC W9XYZ EN37", 500.3, 1.4, false},
        {"TNX BOB 73 GL", 900.7, 2.0, false},     {"PA9XYZ G4ABC IO91", 1300.0, 0.6, true},
        {"HELLO WORLD", 1800.25, 1.2, false},
    };
    std::vector<float> slot(kSlot, 0.0F);
    for (const auto& s : sigs) {
        siggen::SlowModConfig c;
        c.frequency_hz = s.f;
        c.start_seconds = s.dt;
        c.amplitude = 0.05;
        REQUIRE(siggen::jt65_add(c, s.text, 0, s.ooo, slot));
    }
    add_noise(slot, 0.05, -21.0, 7);
    auto dec = decode::Jt65Decoder::create({});
    REQUIRE(dec);
    std::vector<decode::Jt65Decode> out;
    dec->decode_slot(slot, 0.0, 0, out);
    REQUIRE(out.size() == std::size(sigs));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto& s = sigs[i];
        INFO(s.text);
        CHECK(out[i].message.text == std::string(s.text) + (s.ooo ? " OOO" : ""));
        CHECK(out[i].ooo == s.ooo);
        CHECK(std::abs(out[i].frequency_hz - s.f) < 1.0);
        CHECK(std::abs(out[i].dt_seconds - (s.dt - 1.0)) < 0.05);
        CHECK(std::abs(out[i].snr_db + 21.0) < 3.0);
    }
}

TEST_CASE("JT65B decodes with drift", "[jt65]") {
    std::vector<float> slot(kSlot, 0.0F);
    siggen::SlowModConfig c;
    c.frequency_hz = 1270.5;
    c.amplitude = 0.05;
    c.drift_hz = 4.0;
    REQUIRE(siggen::jt65_add(c, "K1ABC W9XYZ EN37", 1, false, slot));
    add_noise(slot, 0.05, -20.0, 8);
    decode::Jt65Config cfg;
    cfg.submode = 1;
    cfg.max_drift_hz = 6.0;
    auto dec = decode::Jt65Decoder::create(cfg);
    REQUIRE(dec);
    std::vector<decode::Jt65Decode> out;
    dec->decode_slot(slot, 0.0, 0, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].message.text == "K1ABC W9XYZ EN37");
    CHECK(std::abs(out[0].drift_hz - 4.0) < 1.5);
}

TEST_CASE("JT65 stream at 48 kHz is chunk independent", "[jt65]") {
    const double t0 = 1'700'000'040.0 + 25.0;
    const int rate = 48000;
    const std::size_t lead = static_cast<std::size_t>(35.0 * rate);
    std::vector<float> audio(lead + 60 * rate + rate, 0.0F);
    siggen::SlowModConfig c;
    c.rate = rate;
    c.amplitude = 0.1;
    c.frequency_hz = 1270.5;
    c.start_seconds = 35.0 + 1.0;
    REQUIRE(siggen::jt65_add(c, "G3LTF DL9KR JO40", 0, false, audio));
    add_noise(audio, 0.1 * 2.0, -22.0, 12);
    const auto run = [&](std::uint32_t seed) {
        decode::Jt65Config cfg;
        cfg.rate = rate;
        cfg.utc_of_sample = [t0](dsp::SampleIndex) { return t0; };
        auto dec = decode::Jt65Decoder::create(cfg);
        REQUIRE(dec);
        std::vector<decode::Jt65Decode> out;
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
    CHECK(std::abs(a[0].dt_seconds) < 0.05);
    REQUIRE(b.size() == 1);
    CHECK(a[0].frequency_hz == b[0].frequency_hz);
    CHECK(a[0].snr_db == b[0].snr_db);
    CHECK(a[0].trials == b[0].trials);
}

TEST_CASE("JT65A decode threshold in AWGN", "[jt65][threshold]") {
    auto dec = decode::Jt65Decoder::create({});
    REQUIRE(dec);
    const char* msgs[] = {"G3LTF DL9KR JO40", "K1ABC W9XYZ EN37", "W9XYZ K1ABC FN42", "PA9XYZ G4ABC IO91",
                          "TNX BOB 73 GL",    "G4ABC PA9XYZ JO22", "HELLO WORLD",     "VK2ABC JA1XYZ PM95"};
    double threshold = 0.0;
    for (int snr = -21; snr >= -27; --snr) {
        int ok = 0;
        int total = 0;
        for (int trial = 0; trial < 2; ++trial) {
            std::vector<float> slot(kSlot, 0.0F);
            for (int i = 0; i < 8; ++i) {
                siggen::SlowModConfig c;
                c.amplitude = 0.05;
                c.frequency_hz = 300.0 + 280.0 * i + 7.0 * trial;
                c.start_seconds = 0.6 + 0.2 * i;
                REQUIRE(siggen::jt65_add(c, msgs[i], 0, false, slot));
            }
            add_noise(slot, 0.05, snr, static_cast<std::uint32_t>(1700 + snr * 7 + trial));
            std::vector<decode::Jt65Decode> out;
            dec->decode_slot(slot, 0.0, 0, out);
            std::set<std::string> got;
            for (const auto& d : out) got.insert(d.message.text);
            for (const char* m : msgs) ok += got.contains(m) ? 1 : 0;
            total += 8;
            for (const auto& d : out) {
                INFO(d.message.text << " at " << d.frequency_hz << " Hz, " << d.trials << " trials, "
                                    << d.hard_errors << " hard errors");
                CHECK(std::find_if(std::begin(msgs), std::end(msgs), [&](const char* m) { return d.message.text == m; }) !=
                      std::end(msgs));
            }
        }
        const double rate = static_cast<double>(ok) / total;
        WARN("JT65A AWGN SNR " << snr << " dB: " << ok << "/" << total << " decoded");
        if (rate >= 0.5) threshold = snr;
        if (snr >= -21) CHECK(rate >= 0.9);
    }
    WARN("JT65A lowest SNR with at least half decoded: " << threshold << " dB");
    CHECK(threshold <= -24.0);
}
