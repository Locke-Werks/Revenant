// GLONASS L1OF: the string check code and time mark against ICD 5.1's own
// rules, then six FDMA channels at 35 to 45 dB-Hz through the transmitter in
// core/dsp/synth/glonass_mod.h, decoded to the immediate information.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <vector>

#include "core/decode/glonass_l1.h"
#include "core/dsp/synth/glonass_mod.h"
#include "core/dsp/synth/gnss_mod.h"

using namespace revenant;
using namespace revenant::decode::gnss;

namespace {

GlonassStringBits random_string(std::mt19937& rng) {
    GlonassStringBits b{};
    for (unsigned i = 9; i <= 84; ++i) {
        b[i] = static_cast<std::uint8_t>(rng() & 1U);
    }
    b[85] = 0;
    glonass_fill_check(b);
    return b;
}

GlonassEphemeris eph_for(int k) {
    GlonassEphemeris e;
    e.channel = k;
    e.tk_h = 13;
    e.tk_min = 41;
    e.tk_30s = 1;
    e.tb_min = 15 * 55;
    e.bn = 0;
    e.p1 = 2;
    e.p2 = true;
    e.p3 = false;
    e.p4 = true;
    e.pos_km = {-12345.0 + k, 23456.5, -7000.25};
    e.vel_km_s = {1.5 + std::ldexp(1.0, -20) * 3, -2.75, 0.125};
    e.acc_km_s2 = {std::ldexp(1.0, -30) * 5, -std::ldexp(1.0, -30) * 3, 0.0};
    e.gamma_n = -std::ldexp(1.0, -40) * 77;
    e.tau_n_s = std::ldexp(1.0, -30) * (12345 + k * 10);
    e.delta_tau_n_s = -std::ldexp(1.0, -30) * 4;
    e.en_days = 3;
    e.ft = 2;
    e.nt_days = 1234;
    e.slot = static_cast<unsigned>(10 + k);
    e.m_type = 1;
    e.p = 3;
    e.ln3 = false;
    return e;
}

}  // namespace

TEST_CASE("GLONASS string check corrects one error and erases two", "[gnss][glonass]") {
    std::mt19937 rng(17);
    for (unsigned t = 0; t < 20; ++t) {
        const auto clean = random_string(rng);
        auto b = clean;
        REQUIRE(glonass_check_string(b) == GlonassCheck::Clean);
        for (unsigned i = 1; i <= 85; ++i) {
            b = clean;
            b[i] ^= 1U;
            INFO("bit " << i);
            if (i == 8) {
                // 4.7 rule c: C1..C7 all zero with C = 1 reads as a multiple
                // error, so a lone error in the overall check bit erases.
                REQUIRE(glonass_check_string(b) == GlonassCheck::Erased);
                continue;
            }
            REQUIRE(glonass_check_string(b) == GlonassCheck::Corrected);
            // Data bits come back; a flipped check bit leaves the data alone.
            for (unsigned j = 9; j <= 85; ++j) {
                REQUIRE(b[j] == clean[j]);
            }
        }
        b = clean;
        b[20] ^= 1U;
        b[47] ^= 1U;
        CHECK(glonass_check_string(b) == GlonassCheck::Erased);
    }
}

TEST_CASE("GLONASS time mark is the g(x) = 1 + x^3 + x^5 sequence ICD 5.1 prints", "[gnss][glonass]") {
    const auto tm = glonass_time_mark();
    for (std::size_t n = 0; n + 5 < tm.size(); ++n) {
        CHECK(tm[n + 5] == (tm[n] ^ tm[n + 2]));
    }
}

TEST_CASE("GLONASS L1OF: six channels at 35 to 45 dB-Hz decode to immediate data",
          "[gnss][glonass]") {
    constexpr dsp::SampleRate kRate = 4'096'000;
    const std::array<int, 6> ks = {-3, -2, -1, 0, 1, 2};
    // The weakest away from the band edges, where k = -3's main lobe wraps
    // past fs/2 and costs it about a decibel.
    const std::array<double, 6> kCn0 = {43.0, 37.0, 35.0, 39.0, 41.0, 45.0};
    std::vector<siggen::GnssSatelliteSpec> sats;
    for (std::size_t i = 0; i < ks.size(); ++i) {
        siggen::GnssSatelliteSpec s;
        s.code = glonass_st_code();
        s.chip_rate_hz = 511000.0;
        s.carrier_hz = 1602.0e6 + 562500.0 * ks[i];
        s.offset_hz = 562500.0 * ks[i];
        s.cn0_dbhz = kCn0[i];
        s.doppler_hz = -3000.0 + 1100.0 * static_cast<double>(i);
        s.code_phase_chips = 30.0 + 80.0 * static_cast<double>(i);
        s.periods_per_symbol = 10;
        std::vector<GlonassStringBits> strings = {siggen::glonass_empty_string(14),
                                                  siggen::glonass_empty_string(15)};
        for (const auto& b : siggen::glonass_immediate_strings(eph_for(ks[i]))) {
            strings.push_back(b);
        }
        strings.push_back(siggen::glonass_empty_string(5));
        for (const auto& b : strings) {
            const auto sym = siggen::glonass_string_symbols(b);
            s.symbols.insert(s.symbols.end(), sym.begin(), sym.end());
        }
        sats.push_back(std::move(s));
    }
    auto synth = siggen::GnssSynth::create(kRate, sats, 31337);
    REQUIRE(synth);
    GlonassL1Config cfg;
    cfg.sample_rate = kRate;
    // The automatic list stops at |k| = 2 here, the channels whose main lobe
    // fits inside +-fs/2; -3 is asked for explicitly.
    cfg.channels = {-3, -2, -1, 0, 1, 2};
    auto dec = GlonassL1Decoder::create(cfg);
    REQUIRE(dec);
    GlonassL1Output out;
    std::vector<dsp::Complex32> chunk(kRate / 4);
    double cpu = 0.0;
    const double seconds = 12.5;
    for (std::size_t b = 0; b < static_cast<std::size_t>(seconds * 4); ++b) {
        synth->generate(chunk);
        const auto t0 = std::chrono::steady_clock::now();
        dec->feed(chunk, out);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    WARN("GLONASS L1OF decoder: " << cpu / seconds << " s CPU per s of input at 4.096 MS/s, "
                                  << "6 channels searched and tracked");
    std::map<int, SatelliteStatus> status;
    for (const auto& st : out.status) {
        status[static_cast<int>(st.prn) - 100] = st;
    }
    std::set<int> got;
    for (const auto& e : out.ephemerides) {
        const auto want = eph_for(e.channel);
        INFO("k " << e.channel);
        CHECK(e.pos_km == want.pos_km);
        CHECK(e.vel_km_s == want.vel_km_s);
        CHECK(e.acc_km_s2 == want.acc_km_s2);
        CHECK(e.tau_n_s == want.tau_n_s);
        CHECK(e.gamma_n == want.gamma_n);
        CHECK(e.delta_tau_n_s == want.delta_tau_n_s);
        CHECK(e.tb_min == want.tb_min);
        CHECK(e.tk_h == want.tk_h);
        CHECK(e.tk_min == want.tk_min);
        CHECK(e.nt_days == want.nt_days);
        CHECK(e.slot == want.slot);
        CHECK(e.p4 == want.p4);
        got.insert(e.channel);
    }
    for (std::size_t i = 0; i < ks.size(); ++i) {
        WARN("k " << ks[i] << " sent " << kCn0[i] << " dB-Hz, estimated "
                  << status[ks[i]].cn0_dbhz << ", Doppler " << status[ks[i]].doppler_hz);
    }
    CHECK(got == std::set<int>(ks.begin(), ks.end()));
}
