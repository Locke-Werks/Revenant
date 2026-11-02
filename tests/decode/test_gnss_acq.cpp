// The GNSS front end: ranging codes against the documents' printed first
// chips, acquisition sensitivity and false alarms measured, tracking against
// the synthetic constellation in core/dsp/synth/gnss_mod.h, and the prompt
// stream's independence from input chunking.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "core/decode/gnss_acq.h"
#include "core/dsp/synth/gnss_mod.h"

using namespace revenant;
using decode::gnss::AcquisitionConfig;
using decode::gnss::SignalSpec;

namespace {

// IS-GPS-200N first 10 chips, octal, as printed. Table 3-Ia (1-37), Table
// 3-Ib (38-63), Table 6-I sheets 1-5 (64-210).
constexpr std::array<unsigned, 211> kFirstChips = {
    0,
    01440, 01620, 01710, 01744, 01133, 01455, 01131, 01454, 01626, 01504, 01642, 01750, 01764,
    01772, 01775, 01776, 01156, 01467, 01633, 01715, 01746, 01763, 01063, 01706, 01743, 01761,
    01770, 01774, 01127, 01453, 01625, 01712, 01745, 01713, 01134, 01456, 01713,
    01760, 01236, 00063, 00626, 00126, 01674, 01234, 00271, 00712, 00213, 00412, 00236, 00450,
    00061, 00142, 00775, 00762, 00111, 01600, 00424, 01351, 01550, 01271, 01441, 00444, 00032,
    01523, 00175, 00617, 00663, 00435, 01752, 00254, 00731, 01373, 00332, 00723, 01705, 01515,
    01700, 01256, 00377, 00767, 00336, 01412, 01507, 01514, 01164, 01500, 00215, 00103, 00664,
    00532, 01171, 01641, 01521, 00227, 00543,
    01517, 00322, 00242, 01031, 00744, 00564, 01067, 01056, 00014, 00026, 01342, 01042, 01006,
    01637, 01666, 01121, 00761, 01315, 00766, 01225, 01732, 00673, 01220, 01413, 00671, 00536,
    01510, 01545, 00160, 00701,
    00013, 01060, 00245, 00527, 01436, 01226, 01257, 00046, 01071, 00561, 01037, 00770, 01327,
    01472, 00124, 00366, 00133, 00465, 00717, 00217, 01742, 01422, 01442, 00523, 00736, 01635,
    00136, 00273, 01026, 00003,
    01670, 00624, 00235, 00554, 00075, 01341, 00042, 00115, 00207, 00204, 01576, 01142, 00040,
    00107, 01643, 00553, 00317, 00415, 00123, 01267, 01535, 00635, 00760, 00707, 01276, 01322,
    00211, 01562, 00774, 00323,
    00112, 01306, 00027, 01470, 01505, 01013, 00355, 00727, 00170, 00030, 00472, 01237, 00414,
    01050, 01630, 00571, 00732, 01301, 01173, 00020, 00447, 01114, 00341, 01024, 01046,
};

std::vector<dsp::Complex32> make(dsp::SampleRate rate, double seconds,
                                 std::vector<siggen::GnssSatelliteSpec> sats, std::uint64_t seed,
                                 bool noise = true) {
    auto synth = siggen::GnssSynth::create(rate, std::move(sats), seed, noise);
    REQUIRE(synth);
    std::vector<dsp::Complex32> out(siggen::gnss_samples(rate, seconds));
    synth->generate(out);
    return out;
}

siggen::GnssSatelliteSpec gps_sat(unsigned prn, double cn0, double doppler, double phase) {
    siggen::GnssSatelliteSpec s;
    s.code = decode::gnss::gps_ca_code(prn);
    s.cn0_dbhz = cn0;
    s.doppler_hz = doppler;
    s.code_phase_chips = phase;
    return s;
}

SignalSpec gps_spec(unsigned prn) {
    SignalSpec s;
    s.id = prn;
    s.code = decode::gnss::gps_ca_code(prn);
    return s;
}

}  // namespace

TEST_CASE("GPS C/A codes match IS-GPS-200N first chips, PRN 1 to 210", "[gnss][acq]") {
    for (unsigned prn = 1; prn <= 210; ++prn) {
        const auto code = decode::gnss::gps_ca_code(prn);
        INFO("PRN " << prn);
        REQUIRE(code.size() == 1023);
        CHECK(decode::gnss::first_chips(code, 10) == kFirstChips[prn]);
    }
    // Table 3-Ia note **: 34 and 37 are the same code.
    CHECK(decode::gnss::gps_ca_code(34) == decode::gnss::gps_ca_code(37));
}

TEST_CASE("QZSS L1 C/A codes match IS-QZSS-PNT-006 Table 3.2.2-1", "[gnss][acq]") {
    // First 10 chips column for 193 to 202, as IS-QZSS-PNT prints them.
    constexpr std::array<unsigned, 10> kQzss = {00727, 00170, 00030, 00472, 01237,
                                                00414, 01050, 01630, 00571, 00732};
    for (unsigned i = 0; i < 10; ++i) {
        CHECK(decode::gnss::first_chips(decode::gnss::gps_ca_code(193 + i), 10) == kQzss[i]);
    }
}

TEST_CASE("GLONASS and BeiDou codes have the documented shape", "[gnss][acq]") {
    const auto st = decode::gnss::glonass_st_code();
    REQUIRE(st.size() == 511);
    // GLONASS ICD 5.1 3.3.2.1: "the first character ... in the group 111111100".
    CHECK(decode::gnss::first_chips(st, 9) == 0b111111100U);
    int sum = 0;
    for (auto c : st) {
        sum += c;
    }
    CHECK(sum == -1);  // an m-sequence has one more one than zero

    // B1I: 2046 chips; the full 2047-chip Gold code is balanced to within
    // one, so the truncated one sums to within two of zero, and distinct
    // PRNs are near-orthogonal.
    const auto b1 = decode::gnss::beidou_b1i_code(1);
    const auto b2 = decode::gnss::beidou_b1i_code(2);
    REQUIRE(b1.size() == 2046);
    int s1 = 0;
    int cross = 0;
    for (std::size_t i = 0; i < b1.size(); ++i) {
        s1 += b1[i];
        cross += b1[i] * b2[i];
    }
    CHECK(std::abs(s1) <= 66);
    CHECK(std::abs(cross) <= 130);
    CHECK(decode::gnss::beidou_nh20().size() == 20);
}

TEST_CASE("GNSS acquisition: detection probability and false alarms, measured", "[gnss][acq]") {
    constexpr dsp::SampleRate kRate = 2'048'000;
    AcquisitionConfig cfg;
    const auto spec = gps_spec(7);
    std::uint64_t seed = 1000;
    // Noise only, every PRN slot a chance at a false alarm.
    unsigned false_alarms = 0;
    double worst_noise = 0.0;
    for (unsigned t = 0; t < 6; ++t) {
        const auto block = make(kRate, 0.042, {}, ++seed);
        for (unsigned prn = 1; prn <= 4; ++prn) {
            const auto ev = decode::gnss::acquire(block, kRate, gps_spec(prn), cfg);
            worst_noise = std::max(worst_noise, ev.peak_ratio);
            false_alarms += ev.found ? 1U : 0U;
        }
    }
    INFO("noise-only peak ratio, worst of 24: " << worst_noise);
    CHECK(false_alarms == 0);
    WARN("acquisition false alarms 0 of 24, worst noise ratio " << worst_noise);

    std::map<int, unsigned> hits;
    for (int cn0 = 31; cn0 <= 37; cn0 += 2) {
        for (unsigned t = 0; t < 10; ++t) {
            const double doppler = -3700.0 + 830.0 * t;
            const double phase = 97.0 * t + 13.3;
            const auto block = make(kRate, 0.042, {gps_sat(7, cn0, doppler, phase)}, ++seed);
            const auto ev = decode::gnss::acquire(block, kRate, spec, cfg);
            const double dphase = std::fmod(std::abs(ev.code_phase_chips - phase) + 1023.0, 1023.0);
            const bool right = ev.found && std::min(dphase, 1023.0 - dphase) < 1.5 &&
                               std::abs(ev.doppler_hz - doppler) < 150.0;
            hits[cn0] += right ? 1U : 0U;
        }
        WARN("C/N0 " << cn0 << " dB-Hz: acquired " << hits[cn0] << " of 10");
    }
    CHECK(hits[35] >= 9);
    CHECK(hits[37] == 10);
}

TEST_CASE("GNSS tracking holds 8 satellites and reports C/N0 and Doppler", "[gnss][acq]") {
    constexpr dsp::SampleRate kRate = 2'048'000;
    const std::array<unsigned, 8> prns = {3, 7, 11, 14, 19, 22, 28, 31};
    std::vector<siggen::GnssSatelliteSpec> sats;
    for (std::size_t i = 0; i < prns.size(); ++i) {
        auto s = gps_sat(prns[i], 35.0 + 10.0 * static_cast<double>(i) / 7.0,
                         -4000.0 + 1100.0 * static_cast<double>(i), 100.0 * static_cast<double>(i) + 7.5);
        s.symbols.assign(200, 0);
        for (std::size_t k = 0; k < s.symbols.size(); ++k) {
            s.symbols[k] = static_cast<std::uint8_t>((k * 7 + i) % 3 == 0);
        }
        sats.push_back(std::move(s));
    }
    const auto samples = make(kRate, 3.0, sats, 77);

    decode::gnss::FrontEndConfig cfg;
    cfg.sample_rate = kRate;
    std::vector<SignalSpec> specs;
    for (unsigned p = 1; p <= 32; ++p) {
        specs.push_back(gps_spec(p));
    }
    auto fe = decode::gnss::FrontEnd::create(cfg, specs);
    REQUIRE(fe);
    decode::gnss::FrontEndOutput out;
    const auto t0 = std::chrono::steady_clock::now();
    fe->feed(samples, out);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    WARN("front end, 32 PRNs searched, 8 tracked: " << secs / 3.0
                                                            << " s of CPU per s of input at 2.048 MS/s");
    CHECK(fe->tracked() == 8);
    std::map<unsigned, decode::gnss::Prompt> last;
    for (const auto& p : out.prompts) {
        last[p.id] = p;
    }
    for (std::size_t i = 0; i < prns.size(); ++i) {
        const auto it = last.find(prns[i]);
        REQUIRE(it != last.end());
        const double want_cn0 = 35.0 + 10.0 * static_cast<double>(i) / 7.0;
        const double want_dop = -4000.0 + 1100.0 * static_cast<double>(i);
        WARN("PRN " << prns[i] << " C/N0 " << it->second.cn0_dbhz << " (sent " << want_cn0
                             << ") Doppler " << it->second.doppler_hz << " (sent " << want_dop << ")");
        CHECK(it->second.phase_locked);
        CHECK(std::abs(it->second.doppler_hz - want_dop) < 5.0);
        CHECK(std::abs(it->second.cn0_dbhz - want_cn0) < 3.0);
    }
}

TEST_CASE("GNSS prompt stream does not depend on input chunking", "[gnss][acq]") {
    constexpr dsp::SampleRate kRate = 2'048'000;
    std::vector<siggen::GnssSatelliteSpec> sats = {gps_sat(5, 44, 1234.0, 300.25),
                                                   gps_sat(9, 40, -2210.0, 17.0),
                                                   gps_sat(12, 38, 310.0, 811.0),
                                                   gps_sat(30, 42, -4400.0, 512.5)};
    const auto samples = make(kRate, 1.2, sats, 5);
    auto run = [&](std::size_t chunk) {
        decode::gnss::FrontEndConfig cfg;
        cfg.sample_rate = kRate;
        std::vector<SignalSpec> specs = {gps_spec(5), gps_spec(9), gps_spec(12), gps_spec(30),
                                         gps_spec(1)};
        auto fe = decode::gnss::FrontEnd::create(cfg, specs);
        REQUIRE(fe);
        decode::gnss::FrontEndOutput out;
        for (std::size_t pos = 0; pos < samples.size(); pos += chunk) {
            const std::size_t n = std::min(chunk, samples.size() - pos);
            fe->feed(std::span<const dsp::Complex32>(samples).subspan(pos, n), out);
        }
        std::map<unsigned, std::vector<decode::gnss::Prompt>> by;
        for (const auto& p : out.prompts) {
            by[p.id].push_back(p);
        }
        return by;
    };
    const auto whole = run(samples.size());
    REQUIRE(whole.size() == 4);
    for (std::size_t chunk : {std::size_t{1}, std::size_t{777}, std::size_t{20480}}) {
        const auto part = run(chunk);
        INFO("chunk " << chunk);
        REQUIRE(part.size() == whole.size());
        for (const auto& [id, ps] : whole) {
            const auto& qs = part.at(id);
            REQUIRE(qs.size() == ps.size());
            bool same = true;
            for (std::size_t k = 0; k < ps.size(); ++k) {
                same = same && ps[k].sample == qs[k].sample && ps[k].prompt == qs[k].prompt;
            }
            CHECK(same);
        }
    }
}

TEST_CASE("GNSS front end acquires at a rate that is not a power of two", "[gnss][acq]") {
    constexpr dsp::SampleRate kRate = 2'400'000;
    const auto samples = make(kRate, 1.0, {gps_sat(17, 42, 2500.0, 600.0)}, 9);
    decode::gnss::FrontEndConfig cfg;
    cfg.sample_rate = kRate;
    auto fe = decode::gnss::FrontEnd::create(cfg, {gps_spec(17), gps_spec(18)});
    REQUIRE(fe);
    decode::gnss::FrontEndOutput out;
    fe->feed(samples, out);
    REQUIRE(fe->tracked() == 1);
    CHECK(out.prompts.back().phase_locked);
    CHECK(std::abs(out.prompts.back().doppler_hz - 2500.0) < 5.0);
}
