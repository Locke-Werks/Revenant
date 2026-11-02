// BeiDou B1I D1: the BCH code against the B1I ICD's Table 5-2, then six MEO
// and IGSO satellites at 35 to 45 dB-Hz with the NH overlay through the
// transmitter in core/dsp/synth/beidou_mod.h, decoded to subframe 1 clock and
// ionosphere and the subframe 2 and 3 ephemeris.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "core/decode/beidou_b1i.h"
#include "core/dsp/synth/beidou_mod.h"
#include "core/dsp/synth/gnss_mod.h"

using namespace revenant;
using namespace revenant::decode::gnss;

namespace {

B1iClockIono clock_for(unsigned prn) {
    B1iClockIono c;
    c.sat_h1 = false;
    c.aodc = 3;
    c.urai = 2;
    c.week = 1000 + prn;
    c.toc_s = 8.0 * 45000.0;
    c.tgd1_s = -37 * 0.1e-9;
    c.tgd2_s = 12 * 0.1e-9;
    c.alpha = {5.0 * std::ldexp(1.0, -30), -2.0 * std::ldexp(1.0, -27), 9.0 * std::ldexp(1.0, -24),
               -1.0 * std::ldexp(1.0, -24)};
    c.beta = {70.0 * std::ldexp(1.0, 11), -3.0 * std::ldexp(1.0, 14), 2.0 * std::ldexp(1.0, 16),
              -5.0 * std::ldexp(1.0, 16)};
    c.a0 = (-123456.0 - prn) * std::ldexp(1.0, -33);
    c.a1 = 7777.0 * std::ldexp(1.0, -50);
    c.a2 = -3.0 * std::ldexp(1.0, -66);
    c.aode = 9;
    return c;
}

B1iEphemeris eph_for(unsigned prn) {
    B1iEphemeris e;
    e.toe_s = 8.0 * 45000.0;
    e.sqrt_a = 2852200000.0 * std::ldexp(1.0, -19);
    e.e = (3000000.0 + prn) * std::ldexp(1.0, -33);
    e.omega_sc = -1200000000.0 * std::ldexp(1.0, -31);
    e.delta_n_sc_s = 2000.0 * std::ldexp(1.0, -43);
    e.m0_sc = (800000000.0 + prn) * std::ldexp(1.0, -31);
    e.omega0_sc = -900000000.0 * std::ldexp(1.0, -31);
    e.omega_dot_sc_s = -30000.0 * std::ldexp(1.0, -43);
    e.i0_sc = 590000000.0 * std::ldexp(1.0, -31);
    e.idot_sc_s = -100.0 * std::ldexp(1.0, -43);
    e.cuc_rad = -5000.0 * std::ldexp(1.0, -31);
    e.cus_rad = 4000.0 * std::ldexp(1.0, -31);
    e.crc_m = 9000.0 * std::ldexp(1.0, -6);
    e.crs_m = -8000.0 * std::ldexp(1.0, -6);
    e.cic_rad = 300.0 * std::ldexp(1.0, -31);
    e.cis_rad = -200.0 * std::ldexp(1.0, -31);
    return e;
}

}  // namespace

TEST_CASE("B1I BCH(15,11,1) syndromes are the ICD's Table 5-2", "[gnss][beidou]") {
    // D3D2D1D0 -> the 15-bit error pattern, first-sent bit leftmost.
    const std::array<std::pair<unsigned, const char*>, 15> rom = {{
        {0b0001, "000000000000001"}, {0b0010, "000000000000010"}, {0b0011, "000000000010000"},
        {0b0100, "000000000000100"}, {0b0101, "000000100000000"}, {0b0110, "000000000100000"},
        {0b0111, "000010000000000"}, {0b1000, "000000000001000"}, {0b1001, "100000000000000"},
        {0b1010, "000001000000000"}, {0b1011, "000000010000000"}, {0b1100, "000000001000000"},
        {0b1101, "010000000000000"}, {0b1110, "000100000000000"}, {0b1111, "001000000000000"},
    }};
    for (const auto& [syn, pattern] : rom) {
        std::uint16_t e = 0;
        for (unsigned i = 0; i < 15; ++i) {
            e = static_cast<std::uint16_t>((e << 1U) | (pattern[i] == '1' ? 1U : 0U));
        }
        INFO(pattern);
        CHECK(b1i_bch_syndrome(e) == syn);
    }
    for (std::uint16_t info = 0; info < 2048; info += 37) {
        const auto cw = b1i_bch_encode(info);
        CHECK(b1i_bch_syndrome(cw) == 0);
        for (unsigned i = 0; i < 15; ++i) {
            bool fixed = false;
            CHECK(b1i_bch_decode(static_cast<std::uint16_t>(cw ^ (1U << i)), fixed) == info);
            CHECK(fixed);
        }
    }
}

TEST_CASE("BeiDou B1I: six D1 satellites at 35 to 45 dB-Hz decode to clock and ephemeris",
          "[gnss][beidou]") {
    constexpr dsp::SampleRate kRate = 4'096'000;
    const std::array<unsigned, 6> prns = {6, 11, 19, 23, 34, 45};
    std::vector<siggen::GnssSatelliteSpec> sats;
    for (std::size_t i = 0; i < prns.size(); ++i) {
        siggen::GnssSatelliteSpec s;
        s.code = beidou_b1i_code(prns[i]);
        s.chip_rate_hz = 2.046e6;
        s.carrier_hz = 1561.098e6;
        s.cn0_dbhz = 35.0 + 2.0 * static_cast<double>(i);
        s.doppler_hz = 2500.0 - 900.0 * static_cast<double>(i);
        s.code_phase_chips = 100.0 + 301.0 * static_cast<double>(i);
        s.overlay = beidou_nh20();
        s.periods_per_symbol = 20;
        const std::uint32_t sow0 = 345600;
        std::vector<siggen::B1iInfo> sf = {siggen::b1i_header(5, sow0 - 6), siggen::b1i_header(1, sow0),
                                           siggen::b1i_header(2, sow0 + 6), siggen::b1i_header(3, sow0 + 12),
                                           siggen::b1i_header(4, sow0 + 18)};
        siggen::b1i_put_clock_iono(sf[1], clock_for(prns[i]));
        siggen::b1i_put_ephemeris(sf[2], sf[3], eph_for(prns[i]));
        for (std::size_t k = 0; k < sf.size(); ++k) {
            const auto bits = siggen::b1i_encode(sf[k]);
            const std::size_t from = k == 0 ? 75 : 0;
            s.symbols.insert(s.symbols.end(), bits.begin() + static_cast<std::ptrdiff_t>(from), bits.end());
        }
        sats.push_back(std::move(s));
    }
    auto synth = siggen::GnssSynth::create(kRate, sats, 2718);
    REQUIRE(synth);
    BeidouB1iConfig cfg;
    cfg.sample_rate = kRate;
    auto dec = BeidouB1iDecoder::create(cfg);
    REQUIRE(dec);
    BeidouB1iOutput out;
    std::vector<dsp::Complex32> chunk(kRate / 4);
    double cpu = 0.0;
    const double seconds = 23.5;
    for (std::size_t b = 0; b < static_cast<std::size_t>(seconds * 4); ++b) {
        synth->generate(chunk);
        const auto t0 = std::chrono::steady_clock::now();
        dec->feed(chunk, out);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    WARN("BeiDou B1I decoder: " << cpu / seconds
                                << " s CPU per s of input at 4.096 MS/s, 63 PRNs searched, 6 tracked");
    std::map<unsigned, SatelliteStatus> status;
    for (const auto& st : out.status) {
        status[st.prn] = st;
    }
    for (std::size_t i = 0; i < prns.size(); ++i) {
        WARN("PRN " << prns[i] << " sent " << 35.0 + 2.0 * static_cast<double>(i) << " dB-Hz, estimated "
                    << status[prns[i]].cn0_dbhz << ", Doppler " << status[prns[i]].doppler_hz);
    }
    std::set<unsigned> clocks;
    for (const auto& [prn, c] : out.clocks) {
        const auto want = clock_for(prn);
        INFO("PRN " << prn);
        CHECK(c.week == want.week);
        CHECK(c.toc_s == want.toc_s);
        CHECK(std::abs(c.tgd1_s - want.tgd1_s) < 1e-14);
        CHECK(std::abs(c.tgd2_s - want.tgd2_s) < 1e-14);
        CHECK(c.alpha == want.alpha);
        CHECK(c.beta == want.beta);
        CHECK(c.a0 == want.a0);
        CHECK(c.a1 == want.a1);
        CHECK(c.a2 == want.a2);
        CHECK(c.aode == want.aode);
        clocks.insert(prn);
    }
    std::set<unsigned> ephs;
    for (const auto& [prn, e] : out.ephemerides) {
        const auto want = eph_for(prn);
        INFO("PRN " << prn);
        CHECK(e.toe_s == want.toe_s);
        CHECK(e.sqrt_a == want.sqrt_a);
        CHECK(e.e == want.e);
        CHECK(e.omega_sc == want.omega_sc);
        CHECK(e.delta_n_sc_s == want.delta_n_sc_s);
        CHECK(e.m0_sc == want.m0_sc);
        CHECK(e.omega0_sc == want.omega0_sc);
        CHECK(e.omega_dot_sc_s == want.omega_dot_sc_s);
        CHECK(e.i0_sc == want.i0_sc);
        CHECK(e.idot_sc_s == want.idot_sc_s);
        CHECK(e.cuc_rad == want.cuc_rad);
        CHECK(e.cus_rad == want.cus_rad);
        CHECK(e.crc_m == want.crc_m);
        CHECK(e.crs_m == want.crs_m);
        CHECK(e.cic_rad == want.cic_rad);
        CHECK(e.cis_rad == want.cis_rad);
        ephs.insert(prn);
    }
    const std::set<unsigned> all(prns.begin(), prns.end());
    CHECK(clocks == all);
    CHECK(ephs == all);
}
