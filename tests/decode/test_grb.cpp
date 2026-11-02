// GOES-R Rebroadcast, core/decode/grb.h and core/decode/dvbs2.h, against
// core/dsp/synth/grb_mod.h: the DVB-S2 pieces on their own, then QPSK 9/10
// and 8PSK 2/3 round trips through carrier offset, phase, timing, noise and
// arbitrary blocking. Synthetic only; see grb.h on why.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <vector>

#include "core/decode/dvbs2.h"
#include "core/decode/grb.h"
#include "core/dsp/synth/ccsds_mod.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/grb_mod.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

std::vector<std::vector<std::uint8_t>> packets(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::vector<std::uint8_t>> out;
    for (std::size_t i = 0; i < n; ++i) {
        SpacePacketHeader h;
        h.apid = static_cast<std::uint16_t>(0x100 + i % 3);
        h.secondary_header = true;
        h.sequence_count = static_cast<std::uint16_t>(i);
        std::vector<std::uint8_t> d(500 + rng() % 4000);
        for (auto& o : d) {
            o = static_cast<std::uint8_t>(rng());
        }
        out.push_back(siggen::ccsds_space_packet(h, d));
    }
    return out;
}

struct Got {
    std::vector<std::vector<std::uint8_t>> octets;
    GrbStats stats;
};

Got run(const GrbConfig& cfg, const std::vector<dsp::Complex32>& s, std::size_t chunk) {
    Got g;
    GrbEvents ev;
    ev.on_packet = [&](std::uint8_t, const SpacePacket& p) { g.octets.push_back(p.octets); };
    auto d = GrbDecoder::create(cfg, std::move(ev)).value();
    for (std::size_t i = 0; i < s.size(); i += chunk) {
        d.push(std::span(s).subspan(i, std::min(chunk, s.size() - i)));
    }
    d.finish();
    g.stats = d.stats();
    return g;
}

double es_n0_rate(GrbModulation m) {
    return m == GrbModulation::Psk8Rate23 ? kGrb8pskSymbolRate : kGrbQpskSymbolRate;
}

}  // namespace

TEST_CASE("DVB-S2 PLHEADER and BBHEADER", "[decode][grb]") {
    // Table 12 by the PUG's values.
    CHECK(kDvbs2ModcodQpsk910 == 0x0B);
    CHECK(kDvbs2Modcod8psk23 == 0x0D);
    // Table 11 and 5.5.1.
    CHECK(dvbs2_plframe_symbols(kDvbs2ModcodQpsk910, 0) == 90 * 361);
    CHECK(dvbs2_plframe_symbols(kDvbs2Modcod8psk23, 0) == 90 * 241);
    CHECK(dvbs2_plframe_symbols(kDvbs2ModcodQpsk910, 1) == 90 * 361 + 36 * 22);
    CHECK(dvbs2_plframe_symbols(kDvbs2ModcodDummy, 0) == 90 * 37);

    // Every PLS code word decodes to itself, at any rotation.
    for (unsigned w = 0; w < 128; ++w) {
        const auto idx = dvbs2_plheader(static_cast<std::uint8_t>(w >> 2U), static_cast<std::uint8_t>(w & 3U));
        std::vector<dsp::Complex32> s;
        const dsp::Complex32 rot = std::polar(1.0F, 0.7F + static_cast<float>(w));
        for (const auto k : idx) {
            s.push_back(psk_point(PskModulation::Psk8, k) * rot);
        }
        const auto h = dvbs2_decode_plheader(s);
        REQUIRE(h.modcod == (w >> 2U));
        REQUIRE(h.type == (w & 3U));
        CHECK(h.metric > 0.99F);
        CHECK(std::abs(h.phase - rot) < 1e-4F);
    }

    // The CRC-8 of a header written is checked on parsing.
    Dvbs2BbHeader h;
    h.matype1 = kGrbMatype1;
    h.dfl = 58112;
    std::vector<std::uint8_t> bits(80);
    dvbs2_write_bbheader(h, bits);
    REQUIRE(dvbs2_parse_bbheader(bits).has_value());
    CHECK(dvbs2_parse_bbheader(bits)->dfl == 58112);
    bits[17] ^= 1U;
    CHECK_FALSE(dvbs2_parse_bbheader(bits).has_value());

    // 5.5.4: R values are 0 to 3 and depend on the Gold code number.
    const auto r0 = dvbs2_pl_scrambling(0, 1000);
    const auto r1 = dvbs2_pl_scrambling(1, 1000);
    CHECK(std::all_of(r0.begin(), r0.end(), [](std::uint8_t v) { return v < 4; }));
    CHECK(r0 != r1);
}

TEST_CASE("DVB-S2 BCH and LDPC correct noise at both GRB rates", "[decode][grb]") {
    for (const std::uint8_t modcod : {kDvbs2ModcodQpsk910, kDvbs2Modcod8psk23}) {
        auto fec = Dvbs2Fec::create(modcod).value();
        const auto& c = fec.code();
        std::mt19937_64 rng(modcod);
        std::vector<std::uint8_t> bb(c.kbch);
        for (auto& b : bb) {
            b = static_cast<std::uint8_t>(rng() & 1U);
        }
        std::vector<std::uint8_t> cw(kDvbs2NormalFrameBits);
        fec.encode(bb, cw);
        std::vector<float> llr(kDvbs2NormalFrameBits);
        // BPSK with margin over where each code turns over: Eb/N0 3.8 dB at
        // 9/10 and 3.2 dB at 2/3.
        const float sigma = modcod == kDvbs2ModcodQpsk910 ? 0.48F : 0.6F;
        std::normal_distribution<float> noise(0.0F, sigma);
        for (std::size_t i = 0; i < cw.size(); ++i) {
            const float y = (cw[i] != 0U ? -1.0F : 1.0F) + noise(rng);
            llr[i] = 2.0F * y / (sigma * sigma);
        }
        std::vector<std::uint8_t> out(c.kbch);
        const auto r = fec.decode(llr, out);
        INFO("modcod " << int(modcod) << " iterations " << r.ldpc_iterations);
        CHECK(r.ok);
        CHECK(r.ldpc_converged);
        CHECK(out == bb);

        // Saturated hard decisions of the clean word.
        std::vector<float> hard(kDvbs2NormalFrameBits);
        for (std::size_t i = 0; i < cw.size(); ++i) {
            hard[i] = cw[i] != 0U ? -1.0F : 1.0F;
        }
        CHECK(fec.decode(hard, out).ok);
    }
}

TEST_CASE("GRB packets round trip at every carrier phase", "[decode][grb]") {
    const auto pk = packets(20, 1);
    for (const auto m : {GrbModulation::Qpsk910, GrbModulation::Psk8Rate23}) {
        for (const double phase : {0.3, 0.3 + std::numbers::pi / 2, 0.3 + std::numbers::pi, 2.0}) {
            siggen::GrbTxSpec tx;
            tx.modulation = m;
            tx.initial_phase = phase;
            tx.carrier_offset_hz = 150000.0;
            tx.timing_offset_samples = 0.4;
            const auto s = siggen::grb_baseband(tx, pk).value();
            GrbConfig cfg;
            cfg.modulation = m;
            const auto g = run(cfg, s, 1 << 16);
            INFO("modulation " << int(m) << " phase " << phase << " plframes " << g.stats.plframes << " fec failures "
                               << g.stats.fec_failures << " frames " << g.stats.frames);
            CHECK(g.octets == pk);
            CHECK(g.stats.fec_failures == 0);
        }
    }
}

TEST_CASE("GRB with pilots and a non-default Gold code", "[decode][grb]") {
    const auto pk = packets(10, 2);
    siggen::GrbTxSpec tx;
    tx.pilots = true;
    tx.gold_code = 1234;
    tx.initial_phase = 1.1;
    const auto s = siggen::grb_baseband(tx, pk).value();
    GrbConfig cfg;
    cfg.gold_code = 1234;
    CHECK(run(cfg, s, 1 << 16).octets == pk);
    cfg.gold_code = 0;
    CHECK(run(cfg, s, 1 << 16).octets.empty());
}

TEST_CASE("GRB decodes the same in any blocking", "[decode][grb]") {
    const auto pk = packets(12, 3);
    siggen::GrbTxSpec tx;
    tx.initial_phase = 0.9;
    tx.carrier_offset_hz = -40000.0;
    auto s = siggen::grb_baseband(tx, pk).value();
    REQUIRE(siggen::add_awgn(s, siggen::NoiseLevel::eb_over_n0_db(9.0, kGrbQpskSymbolRate), 20000000, 7)
                .has_value());
    GrbConfig cfg;
    const auto a = run(cfg, s, s.size());
    const auto b = run(cfg, s, 100003);
    const auto c = run(cfg, s, 4099);
    CHECK(a.octets == pk);
    CHECK(a.octets == b.octets);
    CHECK(a.octets == c.octets);
}

TEST_CASE("GRB with offset, clock error and noise", "[decode][grb]") {
    const auto pk = packets(16, 4);
    for (const auto m : {GrbModulation::Qpsk910, GrbModulation::Psk8Rate23}) {
        for (const double es_n0 : {6.0, 7.0, 8.0, 9.0, 10.0}) {
            siggen::GrbTxSpec tx;
            tx.modulation = m;
            tx.initial_phase = 2.5;
            tx.clock_error = 10e-6;
            // 8PSK is held to 150 kHz: see the note in core/decode/grb.h.
            tx.carrier_offset_hz = m == GrbModulation::Qpsk910 ? 250000.0 : 150000.0;
            auto s = siggen::grb_baseband(tx, pk).value();
            REQUIRE(siggen::add_awgn(s, siggen::NoiseLevel::eb_over_n0_db(es_n0, es_n0_rate(m)), 20000000,
                                     static_cast<std::uint64_t>(es_n0 * 10))
                        .has_value());
            GrbConfig cfg;
            cfg.modulation = m;
            const auto g = run(cfg, s, 1 << 16);
            std::size_t found = 0;
            for (const auto& p : pk) {
                found += std::find(g.octets.begin(), g.octets.end(), p) != g.octets.end() ? 1 : 0;
            }
            std::printf("GRB %s Es/N0 %.1f dB, 10 ppm: %zu PLFRAMEs, %zu FEC failures, mean %.1f LDPC "
                        "iterations, %zu frames, %zu/%zu packets\n",
                        m == GrbModulation::Qpsk910 ? "QPSK 9/10" : "8PSK 2/3", es_n0, g.stats.plframes,
                        g.stats.fec_failures,
                        g.stats.plframes > g.stats.dummy_plframes
                            ? double(g.stats.ldpc_iterations) / double(g.stats.plframes - g.stats.dummy_plframes)
                            : 0.0,
                        g.stats.frames, found, pk.size());
            // The FEC alone, fed ideal symbols, decodes QPSK 9/10 and 8PSK
            // 2/3 at 7 dB; through the demodulator 8PSK needs more.
            if (es_n0 >= (m == GrbModulation::Qpsk910 ? 8.0 : 10.0)) {
                CHECK(found == pk.size());
            }
        }
    }
}
