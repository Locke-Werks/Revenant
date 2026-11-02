// Metop AHRPT framing, core/decode/ahrpt.h, against core/dsp/synth/ahrpt_mod.h:
// the rate 3/4 punctured code over QPSK at every carrier phase, instrument
// naming from TD 18 table 1, Doppler at L band, noise, and any blocking.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "core/decode/ahrpt.h"
#include "core/dsp/synth/ahrpt_mod.h"
#include "core/dsp/synth/ccsds_mod.h"
#include "core/dsp/synth/channel.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

std::vector<std::vector<std::uint8_t>> packets(std::uint16_t apid, std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::vector<std::uint8_t>> out;
    for (std::size_t i = 0; i < n; ++i) {
        SpacePacketHeader h;
        h.apid = apid;
        h.secondary_header = true;
        h.sequence_count = static_cast<std::uint16_t>(i);
        std::vector<std::uint8_t> d(200 + rng() % 1500);
        for (auto& o : d) {
            o = static_cast<std::uint8_t>(rng());
        }
        out.push_back(siggen::ccsds_space_packet(h, d));
    }
    return out;
}

struct Got {
    std::vector<std::vector<std::uint8_t>> octets;
    std::vector<std::string> names;
};

Got run(const std::vector<dsp::Complex32>& s, std::size_t chunk, CcsdsDownlinkStats* stats = nullptr) {
    Got g;
    AhrptEvents ev;
    ev.on_packet = [&](const AhrptPacket& p) {
        g.octets.push_back(p.packet->octets);
        g.names.emplace_back(p.instrument);
    };
    auto d = AhrptDecoder::create({}, std::move(ev)).value();
    for (std::size_t i = 0; i < s.size(); i += chunk) {
        d.push(std::span(s).subspan(i, std::min(chunk, s.size() - i)));
    }
    d.finish();
    if (stats != nullptr) {
        *stats = d.stats();
    }
    return g;
}

const std::vector<siggen::AhrptChannel>& traffic() {
    static const std::vector<siggen::AhrptChannel> t = {
        {9, packets(103, 8, 1)},
        {3, packets(39, 3, 2)},
        {34, packets(6, 2, 3)},
    };
    return t;
}

std::size_t total() {
    std::size_t n = 0;
    for (const auto& c : traffic()) {
        n += c.packets.size();
    }
    return n;
}

}  // namespace

TEST_CASE("AHRPT instrument names follow TD 18 table 1", "[decode][ahrpt]") {
    CHECK(ahrpt_instrument_name(9, 103) == "AVHRR/3");
    CHECK(ahrpt_instrument_name(3, 39) == "AMSU-A1");
    CHECK(ahrpt_instrument_name(3, 38) == "HIRS/4");
    CHECK(ahrpt_instrument_name(34, 6) == "Admin message");
    CHECK(ahrpt_instrument_name(24, 400) == "GOME-2");
    CHECK(ahrpt_instrument_name(5, 1).empty());
    CHECK(kAhrptSymbolRate > 2.333e6);
    CHECK(kAhrptSymbolRate < 2.334e6);
}

TEST_CASE("AHRPT packets round trip at every carrier phase", "[decode][ahrpt]") {
    for (const double phase : {0.2, 0.2 + std::numbers::pi / 2, 0.2 + std::numbers::pi, 0.2 + 1.5 * std::numbers::pi}) {
        siggen::AhrptTxSpec tx;
        tx.initial_phase = phase;
        tx.carrier_offset_hz = 25000.0;
        tx.timing_offset_samples = 0.3;
        const auto s = siggen::ahrpt_baseband(tx, traffic()).value();
        const auto g = run(s, 65536);
        INFO("phase " << phase);
        REQUIRE(g.octets.size() == total());
        std::size_t k = 0;
        for (const auto& c : traffic()) {
            for (const auto& p : c.packets) {
                CHECK(g.octets[k] == p);
                ++k;
            }
        }
        CHECK(std::count(g.names.begin(), g.names.end(), "AVHRR/3") == 8);
        CHECK(std::count(g.names.begin(), g.names.end(), "AMSU-A1") == 3);
    }
}

TEST_CASE("AHRPT decodes the same in any blocking", "[decode][ahrpt]") {
    siggen::AhrptTxSpec tx;
    tx.initial_phase = 1.0;
    auto s = siggen::ahrpt_baseband(tx, traffic()).value();
    REQUIRE(siggen::add_awgn(s, siggen::NoiseLevel::eb_over_n0_db(7.0, kAhrptDataRate * 223 / 255), 6000000, 5)
                .has_value());
    const auto a = run(s, s.size());
    const auto b = run(s, 10007);
    const auto c = run(s, 513);
    CHECK(a.octets == b.octets);
    CHECK(a.octets == c.octets);
}

TEST_CASE("AHRPT with Doppler, drift and noise", "[decode][ahrpt]") {
    for (const double eb_n0 : {4.0, 5.0, 6.0, 8.0}) {
        siggen::AhrptTxSpec tx;
        tx.initial_phase = 2.2;
        tx.clock_error = 20e-6;
        auto s = siggen::ahrpt_baseband(tx, traffic()).value();
        // 1701 MHz from 820 km: up to about 40 kHz, sweeping near 600 Hz/s
        // at closest approach.
        siggen::FrequencyConfig fc;
        fc.doppler_shift_hz = 38000;
        fc.drift_hz_per_second = -600.0;
        REQUIRE(siggen::apply_frequency_offset(s, fc, 6000000, 1).has_value());
        REQUIRE(siggen::add_awgn(s, siggen::NoiseLevel::eb_over_n0_db(eb_n0, kAhrptDataRate * 223.0 / 255.0),
                                 6000000, static_cast<std::uint64_t>(eb_n0 * 10))
                    .has_value());
        CcsdsDownlinkStats st;
        const auto g = run(s, 1 << 16, &st);
        std::size_t m = 0;
        for (const auto& c : traffic()) {
            for (const auto& p : c.packets) {
                m += std::find(g.octets.begin(), g.octets.end(), p) != g.octets.end() ? 1 : 0;
            }
        }
        std::printf("AHRPT r=3/4 QPSK Eb/N0 %.1f dB, 38 kHz at -600 Hz/s: %zu frames, %zu failed, %zu RS symbols "
                    "corrected, %zu/%zu packets\n",
                    eb_n0, st.frames, st.frames_failed, st.rs_corrected, m, total());
        if (eb_n0 >= 6.0) {
            CHECK(m == total());
        }
    }
}
