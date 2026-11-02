// JT9 and the 72-bit message: G4JNT's "The JT9 Coding Process" (March
// 2016), whose last page lists the channel symbols for " G4JNT IO90IV", and
// Figure 2 of K1JT's "The JT65 Communications Protocol" (QEX 2005) for
// standard messages. The receiver runs against the transmitter in
// core/dsp/synth/wsjt_slow_mod.h.
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

#include "core/decode/jt9.h"
#include "core/decode/wsjt72.h"
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

std::vector<int> six_bit(const decode::Wsjt72& b) {
    std::vector<int> out;
    for (int i = 0; i < 12; ++i) {
        int v = 0;
        for (int j = 0; j < 6; ++j) v = (v << 1) | b[static_cast<std::size_t>(6 * i + j)];
        out.push_back(v);
    }
    return out;
}

constexpr std::size_t kSlot = 60 * 12000;

}  // namespace

TEST_CASE("QEX 2005 Figure 2 packed messages", "[jt9][wsjt72]") {
    struct Row {
        const char* c1;
        const char* c2;
        const char* g;
        std::vector<int> packed;
    };
    const Row rows[] = {
        {"G3LTF", "DL9KR", "JO40", {61, 37, 30, 28, 9, 27, 61, 58, 26, 3, 49, 16}},
        {"G3LTE", "DL9KR", "JO40", {61, 37, 30, 28, 5, 27, 61, 58, 26, 3, 49, 16}},
        {"G3LTF", "DL9KR", "JO41", {61, 37, 30, 28, 9, 27, 61, 58, 26, 3, 49, 17}},
    };
    for (const auto& r : rows) {
        auto b = decode::wsjt72_pack_standard(r.c1, r.c2, r.g);
        REQUIRE(b);
        CHECK(six_bit(*b) == r.packed);
        const auto m = decode::wsjt72_unpack(*b);
        CHECK(m.standard);
        CHECK_FALSE(m.plain_text);
        CHECK(m.text == std::string(r.c1) + " " + r.c2 + " " + r.g);
    }
}

TEST_CASE("Plain text packs as the JT9 document describes", "[jt9][wsjt72]") {
    // " G4JNT IO90IV", with its leading space.
    auto b = decode::wsjt72_pack_text(" G4JNT IO90IV");
    REQUIRE(b);
    // N1 from " G4JN", N2 from "T IO9", N3 from "0IV".
    const auto v = [](char c) { return static_cast<std::uint32_t>(std::string_view("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?").find(c)); };
    std::uint32_t n1 = 0;
    std::uint32_t n2 = 0;
    std::uint32_t n3 = 0;
    for (char c : std::string_view(" G4JN")) n1 = n1 * 42 + v(c);
    for (char c : std::string_view("T IO9")) n2 = n2 * 42 + v(c);
    for (char c : std::string_view("0IV")) n3 = n3 * 42 + v(c);
    const std::uint64_t want_hi = (static_cast<std::uint64_t>(n1 * 2 + ((n3 >> 15) & 1)) << 28) | (n2 * 2 + ((n3 >> 16) & 1));
    std::uint64_t hi = 0;
    for (int i = 0; i < 56; ++i) hi = (hi << 1) | (*b)[static_cast<std::size_t>(i)];
    CHECK(hi == want_hi);
    std::uint32_t lo = 0;
    for (int i = 56; i < 72; ++i) lo = (lo << 1) | (*b)[static_cast<std::size_t>(i)];
    CHECK(lo == (n3 % 32768) + 32768);
    // "N1 and N2 have a maximum value of 42^5 - 1 = 130691231".
    CHECK(42ULL * 42 * 42 * 42 * 42 - 1 == 130691231ULL);

    const auto m = decode::wsjt72_unpack(*b);
    CHECK(m.plain_text);
    CHECK(m.text == " G4JNT IO90IV");
    for (const char* t : {"TNX BOB 73 GL", "HELLO", "A+B-C.D/E?", "73"}) {
        auto x = decode::wsjt72_pack(t);
        REQUIRE(x);
        CHECK(decode::wsjt72_unpack(*x).text == t);
    }
    CHECK_FALSE(decode::wsjt72_pack_text("FOURTEEN CHARS"));
    CHECK_FALSE(decode::wsjt72_pack_text("NO#HASH"));
}

TEST_CASE("JT9 channel symbols match the document's listing", "[jt9]") {
    // Page 5: "; JT9 Symbols packed two per byte. Message: G4JNT IO90IV".
    const std::uint8_t listing[] = {
        0x00, 0x13, 0x08, 0x84, 0x10, 0x31, 0x61, 0x40, 0x43, 0x47, 0x72, 0x01, 0x54, 0x16, 0x74,
        0x47, 0x04, 0x03, 0x41, 0x83, 0x68, 0x54, 0x36, 0x86, 0x81, 0x00, 0x72, 0x08, 0x67, 0x70,
        0x56, 0x77, 0x10, 0x61, 0x86, 0x63, 0x04, 0x48, 0x31, 0x52, 0x17, 0x07, 0x00,
    };
    std::vector<int> want;
    for (const auto b : listing) {
        want.push_back(b >> 4);
        want.push_back(b & 15);
    }
    want.resize(85);
    // The listing is for the message without the leading space the text's
    // example mentions; with it the data symbols do not match.
    auto bits = decode::wsjt72_pack_text("G4JNT IO90IV");
    REQUIRE(bits);
    CHECK(decode::jt9_channel_tones(*bits) == want);
    // The manual's sync positions, counted from 1.
    std::vector<int> pos;
    for (int i = 0; i < 85; ++i) {
        if (decode::jt9_sync_vector()[static_cast<std::size_t>(i)]) pos.push_back(i + 1);
    }
    CHECK(pos == std::vector<int>{1, 2, 5, 10, 16, 23, 33, 35, 51, 52, 55, 60, 66, 73, 83, 85});
    // "about 0.576 seconds", "12000/6912 = 1.736 Hz".
    CHECK(std::abs(decode::jt9_protocol(0).symbol_seconds - 0.576) < 1e-9);
    CHECK(std::abs(decode::jt9_protocol(3).tone_spacing_hz - 13.8888) < 1e-3);
}

TEST_CASE("JT9 decodes several signals with offsets and drift in one slot", "[jt9]") {
    struct Sig {
        const char* text;
        double f;
        double dt;
        double drift;
    };
    const Sig sigs[] = {
        {"G3LTF DL9KR JO40", 400.0, 1.0, 0.0}, {"K1ABC W9XYZ EN37", 800.3, 0.4, 1.0},
        {" G4JNT IO90IV", 1200.7, 2.0, -1.0},  {"TNX BOB 73 GL", 1600.0, 1.5, 0.0},
        {"PA9XYZ G4ABC IO91", 2000.0, 0.1, 0.5}, {"HELLO WORLD", 2500.25, 1.2, -0.5},
    };
    std::vector<float> slot(kSlot, 0.0F);
    for (const auto& s : sigs) {
        siggen::SlowModConfig c;
        c.frequency_hz = s.f;
        c.start_seconds = s.dt;
        c.drift_hz = s.drift;
        c.amplitude = 0.05;
        REQUIRE(siggen::jt9_add(c, s.text, 0, slot));
    }
    add_noise(slot, 0.05, -22.0, 7);
    auto dec = decode::Jt9Decoder::create({});
    REQUIRE(dec);
    std::vector<decode::Wsjt72Decode> out;
    dec->decode_slot(slot, 0.0, 0, out);
    REQUIRE(out.size() == std::size(sigs));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto& s = sigs[i];
        INFO(s.text);
        CHECK(out[i].message.text == s.text);
        CHECK(std::abs(out[i].frequency_hz - s.f) < 0.5);
        CHECK(std::abs(out[i].dt_seconds - (s.dt - 1.0)) < 0.1);
        CHECK(std::abs(out[i].snr_db + 22.0) < 3.0);
    }
}

TEST_CASE("JT9 submode D decodes", "[jt9]") {
    std::vector<float> slot(kSlot, 0.0F);
    siggen::SlowModConfig c;
    c.frequency_hz = 1000.0;
    c.amplitude = 0.05;
    REQUIRE(siggen::jt9_add(c, "K1ABC W9XYZ EN37", 3, slot));
    add_noise(slot, 0.05, -18.0, 8);
    decode::Jt9Config cfg;
    cfg.submode = 3;
    auto dec = decode::Jt9Decoder::create(cfg);
    REQUIRE(dec);
    std::vector<decode::Wsjt72Decode> out;
    dec->decode_slot(slot, 0.0, 0, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].message.text == "K1ABC W9XYZ EN37");
}

TEST_CASE("JT9 stream at 48 kHz is chunk independent", "[jt9]") {
    const double t0 = 1'700'000'040.0 + 25.0;  // a multiple of 60, plus 25 s
    const int rate = 48000;
    const std::size_t lead = static_cast<std::size_t>(35.0 * rate);
    std::vector<float> audio(lead + 60 * rate + rate, 0.0F);
    siggen::SlowModConfig c;
    c.rate = rate;
    c.amplitude = 0.1;
    c.frequency_hz = 1500.0;
    c.start_seconds = 35.0 + 1.0;
    REQUIRE(siggen::jt9_add(c, "G3LTF DL9KR JO40", 0, audio));
    add_noise(audio, 0.1 * 2.0, -18.0, 12);
    const auto run = [&](std::uint32_t seed) {
        decode::Jt9Config cfg;
        cfg.rate = rate;
        cfg.utc_of_sample = [t0](dsp::SampleIndex) { return t0; };
        auto dec = decode::Jt9Decoder::create(cfg);
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
    CHECK(std::fmod(a[0].slot_utc, 60.0) == 0.0);
    CHECK(a[0].slot_start_sample == static_cast<long long>(lead));
    REQUIRE(b.size() == 1);
    CHECK(a[0].frequency_hz == b[0].frequency_hz);
    CHECK(a[0].snr_db == b[0].snr_db);
}

TEST_CASE("JT9 decode threshold in AWGN", "[jt9][threshold]") {
    auto dec = decode::Jt9Decoder::create({});
    REQUIRE(dec);
    const char* msgs[] = {"G3LTF DL9KR JO40", "K1ABC W9XYZ EN37", "W9XYZ K1ABC FN42", "PA9XYZ G4ABC IO91",
                          "TNX BOB 73 GL",    "G4ABC PA9XYZ JO22", "HELLO WORLD",     "VK2ABC JA1XYZ PM95"};
    double threshold = 0.0;
    for (int snr = -23; snr >= -29; --snr) {
        int ok = 0;
        int total = 0;
        for (int trial = 0; trial < 2; ++trial) {
            std::vector<float> slot(kSlot, 0.0F);
            for (int i = 0; i < 8; ++i) {
                siggen::SlowModConfig c;
                c.amplitude = 0.05;
                c.frequency_hz = 300.0 + 320.0 * i + 7.0 * trial;
                c.start_seconds = 0.4 + 0.25 * i;
                REQUIRE(siggen::jt9_add(c, msgs[i], 0, slot));
            }
            add_noise(slot, 0.05, snr, static_cast<std::uint32_t>(900 + snr * 7 + trial));
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
        WARN("JT9 AWGN SNR " << snr << " dB: " << ok << "/" << total << " decoded");
        if (rate >= 0.5) threshold = snr;
        if (snr >= -23) CHECK(rate >= 0.9);
    }
    WARN("JT9 lowest SNR with at least half decoded: " << threshold << " dB");
    CHECK(threshold <= -24.0);
}
