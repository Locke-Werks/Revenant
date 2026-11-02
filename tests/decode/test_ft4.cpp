// FT4: the paper's Costas arrays, Gray map and scrambler, and the decoder
// against the transmitter in core/dsp/synth/ft8_mod.h.
//
// Source: Franke, Somerville and Taylor, "The FT4 and FT8 Communication
// Protocols", QEX July/August 2020 pp. 7-17, sections 4 and 5 and Appendix A.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/ft4.h"
#include "core/dsp/synth/ft8_mod.h"

using namespace revenant;

namespace {

void add_noise(std::vector<float>& x, double amplitude, double snr_db, std::uint32_t seed,
               double half_band = 6000.0) {
    const double s = amplitude * amplitude / 2.0;
    const double n2500 = s / std::pow(10.0, snr_db / 10.0);
    const double sigma = std::sqrt(n2500 * half_band / 2500.0);
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, sigma);
    for (auto& v : x) v += static_cast<float>(g(rng));
}

}  // namespace

TEST_CASE("FT4 channel symbols follow section 4", "[ft4]") {
    const auto& p = decode::ft4_protocol();
    REQUIRE(p.layout.size() == 105);
    auto bits = decode::wsjt_pack("CQ K1ABC FN42");
    REQUIRE(bits);
    const auto tones = decode::ft_channel_tones(p, *bits);
    REQUIRE(tones.size() == 105);
    CHECK(tones[0] == 0);
    CHECK(tones[104] == 0);
    const int s[4][4] = {{0, 1, 3, 2}, {1, 0, 2, 3}, {2, 3, 1, 0}, {3, 2, 0, 1}};
    for (int g = 0; g < 4; ++g) {
        for (int i = 0; i < 4; ++i) {
            CHECK(tones[1 + 33 * static_cast<std::size_t>(g) + static_cast<std::size_t>(i)] == s[g][i]);
        }
    }
    CHECK(p.gray[2] == 0b11);
    // 105 symbols of 48 ms: 5.04 s.
    CHECK(105 * p.samples_per_symbol == 60480);
}

TEST_CASE("FT4 scrambler breaks up the zeros of a CQ", "[ft4]") {
    // Appendix A: the reason for the scrambler. "CQ" is c28 = 2, so a plain
    // CQ payload starts with 26 zeros.
    auto bits = decode::wsjt_pack("CQ K1ABC FN42");
    REQUIRE(bits);
    CHECK(std::count((*bits).begin(), (*bits).begin() + 26, 0) == 26);
    const auto& p = decode::ft4_protocol();
    int ones = 0;
    for (std::size_t i = 0; i < 26; ++i) ones += ((*bits)[i] ^ p.scrambler[i]);
    CHECK(ones > 8);
    // The scrambler is its own inverse and the decoder hands back the plain
    // payload.
    std::vector<float> slot(90000, 0.0F);
    siggen::FtModConfig c;
    c.protocol = p;
    c.frequency_hz = 1200.0;
    c.amplitude = 0.1;
    REQUIRE(siggen::ft_add(c, *bits, slot));
    auto dec = decode::FtDecoder::create(decode::ft4_config());
    REQUIRE(dec);
    std::vector<decode::FtDecode> out;
    dec->decode_slot(slot, 0.0, 0, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].payload == *bits);
    CHECK(out[0].message.text == "CQ K1ABC FN42");
    CHECK(std::abs(out[0].dt_seconds) < 0.01);
}

TEST_CASE("FT4 decodes several signals in one slot", "[ft4]") {
    const std::vector<std::pair<const char*, double>> sigs = {
        {"CQ K1ABC FN42", 500.0},          {"K1ABC W9XYZ 579 WI", 900.0},
        {"W9XYZ K1ABC R 559 MA", 1400.0},  {"K1ABC W9XYZ RR73", 1900.0},
        {"CQ TEST G4ABC IO91", 2500.0},
    };
    std::vector<float> slot(90000, 0.0F);
    double dt = 0.3;
    for (const auto& [text, f] : sigs) {
        siggen::FtModConfig c;
        c.protocol = decode::ft4_protocol();
        c.frequency_hz = f;
        c.start_seconds = dt;
        c.amplitude = 0.1;
        dt += 0.2;
        REQUIRE(siggen::ft_add_text(c, text, slot));
    }
    add_noise(slot, 0.1, -8.0, 5);
    auto dec = decode::FtDecoder::create(decode::ft4_config());
    REQUIRE(dec);
    std::vector<decode::FtDecode> out;
    dec->decode_slot(slot, 0.0, 0, out);
    std::set<std::string> got;
    for (const auto& d : out) got.insert(d.message.text);
    for (const auto& [text, f] : sigs) {
        INFO(text);
        CHECK(got.contains(text));
    }
    CHECK(got.size() == sigs.size());
}

TEST_CASE("FT4 stream at 48 kHz is chunk independent", "[ft4]") {
    const double t0 = 1'699'999'995.0 + 3.0;  // 4.5 s before a 7.5 s boundary
    const int rate = 48000;
    std::vector<float> audio(static_cast<std::size_t>((4.5 + 15.5) * rate), 0.0F);
    siggen::FtModConfig c;
    c.protocol = decode::ft4_protocol();
    c.rate = rate;
    c.amplitude = 0.1;
    c.frequency_hz = 1000.0;
    c.start_seconds = 4.5 + 0.5;
    REQUIRE(siggen::ft_add_text(c, "K1ABC W9XYZ -05", audio));
    c.start_seconds = 4.5 + 7.5 + 0.7;
    c.frequency_hz = 2000.0;
    REQUIRE(siggen::ft_add_text(c, "W9XYZ K1ABC R-07", audio));
    add_noise(audio, 0.1, -6.0, 13, 24000.0);
    const auto run = [&](std::uint32_t seed) {
        auto cfg = decode::ft4_config();
        cfg.rate = rate;
        cfg.utc_of_sample = [t0](dsp::SampleIndex) { return t0; };
        auto dec = decode::FtDecoder::create(cfg);
        REQUIRE(dec);
        std::vector<decode::FtDecode> out;
        std::mt19937 rng(seed);
        std::size_t pos = 0;
        while (pos < audio.size()) {
            const std::size_t n = std::min<std::size_t>(audio.size() - pos, 1 + rng() % 20000);
            REQUIRE(dec->process(std::span<const float>(audio.data() + pos, n), out));
            pos += n;
        }
        return out;
    };
    const auto a = run(3);
    const auto b = run(4);
    REQUIRE(a.size() == 2);
    CHECK(a[0].message.text == "K1ABC W9XYZ -05");
    CHECK(a[1].message.text == "W9XYZ K1ABC R-07");
    CHECK(std::fmod(a[0].slot_utc, 7.5) == 0.0);
    CHECK(std::abs(a[1].dt_seconds - 0.2) < 0.02);
    REQUIRE(b.size() == 2);
    CHECK(a[0].frequency_hz == b[0].frequency_hz);
    CHECK(a[1].snr_db == b[1].snr_db);
}

TEST_CASE("FT4 decode threshold in AWGN", "[ft4][threshold]") {
    auto dec = decode::FtDecoder::create(decode::ft4_config());
    REQUIRE(dec);
    const char* msgs[] = {"CQ K1ABC FN42", "K1ABC W9XYZ -11", "W9XYZ K1ABC R-09",
                          "K1ABC W9XYZ RR73", "CQ DX G4ABC IO91", "G4ABC PA9XYZ JO22",
                          "PA9XYZ G4ABC -20", "CQ W9XYZ EN37"};
    double threshold = 0.0;
    for (int snr = -10; snr >= -18; --snr) {
        int ok = 0;
        int total = 0;
        for (int trial = 0; trial < 2; ++trial) {
            std::vector<float> slot(90000, 0.0F);
            for (int i = 0; i < 8; ++i) {
                siggen::FtModConfig c;
                c.protocol = decode::ft4_protocol();
                c.amplitude = 0.05;
                c.frequency_hz = 300.0 + 300.0 * i + 23.0 * trial;
                c.start_seconds = 0.3 + 0.15 * i;
                REQUIRE(siggen::ft_add_text(c, msgs[i], slot));
            }
            add_noise(slot, 0.05, snr, static_cast<std::uint32_t>(500 + snr * 7 + trial));
            std::vector<decode::FtDecode> out;
            dec->decode_slot(slot, 0.0, 0, out);
            std::set<std::string> got;
            for (const auto& d : out) got.insert(d.message.text);
            for (const char* m : msgs) ok += got.contains(m) ? 1 : 0;
            total += 8;
        }
        const double rate = static_cast<double>(ok) / total;
        WARN("FT4 AWGN SNR " << snr << " dB: " << ok << "/" << total << " decoded");
        if (rate >= 0.5) threshold = snr;
        if (snr >= -12) CHECK(rate >= 0.9);
    }
    WARN("FT4 lowest SNR with at least half decoded: " << threshold << " dB");
    CHECK(threshold <= -14.0);
}
