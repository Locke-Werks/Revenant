// DRM30: the structure and coding against ES 201 980's own tables, and the
// receiver against the transmitter in core/dsp/synth/drm_mod.h.
//
// The first group regenerates tables the document prints (cell counts,
// SDC data field lengths, Annex J bit counts, the PRBS of table 26) from the
// rules the implementation carries. The second is the round trip.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "core/decode/drm.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/drm_mod.h"

using namespace revenant;
using decode::DrmMode;
using decode::DrmQam;

TEST_CASE("DRM table 26: the first sixteen PRBS bits", "[drm]") {
    const std::vector<std::uint8_t> expect = {0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0};
    CHECK(decode::drm_prbs(16) == expect);
}

TEST_CASE("DRM cell counts regenerate tables 25 and 41 to 44", "[drm]") {
    struct Row {
        DrmMode mode;
        int so;
        std::size_t n_sdc, n_sfa, n_mux;
    };
    const Row rows[] = {
        {DrmMode::A, 0, 167, 3778, 1259}, {DrmMode::A, 1, 190, 4268, 1422},
        {DrmMode::A, 2, 359, 7897, 2632}, {DrmMode::A, 3, 405, 8877, 2959},
        {DrmMode::A, 4, 754, 16394, 5464}, {DrmMode::A, 5, 846, 18354, 6118},
        {DrmMode::B, 0, 130, 2900, 966},  {DrmMode::B, 1, 150, 3330, 1110},
        {DrmMode::B, 2, 282, 6153, 2051}, {DrmMode::B, 3, 322, 7013, 2337},
        {DrmMode::B, 4, 588, 12747, 4249}, {DrmMode::B, 5, 662, 14323, 4774},
        {DrmMode::C, 3, 288, 5532, 1844}, {DrmMode::C, 5, 607, 11603, 3867},
        {DrmMode::D, 3, 152, 3679, 1226}, {DrmMode::D, 5, 332, 7819, 2606},
    };
    for (const Row& r : rows) {
        auto st = decode::drm_structure(r.mode, r.so);
        REQUIRE(st.has_value());
        INFO("mode " << decode::drm_mode_letter(r.mode) << " occupancy " << r.so);
        CHECK(st->sdc.size() == r.n_sdc);
        CHECK(st->msc.size() == r.n_sfa);
        CHECK(st->n_mux() == r.n_mux);
        for (const auto& fac : st->fac) {
            CHECK(fac.size() == 65);  // ES 7.2.1.2
        }
    }
    CHECK_FALSE(decode::drm_structure(DrmMode::C, 0).has_value());
}

TEST_CASE("DRM SDC data field lengths regenerate table 21", "[drm]") {
    struct Row {
        DrmMode mode;
        int so;
        std::size_t mode0, mode1;
    };
    const Row rows[] = {
        {DrmMode::A, 0, 37, 17},  {DrmMode::A, 1, 43, 20},  {DrmMode::A, 2, 85, 41},
        {DrmMode::A, 3, 97, 47},  {DrmMode::A, 4, 184, 91}, {DrmMode::A, 5, 207, 102},
        {DrmMode::B, 0, 28, 13},  {DrmMode::B, 1, 33, 15},  {DrmMode::B, 2, 66, 32},
        {DrmMode::B, 3, 76, 37},  {DrmMode::B, 4, 143, 70}, {DrmMode::B, 5, 161, 79},
        {DrmMode::C, 3, 68, 32},  {DrmMode::C, 5, 147, 72}, {DrmMode::D, 3, 33, 15},
        {DrmMode::D, 5, 78, 38},
    };
    for (const Row& r : rows) {
        auto st = decode::drm_structure(r.mode, r.so);
        REQUIRE(st.has_value());
        auto p0 = decode::drm_sdc_plan(st->sdc.size(), 0);
        auto p1 = decode::drm_sdc_plan(st->sdc.size(), 1);
        REQUIRE(p0.has_value());
        REQUIRE(p1.has_value());
        INFO("mode " << decode::drm_mode_letter(r.mode) << " occupancy " << r.so);
        CHECK(decode::drm_sdc_data_bytes(p0->l2) == r.mode0);
        CHECK(decode::drm_sdc_data_bytes(p1->l2) == r.mode1);
    }
}

TEST_CASE("DRM EEP multiplex sizes regenerate Annex J", "[drm]") {
    // Tables J.1 and J.2: 64-QAM at protection levels 0 to 3, then 16-QAM at
    // 0 and 1, per spectrum occupancy 0 to 5.
    const std::size_t a[6][6] = {{3757, 4248, 7878, 8857, 16374, 18336},
                                 {4509, 5096, 9450, 10628, 19646, 21998},
                                 {5322, 6018, 11157, 12547, 23193, 25976},
                                 {5898, 6664, 12364, 13908, 25704, 28788},
                                 {2505, 2832, 5250, 5904, 10914, 12222},
                                 {3131, 3540, 6565, 7381, 13645, 15280}};
    const std::size_t b[6][6] = {{2880, 3312, 6133, 6991, 12727, 14304},
                                 {3456, 3972, 7361, 8390, 15272, 17162},
                                 {4080, 4692, 8688, 9900, 18026, 20264},
                                 {4520, 5196, 9630, 10980, 19980, 22456},
                                 {1920, 2208, 4089, 4662, 8484, 9534},
                                 {2400, 2760, 5111, 5826, 10606, 11920}};
    for (int mode = 0; mode < 2; ++mode) {
        for (int so = 0; so < 6; ++so) {
            auto st = decode::drm_structure(static_cast<DrmMode>(mode), so);
            REQUIRE(st.has_value());
            for (int row = 0; row < 6; ++row) {
                const DrmQam qam = row < 4 ? DrmQam::Qam64 : DrmQam::Qam16;
                const int level = row < 4 ? row : row - 4;
                auto plan = decode::drm_msc_plan(qam, 0, level, 0, st->n_mux());
                REQUIRE(plan.has_value());
                INFO("mode " << mode << " so " << so << " row " << row);
                CHECK(plan->l1 + plan->l2 == (mode == 0 ? a[row][so] : b[row][so]));
            }
        }
    }
    // Table J.3, mode C, 64-QAM at Rall 0,5.
    auto c3 = decode::drm_structure(DrmMode::C, 3);
    auto c5 = decode::drm_structure(DrmMode::C, 5);
    CHECK(decode::drm_msc_plan(DrmQam::Qam64, 0, 0, 0, c3->n_mux())->l2 == 5514);
    CHECK(decode::drm_msc_plan(DrmQam::Qam64, 0, 0, 0, c5->n_mux())->l2 == 11581);
}

TEST_CASE("DRM mixed-radix FFT against a direct DFT", "[drm]") {
    for (std::size_t n : {std::size_t{1152}, std::size_t{704}, std::size_t{448}, std::size_t{1024}}) {
        std::mt19937 rng(static_cast<unsigned>(n));
        std::normal_distribution<float> g;
        std::vector<dsp::Complex32> x(n);
        for (auto& v : x) {
            v = {g(rng), g(rng)};
        }
        auto y = x;
        decode::DrmFft(n).transform(y, false);
        double worst = 0.0;
        for (std::size_t k = 0; k < n; k += 37) {
            std::complex<double> acc{};
            for (std::size_t i = 0; i < n; ++i) {
                const double a = -2.0 * std::numbers::pi * static_cast<double>(k * i % n) / static_cast<double>(n);
                acc += std::complex<double>(x[i]) * std::complex<double>(std::cos(a), std::sin(a));
            }
            worst = std::max(worst, std::abs(acc - std::complex<double>(y[k])));
        }
        INFO("n " << n << " worst error " << worst);
        CHECK(worst < 1e-2);
        decode::DrmFft(n).transform(y, true);
        CHECK(std::abs(y[5] / static_cast<float>(n) - x[5]) < 1e-3F);
    }
}

TEST_CASE("DRM interleavers are permutations", "[drm]") {
    for (std::size_t xin : {std::size_t{130}, std::size_t{810}, std::size_t{2337}, std::size_t{5918}}) {
        for (unsigned t : {5U, 13U, 21U}) {
            auto p = decode::drm_interleaver(xin, t);
            std::vector<std::uint8_t> seen(xin, 0);
            for (auto v : p) {
                REQUIRE(v < xin);
                seen[v]++;
            }
            CHECK(std::all_of(seen.begin(), seen.end(), [](std::uint8_t c) { return c == 1; }));
        }
    }
}

TEST_CASE("DRM multilevel coding round trips without noise", "[drm]") {
    std::mt19937 rng(11);
    auto check = [&](const decode::DrmCodingPlan& plan) {
        std::vector<std::uint8_t> bits(plan.l1 + plan.l2);
        for (auto& b : bits) {
            b = static_cast<std::uint8_t>(rng() & 1U);
        }
        auto cells = decode::drm_mlc_encode(plan, bits);
        REQUIRE(cells.has_value());
        REQUIRE(cells->size() == plan.n1 + plan.n2);
        std::vector<float> w(cells->size(), 1.0F);
        auto back = decode::drm_mlc_decode(plan, *cells, w);
        REQUIRE(back.has_value());
        CHECK(*back == bits);
    };
    check(decode::drm_fac_plan());
    check(*decode::drm_sdc_plan(322, 0));
    check(*decode::drm_sdc_plan(322, 1));
    check(*decode::drm_msc_plan(DrmQam::Qam64, 1, 2, 120, 2337));
    check(*decode::drm_msc_plan(DrmQam::Qam16, 0, 1, 40, 1844));
    check(*decode::drm_msc_plan(DrmQam::Qam64, 0, 3, 0, 1226));
}

TEST_CASE("DRM FAC bits and CRC", "[drm]") {
    decode::DrmFacChannel c;
    c.identity = 2;
    c.occupancy = 3;
    c.msc_mode = 3;
    c.sdc_mode = 1;
    c.services = 4;
    decode::DrmFacService s;
    s.service_id = 0xABCDEF;
    s.language = 5;
    s.descriptor = 10;
    auto bits = decode::drm_fac_bits(c, s);
    REQUIRE(bits.size() == 72);
    auto f = decode::drm_parse_fac(bits);
    CHECK(f.crc_ok);
    CHECK(f.channel.identity == 2);
    CHECK(f.channel.occupancy == 3);
    CHECK(f.service.service_id == 0xABCDEF);
    bits[10] ^= 1;
    CHECK_FALSE(decode::drm_parse_fac(bits).crc_ok);
}

TEST_CASE("DRM text message segments", "[drm]") {
    const std::string text = "Revenant DRM30 text message, two segments plus";
    decode::DrmTextAssembler asm_;
    std::optional<decode::DrmTextMessage> got;
    for (const auto& piece : decode::drm_text_pieces(text, true)) {
        if (auto m = asm_.push(0, piece)) {
            got = m;
        }
    }
    REQUIRE(got.has_value());
    CHECK(got->text == text);
}

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

namespace {

struct Scenario {
    siggen::DrmTxConfig tx;
    std::size_t super_frames = 6;
};

struct Sent {
    std::vector<siggen::Complex32> iq;
    std::map<std::uint32_t, std::vector<std::uint8_t>> stream0;  // by counter
    std::size_t data_units = 0;
};

const std::string kText = "DRM text: Revenant round trip";

Sent transmit(const Scenario& sc) {
    auto mod = siggen::DrmModulator::create(sc.tx);
    INFO((mod.has_value() ? std::string{} : mod.error().message));
    REQUIRE(mod.has_value());
    std::vector<std::uint8_t> sdc;
    auto add = [&](const std::vector<std::uint8_t>& e) { sdc.insert(sdc.end(), e.begin(), e.end()); };
    add(siggen::drm_sdc_multiplex(sc.tx.protection_a, sc.tx.protection_b, sc.tx.streams));
    add(siggen::drm_sdc_label(0, "Revenant"));
    add(siggen::drm_sdc_audio(0, 0, true));
    if (sc.tx.streams.size() > 1) {
        add(siggen::drm_sdc_packet_app(1, 1, 2, true, 20));
    }
    add(siggen::drm_sdc_time(61000, 13, 37));
    REQUIRE(sdc.size() <= mod->sdc_data_bytes());

    Sent sent;
    const auto pieces = decode::drm_text_pieces(kText, false);
    std::size_t piece = 0;
    std::uint32_t counter = 0;
    std::uint8_t ci = 0;
    for (std::size_t sf = 0; sf < sc.super_frames; ++sf) {
        siggen::DrmTxSuperFrame f;
        f.sdc = sdc;
        for (std::size_t m = 0; m < 3; ++m) {
            const auto& d0 = sc.tx.streams[0];
            std::vector<std::uint8_t> s0(d0.part_a + d0.part_b);
            for (std::size_t i = 0; i < s0.size(); ++i) {
                s0[i] = static_cast<std::uint8_t>((i * 31 + counter * 7) & 0xFF);
            }
            s0[0] = static_cast<std::uint8_t>(counter >> 8U);
            s0[1] = static_cast<std::uint8_t>(counter & 0xFFU);
            const auto& p = pieces[piece++ % pieces.size()];
            std::copy(p.begin(), p.end(), s0.end() - 4);
            sent.stream0[counter] = s0;
            f.logical[m].push_back(s0);
            if (sc.tx.streams.size() > 1) {
                const auto& d1 = sc.tx.streams[1];
                std::vector<std::uint8_t> s1;
                const std::size_t packets = (d1.part_a + d1.part_b) / 23;
                for (std::size_t k = 0; k < packets; ++k) {
                    std::vector<std::uint8_t> useful(k == packets - 1 ? 7 : 20, static_cast<std::uint8_t>(counter + k));
                    auto pk = decode::drm_packet(k == 0, k == packets - 1, 2, ci++, useful, 20);
                    s1.insert(s1.end(), pk.begin(), pk.end());
                }
                ++sent.data_units;
                f.logical[m].push_back(s1);
            }
            ++counter;
        }
        REQUIRE(mod->super_frame(f, sent.iq).has_value());
    }
    // Lead-in noise floor so acquisition does not start on sample 0.
    std::vector<siggen::Complex32> lead(4000, siggen::Complex32{});
    sent.iq.insert(sent.iq.begin(), lead.begin(), lead.end());
    return sent;
}

struct Received {
    decode::DrmOutput out;
    decode::DrmStats stats;
    decode::DrmSyncInfo sync;
};

Received receive(std::span<const siggen::Complex32> iq, std::size_t block) {
    auto rx = decode::DrmReceiver::create({});
    REQUIRE(rx.has_value());
    Received r;
    for (std::size_t i = 0; i < iq.size(); i += block) {
        rx->process(iq.subspan(i, std::min(block, iq.size() - i)), r.out);
    }
    r.stats = rx->stats();
    r.sync = rx->sync();
    return r;
}

std::size_t matching_frames(const Sent& sent, const Received& r) {
    std::size_t good = 0;
    for (const auto& f : r.out.streams) {
        if (f.stream != 0 || f.bytes.size() < 2) {
            continue;
        }
        const std::uint32_t counter = (static_cast<std::uint32_t>(f.bytes[0]) << 8U) | f.bytes[1];
        auto it = sent.stream0.find(counter);
        if (it != sent.stream0.end() && it->second == f.bytes) {
            ++good;
        }
    }
    return good;
}

void report(const char* name, const Sent& sent, const Received& r) {
    const std::size_t good = matching_frames(sent, r);
    WARN(name << ": mode " << decode::drm_mode_letter(r.sync.mode) << ", FAC ok " << r.stats.fac_ok
              << " bad " << r.stats.fac_bad << ", SDC ok " << r.stats.sdc_ok << " bad "
              << r.stats.sdc_bad << ", multiplex frames " << r.stats.multiplex_frames
              << ", stream frames intact " << good << "/" << sent.stream0.size() << ", texts "
              << r.out.texts.size() << ", data units " << r.out.data_units.size() << ", FAC MER "
              << r.stats.last_mer_db << " dB");
}

Scenario scenario(DrmMode mode, int so, DrmQam qam, int pb, bool long_il, int sdc_mode) {
    Scenario sc;
    sc.tx.mode = mode;
    sc.tx.occupancy = so;
    sc.tx.msc = qam;
    sc.tx.protection_b = pb;
    sc.tx.long_interleave = long_il;
    sc.tx.sdc_mode = sdc_mode;
    sc.tx.service.service_id = 0x1234AB;
    auto st = decode::drm_structure(mode, so);
    auto plan = decode::drm_msc_plan(qam, 0, pb, 0, st->n_mux());
    const std::size_t bytes = (plan->l1 + plan->l2) / 8;
    const std::size_t data = 23 * 3;  // three packets of 20 data bytes
    sc.tx.streams.push_back({0, static_cast<std::uint16_t>(bytes - data - 3)});
    sc.tx.streams.push_back({0, static_cast<std::uint16_t>(data)});
    if (long_il) {
        sc.super_frames = 8;
    }
    return sc;
}

}  // namespace

TEST_CASE("DRM round trip, clean, every mode", "[drm]") {
    const Scenario scs[] = {
        scenario(DrmMode::B, 3, DrmQam::Qam64, 1, false, 1),
        scenario(DrmMode::A, 3, DrmQam::Qam16, 1, false, 0),
        scenario(DrmMode::C, 3, DrmQam::Qam64, 0, false, 1),
        scenario(DrmMode::D, 3, DrmQam::Qam16, 0, false, 0),
    };
    for (const Scenario& sc : scs) {
        const Sent sent = transmit(sc);
        const Received r = receive(sent.iq, 4800);
        report("clean", sent, r);
        CHECK(r.sync.mode == sc.tx.mode);
        REQUIRE_FALSE(r.out.fac.empty());
        CHECK(r.out.fac.front().service.service_id == 0x1234AB);
        REQUIRE_FALSE(r.out.sdc.empty());
        const auto& sdc = r.out.sdc.back();
        CHECK(sdc.crc_ok);
        REQUIRE(sdc.labels.size() == 1);
        CHECK(sdc.labels[0].second == "Revenant");
        CHECK(sdc.mjd == std::optional<std::uint32_t>{61000});
        CHECK(sdc.utc_hours == std::optional<std::uint8_t>{13});
        CHECK(sdc.utc_minutes == std::optional<std::uint8_t>{37});
        // All but the super frames spent acquiring.
        CHECK(matching_frames(sent, r) + 9 >= sent.stream0.size());
        CHECK(r.stats.multiplex_frames == matching_frames(sent, r));
        REQUIRE_FALSE(r.out.texts.empty());
        CHECK(r.out.texts.front().text == kText);
        CHECK(r.out.data_units.size() + 9 >= sent.data_units);
    }
}

TEST_CASE("DRM round trip with long interleaving", "[drm]") {
    const Scenario sc = scenario(DrmMode::B, 3, DrmQam::Qam64, 1, true, 1);
    const Sent sent = transmit(sc);
    const Received r = receive(sent.iq, 4800);
    report("long interleave", sent, r);
    CHECK(matching_frames(sent, r) + 14 >= sent.stream0.size());
    CHECK(matching_frames(sent, r) > 0);
}

TEST_CASE("DRM chunk independence", "[drm]") {
    const Scenario sc = scenario(DrmMode::B, 3, DrmQam::Qam16, 0, false, 1);
    const Sent sent = transmit(sc);
    const Received whole = receive(sent.iq, sent.iq.size());
    for (std::size_t block : {std::size_t{97}, std::size_t{1000}, std::size_t{48000}}) {
        const Received split = receive(sent.iq, block);
        REQUIRE(split.out.streams.size() == whole.out.streams.size());
        for (std::size_t i = 0; i < whole.out.streams.size(); ++i) {
            CHECK(split.out.streams[i].bytes == whole.out.streams[i].bytes);
        }
        CHECK(split.out.fac.size() == whole.out.fac.size());
        CHECK(split.sync.frame_start_sample == whole.sync.frame_start_sample);
    }
}

TEST_CASE("DRM round trip through AWGN and a two-path channel", "[drm]") {
    const Scenario sc16 = scenario(DrmMode::B, 3, DrmQam::Qam16, 1, false, 1);
    const Sent sent16 = transmit(sc16);
    for (double snr : {20.0, 15.0, 12.0}) {
        siggen::ChannelConfig cfg;
        cfg.noise = siggen::NoiseLevel::snr_in_reference_bandwidth_db(snr, 10000);
        cfg.frequency.doppler_shift_hz = 37;
        auto ch = siggen::apply_channel(sent16.iq, cfg, decode::kDrmSampleRate, 0xD8A0ULL + static_cast<std::uint64_t>(snr));
        REQUIRE(ch.has_value());
        const Received r = receive(ch->samples, 4800);
        const std::string name = "16-QAM AWGN " + std::to_string(static_cast<int>(snr)) + " dB in 10 kHz, +37 Hz";
        report(name.c_str(), sent16, r);
        if (snr >= 15.0) {
            CHECK(matching_frames(sent16, r) + 9 >= sent16.stream0.size());
        }
    }

    const Scenario sc64 = scenario(DrmMode::B, 3, DrmQam::Qam64, 1, false, 1);
    const Sent sent64 = transmit(sc64);
    for (double snr : {25.0, 20.0}) {
        siggen::ChannelConfig cfg;
        cfg.noise = siggen::NoiseLevel::snr_in_reference_bandwidth_db(snr, 10000);
        auto ch = siggen::apply_channel(sent64.iq, cfg, decode::kDrmSampleRate, 0xD8A1ULL + static_cast<std::uint64_t>(snr));
        REQUIRE(ch.has_value());
        const Received r = receive(ch->samples, 4800);
        const std::string name = "64-QAM AWGN " + std::to_string(static_cast<int>(snr)) + " dB in 10 kHz";
        report(name.c_str(), sent64, r);
        if (snr >= 25.0) {
            CHECK(matching_frames(sent64, r) + 9 >= sent64.stream0.size());
        }
    }

    // Two paths 1 ms apart, the second 6 dB down: inside mode B's 5.33 ms
    // guard interval, and a deep notch every 1 kHz across the channel.
    siggen::ChannelConfig cfg;
    cfg.multipath.taps = {{0.0, 0.0, 0.0}, {1e-3, -6.0, 2.0}};
    cfg.noise = siggen::NoiseLevel::snr_in_reference_bandwidth_db(25.0, 10000);
    auto ch = siggen::apply_channel(sent16.iq, cfg, decode::kDrmSampleRate, 0xD8A2ULL);
    REQUIRE(ch.has_value());
    const Received r = receive(ch->samples, 4800);
    report("16-QAM two-path 1 ms at -6 dB, 25 dB", sent16, r);
    CHECK(matching_frames(sent16, r) + 9 >= sent16.stream0.size());
}

TEST_CASE("DRM noise alone reports nothing", "[drm]") {
    std::vector<siggen::Complex32> noise(decode::kDrmSampleRate * 6, siggen::Complex32{});
    REQUIRE(siggen::add_awgn_at_power(noise, 1.0, 0xD8A3ULL).has_value());
    const Received r = receive(noise, 4800);
    CHECK(r.out.fac.empty());
    CHECK(r.out.sdc.empty());
    CHECK(r.out.streams.empty());
}
