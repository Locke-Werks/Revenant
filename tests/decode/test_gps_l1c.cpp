// GPS L1C CNAV-2: codes against IS-GPS-800J's own tables, the channel
// coding round trip, and a synthetic constellation decoded by handover from
// C/A.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/gps_l1c.h"
#include "core/decode/gps_l1c_tables.h"
#include "core/dsp/synth/gnss_mod.h"
#include "core/dsp/synth/gps_l1c_mod.h"

using namespace revenant;
using namespace revenant::decode::gnss;

namespace {

unsigned last_chips(const std::vector<std::int8_t>& code, unsigned count) {
    return first_chips(std::span<const std::int8_t>(code).last(count), count);
}

std::vector<std::uint8_t> random_bits(std::size_t n, std::mt19937& rng) {
    std::vector<std::uint8_t> v(n);
    for (auto& b : v) {
        b = static_cast<std::uint8_t>(rng() & 1U);
    }
    return v;
}

void put_bits(std::vector<std::uint8_t>& v, unsigned first, unsigned count, unsigned value) {
    for (unsigned i = 0; i < count; ++i) {
        v[first - 1 + i] = static_cast<std::uint8_t>((value >> (count - 1 - i)) & 1U);
    }
}

}  // namespace

TEST_CASE("L1C ranging codes reproduce Table 3.2-2", "[gnss][l1c]") {
    // Figure in 6.2.3: the Legendre sequence starts 0111101111001010.
    const auto& l = l1c_legendre();
    REQUIRE(l.size() == 10223);
    const std::vector<std::uint8_t> head = {0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0};
    CHECK(std::vector<std::uint8_t>(l.begin(), l.begin() + 16) == head);
    for (unsigned prn = 1; prn <= 63; ++prn) {
        INFO("PRN " << prn);
        const auto& row = l1c_tables::kRanging[prn - 1];
        const auto p = l1c_code(prn, true);
        const auto d = l1c_code(prn, false);
        REQUIRE(p.size() == 10230);
        REQUIRE(d.size() == 10230);
        CHECK(first_chips(p, 24) == row.pilot_first24);
        CHECK(last_chips(p, 24) == row.pilot_last24);
        CHECK(first_chips(d, 24) == row.data_first24);
        CHECK(last_chips(d, 24) == row.data_last24);
    }
}

TEST_CASE("L1C overlay reproduces Table 3.2-3", "[gnss][l1c]") {
    for (unsigned prn = 1; prn <= 63; ++prn) {
        INFO("PRN " << prn);
        const auto o = l1c_overlay(prn);
        REQUIRE(o.size() == 1800);
        CHECK(first_chips(o, 11) == l1c_tables::kOverlay[prn - 1].initial);
        CHECK(last_chips(o, 11) == l1c_tables::kOverlay[prn - 1].final_bits);
    }
}

TEST_CASE("L1C LDPC matrices are the shape 3.2.3.4 describes", "[gnss][l1c]") {
    for (unsigned sf : {2U, 3U}) {
        INFO("subframe " << sf);
        const auto checks = l1c_ldpc_checks(sf);
        const std::size_t n = sf == 2 ? 1200 : 548;
        REQUIRE(checks.size() == n / 2);
        auto dec = decode::LdpcDecoder::create({n, checks});
        REQUIRE(dec);
        std::mt19937 rng(sf);
        for (int trial = 0; trial < 20; ++trial) {
            const auto info = random_bits(n / 2, rng);
            const auto word = l1c_ldpc_encode(sf, info);
            REQUIRE(word.size() == n);
            CHECK(dec->syndrome_zero(word));
            CHECK(std::equal(info.begin(), info.end(), word.begin()));
        }
        // Through noise at Es/N0 = 0 dB (Eb/N0 = 3 dB), where hard
        // decisions are wrong one time in thirteen.
        std::normal_distribution<float> noise(0.0F, std::sqrt(0.5F));
        unsigned good = 0;
        for (int trial = 0; trial < 20; ++trial) {
            const auto word = l1c_ldpc_encode(sf, random_bits(n / 2, rng));
            std::vector<float> llr(n);
            for (std::size_t i = 0; i < n; ++i) {
                llr[i] = (word[i] != 0 ? -1.0F : 1.0F) + noise(rng);
            }
            std::vector<std::uint8_t> bits(n);
            const auto r = dec->decode(llr, bits);
            good += r.converged && bits == word ? 1U : 0U;
        }
        WARN("L1C subframe " << sf << " LDPC at Eb/N0 3 dB: " << good << " of 20 words");
        CHECK(good >= 18);
    }
    // T is lower triangular with ones on its diagonal.
    auto lower_unit = [](std::span<const l1c_tables::Coord> t, unsigned size) {
        std::set<unsigned> diag;
        for (const auto& c : t) {
            CHECK(c.col <= c.row);
            if (c.col == c.row) {
                diag.insert(c.row);
            }
        }
        CHECK(diag.size() == size);
    };
    lower_unit(l1c_tables::kSf2T, 599);
    lower_unit(l1c_tables::kSf3T, 273);
}

TEST_CASE("L1C TOI, CRC-24Q and the interleaver round trip", "[gnss][l1c]") {
    for (unsigned toi = 0; toi < 512; ++toi) {
        const auto sym = l1c_toi_encode(toi);
        std::vector<float> soft(52);
        for (std::size_t i = 0; i < 52; ++i) {
            soft[i] = sym[i] != 0 ? -1.0F : 1.0F;
        }
        // Eleven symbols inverted at half weight still decode.
        for (std::size_t i = 0; i < 52; i += 5) {
            soft[i] = -soft[i] * 0.5F;
        }
        CHECK(l1c_toi_decode(soft) == toi);
    }
    // The CRC-24Q check value for the ASCII string "123456789".
    std::vector<std::uint8_t> bits;
    for (char ch : std::string("123456789")) {
        for (int i = 7; i >= 0; --i) {
            bits.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(ch) >> i) & 1U));
        }
    }
    CHECK(crc24q(bits) == 0xCDE703U);
    std::mt19937 rng(5);
    const auto v = random_bits(1748, rng);
    const auto i = l1c_interleave(v);
    CHECK(l1c_deinterleave(std::span<const std::uint8_t>(i)) == v);
    CHECK(i[1] == v[46]);  // column 1 is read top to bottom
}

namespace {

struct Scenario {
    std::vector<siggen::L1cSatellite> sats;
    std::map<unsigned, std::vector<std::uint8_t>> sf2, sf3;
};

Scenario scenario() {
    Scenario s;
    std::mt19937 rng(800);
    const std::array<unsigned, 6> prns = {3, 8, 14, 19, 25, 31};
    for (std::size_t i = 0; i < prns.size(); ++i) {
        const unsigned prn = prns[i];
        auto sf2 = random_bits(576, rng);
        put_bits(sf2, 1, 13, 2345);
        put_bits(sf2, 14, 8, 77);
        put_bits(sf2, 22, 11, 100 + prn);
        put_bits(sf2, 33, 1, 0);
        put_bits(sf2, 34, 5, 30);  // -2
        put_bits(sf2, 39, 11, 200 + prn);
        auto sf3 = random_bits(250, rng);
        put_bits(sf3, 1, 8, prn);
        put_bits(sf3, 9, 6, 1);
        siggen::L1cSatellite sat;
        sat.prn = prn;
        sat.ca_code_phase_chips = 37.25 + 151.5 * static_cast<double>(i);
        sat.epoch_offset = static_cast<unsigned>((3 * i + 1) % 10);
        sat.lead_periods = 1250 + 20 * static_cast<unsigned>(i);
        sat.doppler_hz = 3100.0 - 1150.0 * static_cast<double>(i);
        sat.ca_cn0_dbhz = 35.0 + 2.0 * static_cast<double>(i);
        sat.frames = {siggen::l1c_frame_symbols(17, sf2, sf3),
                      siggen::l1c_frame_symbols(18, sf2, sf3)};
        s.sf2[prn] = sf2;
        s.sf3[prn] = sf3;
        s.sats.push_back(std::move(sat));
    }
    return s;
}

std::vector<siggen::GnssSatelliteSpec> specs(const Scenario& s) {
    std::vector<siggen::GnssSatelliteSpec> out;
    for (const auto& sat : s.sats) {
        for (auto& sp : siggen::l1c_satellite(sat)) {
            out.push_back(std::move(sp));
        }
    }
    return out;
}

}  // namespace

TEST_CASE("L1C decodes six satellites by handover from C/A", "[gnss][l1c]") {
    constexpr dsp::SampleRate kRate = 4'096'000;
    const auto sc = scenario();
    auto synth = siggen::GnssSynth::create(kRate, specs(sc), 4242);
    REQUIRE(synth);
    GpsL1cConfig cfg;
    cfg.sample_rate = kRate;
    auto dec = GpsL1cDecoder::create(cfg);
    REQUIRE(dec);
    GpsL1cOutput out;
    std::vector<dsp::Complex32> chunk(kRate / 4);
    const double seconds = 32.0;
    double cpu = 0.0;
    for (std::size_t b = 0; b < static_cast<std::size_t>(seconds * 4.0); ++b) {
        synth->generate(chunk);
        const auto t0 = std::chrono::steady_clock::now();
        dec->feed(chunk, out);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    WARN("GPS L1C decoder: " << cpu / seconds
                             << " s CPU per s of input at 4.096 MS/s, 32 C/A PRNs searched, 6 handed over");

    std::map<unsigned, unsigned> attempts, successes;
    for (const auto& h : out.handovers) {
        ++attempts[h.prn];
        successes[h.prn] += h.success ? 1U : 0U;
        WARN("handover PRN " << h.prn << (h.success ? " ok" : " failed") << " ratio " << h.ratio
                             << " candidate " << h.ca_epochs_after << " at "
                             << static_cast<double>(h.sample) / kRate << " s");
    }
    std::map<unsigned, L1cStatus> status;
    for (const auto& st : out.status) {
        status[st.prn] = st;
    }
    std::set<unsigned> decoded;
    for (const auto& f : out.frames) {
        INFO("PRN " << f.prn);
        WARN("frame PRN " << f.prn << " at " << static_cast<double>(f.sample) / kRate
                          << " s, LDPC iterations " << f.sf2_iterations << " and "
                          << f.sf3_iterations);
        CHECK(f.sf2_ok);
        CHECK(f.sf3_ok);
        CHECK(f.toi == 18);
        CHECK(f.sf2 == sc.sf2.at(f.prn));
        CHECK(f.sf3 == sc.sf3.at(f.prn));
        CHECK(f.clock.wn == 2345);
        CHECK(f.clock.ura_ed == -2);
        CHECK(f.clock.toe == 200 + f.prn);
        CHECK(f.sf3_prn == f.prn);
        CHECK(f.sf3_page == 1);
        if (f.sf2_ok && f.sf3_ok) {
            decoded.insert(f.prn);
        }
    }
    for (const auto& sat : sc.sats) {
        INFO("PRN " << sat.prn);
        WARN("PRN " << sat.prn << " C/A " << sat.ca_cn0_dbhz << " dB-Hz, pilot estimated "
                    << status[sat.prn].cn0_dbhz << ", handovers " << successes[sat.prn] << "/"
                    << attempts[sat.prn]);
        CHECK(successes[sat.prn] == 1);
        CHECK(attempts[sat.prn] == 1);
        CHECK(std::abs(status[sat.prn].doppler_hz - sat.doppler_hz) < 5.0);
    }
    CHECK(decoded.size() == sc.sats.size());
}

TEST_CASE("L1C handover and tracking do not depend on chunking", "[gnss][l1c]") {
    constexpr dsp::SampleRate kRate = 2'048'000;
    auto sc = scenario();
    sc.sats.resize(2);
    const auto all = [&] {
        auto synth = siggen::GnssSynth::create(kRate, specs(sc), 7);
        std::vector<dsp::Complex32> v(siggen::gnss_samples(kRate, 4.0));
        synth->generate(v);
        return v;
    }();
    auto run = [&](std::size_t chunk) {
        GpsL1cConfig cfg;
        cfg.sample_rate = kRate;
        cfg.prns = {3, 8};
        cfg.status_periods = 10;
        auto dec = GpsL1cDecoder::create(cfg);
        GpsL1cOutput out;
        for (std::size_t i = 0; i < all.size(); i += chunk) {
            const auto n = std::min(chunk, all.size() - i);
            dec->feed(std::span<const dsp::Complex32>(all).subspan(i, n), out);
        }
        return out;
    };
    auto a = run(4096 * 25);
    auto b = run(1777);
    // Satellites interleave differently with chunking; each one's own
    // sequence must not.
    auto by_prn = [](GpsL1cOutput& o) {
        std::stable_sort(o.handovers.begin(), o.handovers.end(),
                         [](const auto& x, const auto& y) { return x.prn < y.prn; });
        std::stable_sort(o.status.begin(), o.status.end(),
                         [](const auto& x, const auto& y) { return x.prn < y.prn; });
    };
    by_prn(a);
    by_prn(b);
    REQUIRE(a.handovers.size() == 2);
    REQUIRE(a.handovers.size() == b.handovers.size());
    REQUIRE(!a.status.empty());
    for (std::size_t i = 0; i < a.handovers.size(); ++i) {
        CHECK(a.handovers[i].success);
        CHECK(a.handovers[i].prn == b.handovers[i].prn);
        CHECK(a.handovers[i].sample == b.handovers[i].sample);
        CHECK(a.handovers[i].ratio == b.handovers[i].ratio);
    }
    REQUIRE(a.status.size() == b.status.size());
    for (std::size_t i = 0; i < a.status.size(); ++i) {
        CHECK(a.status[i].prn == b.status[i].prn);
        CHECK(a.status[i].doppler_hz == b.status[i].doppler_hz);
    }
}
