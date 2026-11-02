// Meteor-M LRPT framing, core/decode/lrpt.h, against core/dsp/synth/lrpt_mod.h:
// QPSK and OQPSK, every phase the demodulator can settle at, Doppler and its
// drift at 137 MHz, white noise, and any blocking. Error rates are measured
// and printed; the assertions around them are loose.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <cstdio>
#include <random>
#include <vector>

#include "core/decode/lrpt.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/ccsds_mod.h"
#include "core/dsp/synth/lrpt_mod.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

std::vector<std::vector<std::uint8_t>> make_packets(std::size_t n, std::uint16_t apid_base, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::vector<std::uint8_t>> out;
    for (std::size_t i = 0; i < n; ++i) {
        SpacePacketHeader h;
        h.apid = static_cast<std::uint16_t>(apid_base + i % 3);
        h.sequence_count = static_cast<std::uint16_t>(i);
        std::vector<std::uint8_t> data(30 + rng() % 900);
        for (auto& o : data) {
            o = static_cast<std::uint8_t>(rng());
        }
        out.push_back(siggen::ccsds_space_packet(h, data));
    }
    return out;
}

struct Got {
    std::vector<std::vector<std::uint8_t>> packets;
    std::size_t frames = 0;
};

Got run(const LrptConfig& cfg, const std::vector<dsp::Complex32>& s, std::size_t chunk) {
    Got g;
    LrptEvents ev;
    ev.on_frame = [&](const CcsdsDownlinkFrame&) { ++g.frames; };
    ev.on_packet = [&](std::uint8_t, const SpacePacket& p) { g.packets.push_back(p.octets); };
    auto d = LrptDecoder::create(cfg, std::move(ev)).value();
    for (std::size_t i = 0; i < s.size(); i += chunk) {
        d.push(std::span(s).subspan(i, std::min(chunk, s.size() - i)));
    }
    d.finish();
    return g;
}

std::size_t matched(const Got& g, const std::vector<std::vector<std::uint8_t>>& want) {
    std::size_t m = 0;
    for (const auto& p : want) {
        m += std::find(g.packets.begin(), g.packets.end(), p) != g.packets.end() ? 1 : 0;
    }
    return m;
}

}  // namespace

TEST_CASE("LRPT packets round trip at every carrier phase, QPSK and OQPSK", "[decode][lrpt]") {
    const auto a = make_packets(12, 64, 1);
    const auto b = make_packets(6, 70, 2);
    std::vector<std::vector<std::uint8_t>> all = a;
    all.insert(all.end(), b.begin(), b.end());
    for (const bool oqpsk : {false, true}) {
        for (const double phase : {0.3, 0.3 + std::numbers::pi / 2, 0.3 + std::numbers::pi, 0.3 + 3 * std::numbers::pi / 2}) {
            siggen::LrptTxSpec tx;
            tx.oqpsk = oqpsk;
            tx.initial_phase = phase;
            tx.carrier_offset_hz = 1500.0;
            tx.timing_offset_samples = 0.37;
            const auto s = siggen::lrpt_baseband(tx, {{5, a}, {2, b}}).value();
            LrptConfig cfg;
            cfg.oqpsk = oqpsk;
            const auto g = run(cfg, s, 8192);
            INFO("oqpsk " << oqpsk << " phase " << phase);
            CHECK(matched(g, all) == all.size());
            CHECK(g.packets.size() == all.size());
        }
    }
}

TEST_CASE("LRPT APIDs are counted per virtual channel", "[decode][lrpt]") {
    const auto a = make_packets(9, 64, 3);
    siggen::LrptTxSpec tx;
    const auto s = siggen::lrpt_baseband(tx, {{5, a}}).value();
    auto d = LrptDecoder::create({}, {}).value();
    d.push(s);
    d.finish();
    REQUIRE(d.apids().size() == 3);
    for (const auto& [apid, st] : d.apids()) {
        CHECK(apid >= 64);
        CHECK(apid <= 66);
        CHECK(st.packets == 3);
        CHECK(st.virtual_channel == 5);
    }
}

TEST_CASE("LRPT decodes the same in any blocking", "[decode][lrpt]") {
    const auto a = make_packets(10, 64, 4);
    siggen::LrptTxSpec tx;
    tx.initial_phase = 2.0;
    tx.carrier_offset_hz = -800.0;
    auto s = siggen::lrpt_baseband(tx, {{5, a}}).value();
    REQUIRE(siggen::add_awgn(s, siggen::NoiseLevel::eb_over_n0_db(6.0, 72000.0 * 223 / 255), 288000, 11)
                .has_value());
    const auto whole = run({}, s, s.size());
    const auto pieces = run({}, s, 4093);
    const auto small = run({}, s, 97);
    CHECK(whole.packets == pieces.packets);
    CHECK(whole.packets == small.packets);
    CHECK(whole.frames == pieces.frames);
}

TEST_CASE("LRPT with Doppler, drift and noise", "[decode][lrpt]") {
    const auto a = make_packets(30, 64, 5);
    for (const double eb_n0 : {3.0, 4.0, 6.0}) {
        for (const bool oqpsk : {false, true}) {
            siggen::LrptTxSpec tx;
            tx.oqpsk = oqpsk;
            tx.initial_phase = 0.9;
            tx.clock_error = 30e-6;
            auto s = siggen::lrpt_baseband(tx, {{5, a}}).value();
            // 137.1 MHz from low orbit: about 3 kHz either side, sweeping
            // fastest, near 50 Hz/s, at closest approach.
            siggen::FrequencyConfig fc;
            fc.doppler_shift_hz = 2500;
            fc.drift_hz_per_second = -50.0;
            REQUIRE(siggen::apply_frequency_offset(s, fc, 288000, 3).has_value());
            // Eb per information bit: QPSK carries two coded bits a symbol,
            // r = 1/2, then RS 223/255.
            const double info = 72000.0 * 223.0 / 255.0;
            REQUIRE(siggen::add_awgn(s, siggen::NoiseLevel::eb_over_n0_db(eb_n0, info), 288000,
                                     static_cast<std::uint64_t>(eb_n0 * 10 + oqpsk))
                        .has_value());
            LrptConfig cfg;
            cfg.oqpsk = oqpsk;
            std::vector<std::vector<std::uint8_t>> got;
            LrptEvents ev;
            ev.on_packet = [&](std::uint8_t, const SpacePacket& p) { got.push_back(p.octets); };
            auto d2 = LrptDecoder::create(cfg, std::move(ev)).value();
            for (std::size_t i = 0; i < s.size(); i += 16384) {
                d2.push(std::span(s).subspan(i, std::min<std::size_t>(16384, s.size() - i)));
            }
            d2.finish();
            std::size_t m = 0;
            for (const auto& p : a) {
                m += std::find(got.begin(), got.end(), p) != got.end() ? 1 : 0;
            }
            std::printf("LRPT %s Eb/N0 %.1f dB, 2.5 kHz at -50 Hz/s: %zu frames, %zu failed, %zu RS symbols "
                        "corrected, %zu/%zu packets\n",
                        oqpsk ? "OQPSK" : "QPSK", eb_n0, d2.stats().frames, d2.stats().frames_failed,
                        d2.stats().rs_corrected, m, a.size());
            // Measured: QPSK clean from 4 dB; OQPSK, whose rails the
            // demodulator re-times after the carrier loop, from 6 dB.
            if (eb_n0 >= (oqpsk ? 6.0 : 4.0)) {
                CHECK(m == a.size());
            }
        }
    }
}
