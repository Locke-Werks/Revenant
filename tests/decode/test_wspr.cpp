// WSPR: G4JNT's "The WSPR Coding Process" (June 2009) worked through, and
// the receiver against the transmitter in core/dsp/synth/wsjt_slow_mod.h.
//
// The document prints no symbol listing for a message, so the coding chain
// is checked against the values its text does state, and end to end through
// JT9 in test_jt9.cpp, whose document shares the encoder and interleaver
// and does print a listing.
//
// Decode rates are measured and reported in WARN lines with loose
// assertions, for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/wspr.h"
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

std::uint32_t field(const decode::WsprPayload& b, int first, int n) {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1U) | b[static_cast<std::size_t>(first + i)];
    return v;
}

constexpr std::size_t kSlot = 120 * 12000;

}  // namespace

TEST_CASE("WSPR source coding follows the document's worked rules", "[wspr]") {
    // "G4JNT will become [sp]G4JNT whereas GD4JNT stays as-is."
    auto a = decode::wspr_pack("G4JNT", "IO90", 30);
    REQUIRE(a);
    const std::uint32_t n = field(*a, 0, 28);
    // [sp]G4JNT built up as the document's N1 to N6.
    std::uint32_t want = 36;
    want = want * 36 + 16;  // G
    want = want * 10 + 4;
    want = 27 * want + (19 - 10);  // J
    want = 27 * want + (23 - 10);  // N
    want = 27 * want + (29 - 10);  // T
    CHECK(n == want);
    // "M1 = (179 - 10 * [Loc 1] - [Loc 3]) * 180 + 10 * [Loc 2] + [Loc 4]",
    // "M = M1 * 128 + [Pwr] + 64".
    const std::uint32_t m1 = (179 - 10 * 8 - 9) * 180 + 10 * 14 + 0;
    CHECK(field(*a, 28, 22) == m1 * 128 + 30 + 64);
    // "from AA00 = 32220 to RR99 = 179".
    auto aa = decode::wspr_pack("K1ABC", "AA00", 0);
    auto rr = decode::wspr_pack("K1ABC", "RR99", 0);
    REQUIRE(aa);
    REQUIRE(rr);
    CHECK(field(*aa, 28, 22) / 128 == 32220);
    CHECK(field(*rr, 28, 22) / 128 == 179);
    // "37 * 36 * 10 * 27 * 27 * 27 = 262177560".
    CHECK(37ULL * 36 * 10 * 27 * 27 * 27 == 262177560ULL);

    for (const auto& [call, grid, dbm] : {std::tuple{"G4JNT", "IO90", 30}, {"GD4JNT", "IO90", 37},
                                          {"K1ABC", "FN42", 0}, {"W9XYZ", "EN37", 60}, {"2E0ABC", "JO01", 23}}) {
        auto b = decode::wspr_pack(call, grid, dbm);
        REQUIRE(b);
        const auto m = decode::wspr_unpack(*b);
        REQUIRE(m);
        CHECK(m->call == call);
        CHECK(m->grid == grid);
        CHECK(m->power_dbm == dbm);
        CHECK_FALSE(m->odd_power);
    }
    // "Illegal values are labelled when decoded."
    auto odd = decode::wspr_pack("K1ABC", "FN42", 31);
    REQUIRE(odd);
    CHECK(decode::wspr_unpack(*odd)->odd_power);

    CHECK_FALSE(decode::wspr_pack("K1ABC1", "FN42", 30));  // ends in a digit
    CHECK_FALSE(decode::wspr_pack("K1ABC", "SN42", 30));   // S is past R
    CHECK_FALSE(decode::wspr_pack("K1ABC", "FN42", 61));
}

TEST_CASE("WSPR interleaver and sync vector as described", "[wspr]") {
    // "I = 1 gives J = 128": address 0 takes source bit 0 and address 1 takes
    // source bit 1, which lands at 128.
    std::vector<std::uint8_t> s(162, 0);
    s[1] = 1;
    auto d = decode::wsjt_interleave(s);
    CHECK(d[128] == 1);
    CHECK(std::count(d.begin(), d.end(), 1) == 1);
    // A permutation that the deinterleaver undoes.
    std::vector<float> v(162);
    for (int i = 0; i < 162; ++i) v[static_cast<std::size_t>(i)] = static_cast<float>(i);
    std::vector<float> back = decode::wsjt_deinterleave(v);
    std::set<float> seen(back.begin(), back.end());
    CHECK(seen.size() == 162);
    CHECK(decode::wspr_sync_vector().size() == 162);

    // 50 bits and 31 zeros give 162 coded bits; every tone is 0 to 3 with
    // the sync bit as its low bit.
    auto b = decode::wspr_pack("K1ABC", "FN42", 37);
    REQUIRE(b);
    const auto tones = decode::wspr_channel_tones(*b);
    REQUIRE(tones.size() == 162);
    for (int i = 0; i < 162; ++i) {
        CHECK((tones[static_cast<std::size_t>(i)] & 1) == decode::wspr_sync_vector()[static_cast<std::size_t>(i)]);
        CHECK(tones[static_cast<std::size_t>(i)] >= 0);
        CHECK(tones[static_cast<std::size_t>(i)] <= 3);
    }
    // "approximately 0.683 seconds ... around 110.6 seconds".
    const auto& p = decode::wspr_protocol();
    CHECK(std::abs(p.symbol_seconds - 0.683) < 0.001);
    CHECK(std::abs(p.symbols * p.symbol_seconds - 110.6) < 0.1);
}

TEST_CASE("WSPR decodes several signals with offsets and drift in one slot", "[wspr]") {
    struct Sig {
        const char* call;
        const char* grid;
        int dbm;
        double f;
        double dt;
        double drift;
    };
    const Sig sigs[] = {
        {"K1ABC", "FN42", 37, 1400.0, 1.0, 0.0},   {"G4JNT", "IO90", 30, 1440.3, 0.5, 1.5},
        {"W9XYZ", "EN37", 23, 1480.7, 2.0, -2.0},  {"PA9XYZ", "JO22", 10, 1520.0, 1.7, 0.0},
        {"2E0ABC", "JO01", 33, 1560.25, 0.2, 3.0}, {"VK2ABC", "QF56", 40, 1600.5, 1.2, -1.0},
    };
    std::vector<float> slot(kSlot, 0.0F);
    for (const auto& s : sigs) {
        siggen::SlowModConfig c;
        c.frequency_hz = s.f;
        c.start_seconds = s.dt;
        c.drift_hz = s.drift;
        c.amplitude = 0.05;
        REQUIRE(siggen::wspr_add(c, s.call, s.grid, s.dbm, slot));
    }
    add_noise(slot, 0.05, -24.0, 5);
    auto dec = decode::WsprDecoder::create({});
    REQUIRE(dec);
    std::vector<decode::WsprDecode> out;
    dec->decode_slot(slot, 0.0, 0, out);
    REQUIRE(out.size() == std::size(sigs));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto& s = sigs[i];
        INFO(s.call);
        CHECK(out[i].message.call == s.call);
        CHECK(out[i].message.grid == s.grid);
        CHECK(out[i].message.power_dbm == s.dbm);
        CHECK(std::abs(out[i].frequency_hz - s.f) < 0.5);
        CHECK(std::abs(out[i].dt_seconds - (s.dt - 1.0)) < 0.1);
        CHECK(std::abs(out[i].drift_hz - s.drift) < 1.0);
        CHECK(std::abs(out[i].snr_db + 24.0) < 3.0);
    }
}

TEST_CASE("WSPR stream at 48 kHz is chunk independent", "[wspr]") {
    const double t0 = 1'700'000'040.0 + 50.0;  // 1700000040 is a multiple of 120
    const int rate = 48000;
    const std::size_t lead = static_cast<std::size_t>(70.0 * rate);
    std::vector<float> audio(lead + 120 * rate + rate, 0.0F);
    siggen::SlowModConfig c;
    c.rate = rate;
    c.amplitude = 0.1;
    c.frequency_hz = 1500.0;
    c.start_seconds = 70.0 + 1.0;
    REQUIRE(siggen::wspr_add(c, "K1ABC", "FN42", 37, audio));
    add_noise(audio, 0.1 * 2.0, -20.0, 11);

    const auto run = [&](std::uint32_t seed) {
        decode::WsprConfig cfg;
        cfg.rate = rate;
        cfg.utc_of_sample = [t0](dsp::SampleIndex) { return t0; };
        auto dec = decode::WsprDecoder::create(cfg);
        REQUIRE(dec);
        std::vector<decode::WsprDecode> out;
        std::mt19937 rng(seed);
        std::size_t pos = 0;
        while (pos < audio.size()) {
            const std::size_t n = std::min<std::size_t>(audio.size() - pos, 1 + rng() % 200000);
            REQUIRE(dec->process(std::span<const float>(audio.data() + pos, n), out));
            pos += n;
        }
        return out;
    };
    const auto a = run(1);
    const auto b = run(2);
    REQUIRE(a.size() == 1);
    CHECK(a[0].message.text == "K1ABC FN42 37");
    CHECK(std::fmod(a[0].slot_utc, 120.0) == 0.0);
    CHECK(a[0].slot_start_sample == lead);
    CHECK(std::abs(a[0].dt_seconds) < 0.1);
    REQUIRE(b.size() == 1);
    CHECK(a[0].frequency_hz == b[0].frequency_hz);
    CHECK(a[0].snr_db == b[0].snr_db);
}

TEST_CASE("WSPR decode threshold in AWGN", "[wspr][threshold]") {
    auto dec = decode::WsprDecoder::create({});
    REQUIRE(dec);
    const char* calls[] = {"K1ABC", "G4JNT", "W9XYZ", "PA9XYZ", "VK2ABC", "JA1XYZ", "F5ABC", "DL1ABC"};
    const char* grids[] = {"FN42", "IO90", "EN37", "JO22", "QF56", "PM95", "JN18", "JO62"};
    double threshold = 0.0;
    for (int snr = -26; snr >= -32; --snr) {
        int ok = 0;
        int total = 0;
        for (int trial = 0; trial < 2; ++trial) {
            std::vector<float> slot(kSlot, 0.0F);
            for (int i = 0; i < 8; ++i) {
                siggen::SlowModConfig c;
                c.amplitude = 0.05;
                c.frequency_hz = 1380.0 + 32.0 * i + 3.0 * trial;
                c.start_seconds = 0.5 + 0.3 * i;
                c.drift_hz = (i % 3) - 1.0;
                REQUIRE(siggen::wspr_add(c, calls[i], grids[i], 37, slot));
            }
            add_noise(slot, 0.05, snr, static_cast<std::uint32_t>(500 + snr * 7 + trial));
            std::vector<decode::WsprDecode> out;
            dec->decode_slot(slot, 0.0, 0, out);
            std::set<std::string> got;
            for (const auto& d : out) got.insert(d.message.text);
            for (int i = 0; i < 8; ++i) ok += got.contains(std::string(calls[i]) + " " + grids[i] + " 37") ? 1 : 0;
            total += 8;
            for (const auto& g : got) {
                bool sent = false;
                for (int i = 0; i < 8; ++i) sent = sent || g == std::string(calls[i]) + " " + grids[i] + " 37";
                CHECK(sent);
            }
        }
        const double rate = static_cast<double>(ok) / total;
        WARN("WSPR AWGN SNR " << snr << " dB: " << ok << "/" << total << " decoded");
        if (rate >= 0.5) threshold = snr;
        if (snr >= -26) CHECK(rate >= 0.9);
    }
    WARN("WSPR lowest SNR with at least half decoded: " << threshold << " dB");
    CHECK(threshold <= -27.0);
}
