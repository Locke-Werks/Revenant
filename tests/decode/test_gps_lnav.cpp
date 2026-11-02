// GPS L1 C/A LNAV: the parity code against IS-GPS-200N's own rules, then a
// constellation of eight satellites at 35 to 45 dB-Hz through the synthetic
// transmitter in core/dsp/synth/gps_lnav_mod.h and gnss_mod.h, decoded to
// clock, ephemeris, almanac and UTC/ionosphere and compared field by field.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "core/decode/gps_lnav.h"
#include "core/dsp/synth/gnss_mod.h"
#include "core/dsp/synth/gps_lnav_mod.h"

using namespace revenant;
using namespace revenant::decode::gnss;

namespace {

LnavClock clock_for(unsigned prn) {
    LnavClock c;
    c.week = 345;
    c.l2_codes = 1;
    c.ura_index = 2;
    c.health = 0;
    c.iodc = 0x100U + prn;
    c.l2_p_data = false;
    c.tgd_s = -11.0 * std::ldexp(1.0, -31);
    c.toc_s = 16.0 * 22500.0;
    c.af2 = 0.0;
    c.af1 = -37.0 * std::ldexp(1.0, -43);
    c.af0 = (12345.0 + prn) * std::ldexp(1.0, -31);
    return c;
}

LnavEphemeris eph_for(unsigned prn) {
    LnavEphemeris e;
    e.iode = prn;
    e.crs_m = -1234.0 * std::ldexp(1.0, -5);
    e.delta_n_sc_s = 4321.0 * std::ldexp(1.0, -43);
    e.m0_sc = -123456789.0 * std::ldexp(1.0, -31) + prn * std::ldexp(1.0, -20);
    e.cuc_rad = 999.0 * std::ldexp(1.0, -29);
    e.e = 77777777.0 * std::ldexp(1.0, -33);
    e.cus_rad = -888.0 * std::ldexp(1.0, -29);
    e.sqrt_a = 2702429457.0 * std::ldexp(1.0, -19);
    e.toe_s = 16.0 * 22500.0;
    e.fit_interval = false;
    e.aodo = 27;
    e.cic_rad = 55.0 * std::ldexp(1.0, -29);
    e.omega0_sc = 987654321.0 * std::ldexp(1.0, -31);
    e.cis_rad = -66.0 * std::ldexp(1.0, -29);
    e.i0_sc = 650000000.0 * std::ldexp(1.0, -31);
    e.crc_m = 7000.0 * std::ldexp(1.0, -5);
    e.omega_sc = -400000000.0 * std::ldexp(1.0, -31);
    e.omega_dot_sc_s = -20000.0 * std::ldexp(1.0, -43);
    e.idot_sc_s = 300.0 * std::ldexp(1.0, -43);
    return e;
}

LnavAlmanac alm_for(unsigned sv) {
    LnavAlmanac a;
    a.sv_id = sv;
    a.e = 5000.0 * std::ldexp(1.0, -21);
    a.toa_s = 147.0 * 4096.0;
    a.delta_i_sc = 3000.0 * std::ldexp(1.0, -19);
    a.omega_dot_sc_s = -500.0 * std::ldexp(1.0, -38);
    a.health = 0;
    a.sqrt_a = 10556000.0 * std::ldexp(1.0, -11);
    a.omega0_sc = -2000000.0 * std::ldexp(1.0, -23);
    a.omega_sc = 3000000.0 * std::ldexp(1.0, -23);
    a.m0_sc = (100000.0 + sv) * std::ldexp(1.0, -23);
    a.af0_s = -100.0 * std::ldexp(1.0, -20);
    a.af1 = 12.0 * std::ldexp(1.0, -38);
    return a;
}

LnavUtcIono utc() {
    LnavUtcIono x;
    x.alpha = {11.0 * std::ldexp(1.0, -30), -3.0 * std::ldexp(1.0, -27), -60.0 * std::ldexp(1.0, -24),
               120.0 * std::ldexp(1.0, -24)};
    x.beta = {80.0 * std::ldexp(1.0, 11), -2.0 * std::ldexp(1.0, 14), -64.0 * std::ldexp(1.0, 16),
              6.0 * std::ldexp(1.0, 16)};
    x.a1 = -5.0 * std::ldexp(1.0, -50);
    x.a0_s = 1234567.0 * std::ldexp(1.0, -30);
    x.tot_s = 144.0 * 4096.0;
    x.wnt = 89;
    x.dt_ls = 18;
    x.wn_lsf = 137;
    x.dn = 7;
    x.dt_lsf = 18;
    return x;
}

// A frame of LNAV for one satellite, led by the last 150 bits of the
// previous frame's subframe 5 so the first subframe 1 arrives after the
// receiver has locked and found the bit edges.
std::vector<std::uint8_t> frame_bits(unsigned prn, std::uint32_t tow0) {
    using namespace revenant::siggen;
    std::vector<std::uint8_t> out;
    bool d29 = false;
    bool d30 = false;
    std::array<LnavWords, 6> sf;
    sf[0] = lnav_header(5, tow0);
    lnav_put_almanac(sf[0], alm_for(1 + (prn + 5) % 24));
    sf[1] = lnav_header(1, tow0 + 1);
    lnav_put_clock(sf[1], clock_for(prn));
    sf[2] = lnav_header(2, tow0 + 2);
    sf[3] = lnav_header(3, tow0 + 3);
    lnav_put_ephemeris(sf[2], sf[3], eph_for(prn));
    sf[4] = lnav_header(4, tow0 + 4);
    lnav_put_utc_iono(sf[4], utc());
    sf[5] = lnav_header(5, tow0 + 5);
    lnav_put_almanac(sf[5], alm_for(1 + prn % 24));
    for (std::size_t k = 0; k < sf.size(); ++k) {
        const auto bits = lnav_encode(sf[k], d29, d30);
        const std::size_t from = k == 0 ? 150 : 0;
        out.insert(out.end(), bits.begin() + static_cast<std::ptrdiff_t>(from), bits.end());
    }
    return out;
}

}  // namespace

TEST_CASE("LNAV parity follows Table 20-XIV and survives inversion", "[gnss][gps]") {
    siggen::LnavWords w = siggen::lnav_header(1, 12345);
    siggen::lnav_put_clock(w, clock_for(9));
    bool d29 = false;
    bool d30 = false;
    const auto bits = siggen::lnav_encode(w, d29, d30);
    REQUIRE(bits.size() == 300);
    // Words 2 and 10 end in two zeros (Figure 20-2).
    CHECK(bits[58] == 0);
    CHECK(bits[59] == 0);
    CHECK(bits[298] == 0);
    CHECK(bits[299] == 0);

    LnavFramer framer(9);
    std::vector<LnavSubframe> out;
    // Two lead bits, then the subframe, then the next header so it closes.
    std::vector<std::uint8_t> stream = {0, 0};
    stream.insert(stream.end(), bits.begin(), bits.end());
    stream.insert(stream.end(), bits.begin(), bits.begin() + 64);
    for (std::size_t i = 0; i < stream.size(); ++i) {
        framer.push(stream[i], i, out);
    }
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == 1);
    CHECK(out[0].tow_count == 12345);
    const auto c = lnav_clock(out[0]);
    REQUIRE(c);
    CHECK(c->af0 == clock_for(9).af0);
    CHECK(c->iodc == clock_for(9).iodc);

    // The whole stream inverted, as a PLL settled 180 degrees out gives it.
    LnavFramer inv(9);
    out.clear();
    for (std::size_t i = 0; i < stream.size(); ++i) {
        inv.push(static_cast<std::uint8_t>(stream[i] ^ 1U), i, out);
    }
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == 1);
    CHECK(lnav_clock(out[0])->af1 == clock_for(9).af1);

    // One flipped bit in word 7 kills the subframe.
    LnavFramer bad(9);
    out.clear();
    for (std::size_t i = 0; i < stream.size(); ++i) {
        bad.push(static_cast<std::uint8_t>(stream[i] ^ (i == 2 + 6 * 30 + 4 ? 1U : 0U)), i, out);
    }
    CHECK(out.empty());
}

TEST_CASE("GPS L1 C/A: eight satellites at 35 to 45 dB-Hz decode to LNAV data", "[gnss][gps]") {
    constexpr dsp::SampleRate kRate = 2'048'000;
    const std::array<unsigned, 8> prns = {2, 6, 10, 13, 17, 24, 27, 32};
    std::vector<siggen::GnssSatelliteSpec> sats;
    for (std::size_t i = 0; i < prns.size(); ++i) {
        siggen::GnssSatelliteSpec s;
        s.code = gps_ca_code(prns[i]);
        s.cn0_dbhz = 35.0 + 10.0 * static_cast<double>(i) / 7.0;
        s.doppler_hz = 3900.0 - 1050.0 * static_cast<double>(i);
        s.code_phase_chips = 1022.0 - 117.3 * static_cast<double>(i);
        s.carrier_phase_cycles = 0.1 * static_cast<double>(i);
        s.symbols = frame_bits(prns[i], 5000);
        s.periods_per_symbol = 20;
        sats.push_back(std::move(s));
    }
    auto synth = siggen::GnssSynth::create(kRate, sats, 4242);
    REQUIRE(synth);

    GpsL1caConfig cfg;
    cfg.sample_rate = kRate;
    auto dec = GpsL1caDecoder::create(cfg);
    REQUIRE(dec);
    GpsL1caOutput out;
    std::vector<dsp::Complex32> chunk(kRate / 4);
    double cpu = 0.0;
    const double seconds = 33.6;
    const auto blocks = static_cast<std::size_t>(seconds * 4.0);
    for (std::size_t b = 0; b < blocks; ++b) {
        synth->generate(chunk);
        const auto t0 = std::chrono::steady_clock::now();
        dec->feed(chunk, out);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    WARN("GPS L1 C/A decoder: " << cpu / seconds
                                << " s CPU per s of input at 2.048 MS/s, 32 PRNs searched, 8 tracked");

    std::map<unsigned, std::set<unsigned>> ids;
    for (const auto& sf : out.subframes) {
        ids[sf.prn].insert(sf.id);
    }
    std::map<unsigned, SatelliteStatus> status;
    for (const auto& st : out.status) {
        status[st.prn] = st;
    }
    for (std::size_t i = 0; i < prns.size(); ++i) {
        const unsigned prn = prns[i];
        INFO("PRN " << prn);
        WARN("PRN " << prn << " sent " << 35.0 + 10.0 * static_cast<double>(i) / 7.0
                    << " dB-Hz, estimated " << status[prn].cn0_dbhz << ", Doppler "
                    << status[prn].doppler_hz << ", subframes " << ids[prn].size());
        CHECK(ids[prn] == std::set<unsigned>{1, 2, 3, 4, 5});
        CHECK(std::abs(status[prn].doppler_hz - (3900.0 - 1050.0 * static_cast<double>(i))) < 5.0);
    }
    // Every decoded field equals what was sent.
    std::set<unsigned> clock_prns, eph_prns;
    for (const auto& [prn, c] : out.clocks) {
        const auto want = clock_for(prn);
        CHECK(c.week == want.week);
        CHECK(c.iodc == want.iodc);
        CHECK(c.af0 == want.af0);
        CHECK(c.af1 == want.af1);
        CHECK(c.tgd_s == want.tgd_s);
        CHECK(c.toc_s == want.toc_s);
        clock_prns.insert(prn);
    }
    for (const auto& [prn, e] : out.ephemerides) {
        const auto want = eph_for(prn);
        CHECK(e.iode == want.iode);
        CHECK(e.m0_sc == want.m0_sc);
        CHECK(e.e == want.e);
        CHECK(e.sqrt_a == want.sqrt_a);
        CHECK(e.omega0_sc == want.omega0_sc);
        CHECK(e.i0_sc == want.i0_sc);
        CHECK(e.omega_sc == want.omega_sc);
        CHECK(e.omega_dot_sc_s == want.omega_dot_sc_s);
        CHECK(e.idot_sc_s == want.idot_sc_s);
        CHECK(e.crs_m == want.crs_m);
        CHECK(e.crc_m == want.crc_m);
        CHECK(e.aodo == want.aodo);
        eph_prns.insert(prn);
    }
    CHECK(clock_prns.size() == prns.size());
    CHECK(eph_prns.size() == prns.size());
    REQUIRE(!out.almanacs.empty());
    for (const auto& a : out.almanacs) {
        const auto want = alm_for(a.sv_id);
        CHECK(a.m0_sc == want.m0_sc);
        CHECK(a.af0_s == want.af0_s);
        CHECK(a.af1 == want.af1);
        CHECK(a.sqrt_a == want.sqrt_a);
    }
    REQUIRE(!out.utc_iono.empty());
    const auto want = utc();
    for (const auto& x : out.utc_iono) {
        CHECK(x.alpha == want.alpha);
        CHECK(x.beta == want.beta);
        CHECK(x.a0_s == want.a0_s);
        CHECK(x.a1 == want.a1);
        CHECK(x.dt_ls == want.dt_ls);
        CHECK(x.wn_lsf == want.wn_lsf);
    }
}
