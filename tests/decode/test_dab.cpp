// DAB, ETSI EN 300 401 V2.1.1: the standard's printed sequences, then the FIC,
// packet mode and MOT layers on their own, then whole ensembles through the
// air interface against core/dsp/synth/dab_mod.h with noise, multipath and a
// carrier offset, and the same ensemble decoded in several blockings.
//
// Every figure the air cases measure is printed in an INFO or WARN line and
// the assertion around it is loose, as in the other decoder suites.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <map>
#include <numbers>
#include <string>
#include <vector>

#include "core/decode/dab.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dab_mod.h"

using namespace revenant;
using decode::DabProtection;
using decode::DabSubchannel;

namespace {

std::vector<std::uint8_t> pseudo_bytes(std::size_t n, std::uint64_t seed) {
    std::vector<std::uint8_t> out(n);
    std::uint64_t state = seed;
    for (auto& b : out) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        b = static_cast<std::uint8_t>(state >> 45U);
    }
    return out;
}

// The test ensemble: a stream data sub-channel at 16 kbit/s, 3-A, and a
// packet mode sub-channel at 32 kbit/s, 2-A, carrying one MOT object.
constexpr unsigned kStreamId = 1;
constexpr unsigned kPacketId = 2;
constexpr unsigned kAddress = 100;
constexpr std::uint16_t kEid = 0x4FFF;
constexpr std::uint32_t kAudioSid = 0x4C0D;
constexpr std::uint32_t kDataSid = 0xE04C0FEE;

std::vector<DabSubchannel> test_subchannels() {
    DabSubchannel stream;
    stream.id = kStreamId;
    stream.start_cu = 0;
    stream.size_cu = 12;  // table 9: 6n CUs at 3-A, n = 2
    stream.protection = DabProtection::EepA;
    stream.level = 3;
    DabSubchannel packet;
    packet.id = kPacketId;
    packet.start_cu = 40;
    packet.size_cu = 32;  // 8n CUs at 2-A, n = 4
    packet.protection = DabProtection::EepA;
    packet.level = 2;
    return {stream, packet};
}

struct Ensemble {
    std::vector<dsp::Complex32> samples;
    std::vector<std::vector<std::uint8_t>> stream_frames;  // per CIF
    std::vector<std::uint8_t> mot_body;
};

Ensemble build_ensemble(std::size_t frames) {
    auto tx = siggen::DabTransmitter::create(test_subchannels());
    REQUIRE(tx.has_value());
    Ensemble e;

    const auto subchannels = test_subchannels();
    decode::DabService audio;
    audio.sid = kAudioSid;
    audio.components.push_back({1, 0, kStreamId, 0, true, false});
    decode::DabService data;
    data.sid = kDataSid;
    data.data_sid = true;
    data.components.push_back({3, 0, 0, 5, true, false});
    decode::DabPacketComponent pc;
    pc.scid = 5;
    pc.subchannel = kPacketId;
    pc.address = kAddress;
    pc.dscty = 60;  // any value: the receiver does not interpret it

    // The MOT object: a 700-byte body in 200-byte segments, the carousel
    // repeating header then body.
    e.mot_body = pseudo_bytes(700, 0xDAB0'0001);
    std::vector<std::vector<std::uint8_t>> carousel;
    unsigned dg_continuity = 0;
    const auto header = siggen::mot_header(e.mot_body.size(), 2, 1, "slide1.jpg");
    {
        siggen::DabDataGroupSpec spec{3, dg_continuity, 0, true, true, 0, true, 0x1234};
        carousel.push_back(siggen::dab_data_group(spec, siggen::mot_segment(header, 0)));
    }
    for (std::size_t s = 0; s * 200 < e.mot_body.size(); ++s) {
        const std::size_t n = std::min<std::size_t>(200, e.mot_body.size() - s * 200);
        const bool last = (s + 1) * 200 >= e.mot_body.size();
        siggen::DabDataGroupSpec spec{4, dg_continuity, 0, true, last,
                                          static_cast<unsigned>(s), true, 0x1234};
        const auto segment = std::span<const std::uint8_t>(e.mot_body).subspan(s * 200, n);
        carousel.push_back(siggen::dab_data_group(spec, siggen::mot_segment(segment, 0)));
    }
    std::vector<std::vector<std::uint8_t>> packets;
    unsigned packet_continuity = 0;
    std::size_t next_group = 0;

    const std::size_t stream_bytes = tx->logical_frame_bytes(kStreamId);
    const std::size_t packet_bytes = tx->logical_frame_bytes(kPacketId);
    REQUIRE(packet_bytes == 96);
    std::uint64_t seed = 0x5EED;
    unsigned cif_count = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        std::vector<std::array<std::uint8_t, 30>> fibs;
        std::map<unsigned, std::array<std::vector<std::uint8_t>, 4>> payload;
        for (std::size_t j = 0; j < 4; ++j, ++cif_count) {
            std::vector<std::vector<std::uint8_t>> fib0{
                siggen::dab_fig_ensemble(kEid, cif_count % 5000),
                siggen::dab_fig_subchannels(subchannels),
                siggen::dab_fig_service(audio)};
            std::vector<std::vector<std::uint8_t>> fib1{
                siggen::dab_fig_label(0, kEid, "REVENANT TEST")};
            std::vector<std::vector<std::uint8_t>> fib2;
            if (j % 2 == 0) {
                fib2.push_back(siggen::dab_fig_label(1, kAudioSid, "Stream Data"));
            } else {
                fib2.push_back(siggen::dab_fig_service(data));
                fib2.push_back(siggen::dab_fig_packet(pc));
            }
            for (const auto* figs : {&fib0, &fib1, &fib2}) {
                auto fib = siggen::dab_pack_fib(*figs);
                REQUIRE(fib.has_value());
                fibs.push_back(*fib);
            }
            auto bytes = pseudo_bytes(stream_bytes, ++seed);
            e.stream_frames.push_back(bytes);
            payload[kStreamId][j] = std::move(bytes);
            // One 96-byte packet per logical frame.
            if (packets.empty()) {
                packets = siggen::dab_packetize(carousel[next_group], kAddress, 96,
                                                    packet_continuity);
                next_group = (next_group + 1) % carousel.size();
            }
            payload[kPacketId][j] = packets.front();
            packets.erase(packets.begin());
        }
        auto samples = tx->frame(fibs, payload);
        INFO((samples.has_value() ? std::string{} : samples.error().message));
        REQUIRE(samples.has_value());
        e.samples.insert(e.samples.end(), samples->begin(), samples->end());
    }
    return e;
}

struct Impairments {
    double snr_db = 30.0;  // OFDM symbol power over noise in the full 2.048 MHz
    double cfo_hz = 0.0;
    bool multipath = false;
    std::size_t lead_in = 0;
    std::uint64_t seed = 1;
};

std::vector<dsp::Complex32> impair(std::vector<dsp::Complex32> clean, const Impairments& imp) {
    std::vector<dsp::Complex32> x(imp.lead_in, dsp::Complex32{0.0F, 0.0F});
    x.insert(x.end(), clean.begin(), clean.end());
    if (imp.multipath) {
        siggen::MultipathProfile profile;
        profile.taps = {{0.0, 0.0, 0.0}, {12e-6, -4.0, 1.0}, {40e-6, -10.0, 2.5}};
        auto out = siggen::apply_multipath(x, profile, decode::kDabSampleRate);
        REQUIRE(out.has_value());
        x = std::move(*out);
    }
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double phase = 2.0 * std::numbers::pi * imp.cfo_hz * static_cast<double>(n) /
                             static_cast<double>(decode::kDabSampleRate);
        x[n] *= dsp::Complex32(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
    }
    REQUIRE(siggen::add_awgn_at_power(x, std::pow(10.0, -imp.snr_db / 10.0), imp.seed).has_value());
    return x;
}

struct Decoded {
    std::vector<decode::DabLogicalFrame> frames;
    std::vector<decode::MotObject> objects;
    decode::DabEnsemble ensemble;
    decode::DabSync sync;
    double seconds = 0.0;
};

Decoded run(std::span<const dsp::Complex32> samples, std::size_t block) {
    Decoded d;
    decode::DabCallbacks cb;
    cb.on_frame = [&](const decode::DabLogicalFrame& f) { d.frames.push_back(f); };
    cb.on_mot_object = [&](const decode::MotObject& o) { d.objects.push_back(o); };
    auto rx = decode::DabReceiver::create({}, cb);
    REQUIRE(rx.has_value());
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t at = 0; at < samples.size(); at += block) {
        rx->push(samples.subspan(at, std::min(block, samples.size() - at)));
    }
    d.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    d.ensemble = rx->ensemble();
    d.sync = rx->sync();
    return d;
}

// Logical frames of the stream sub-channel that match what was sent. The
// receiver numbers CIFs from its lock, so the offset is found from the data.
std::size_t matching_stream_frames(const Decoded& d, const Ensemble& e, std::size_t& total) {
    total = 0;
    std::size_t best = 0;
    for (std::size_t offset = 0; offset < e.stream_frames.size(); ++offset) {
        std::size_t ok = 0;
        std::size_t seen = 0;
        for (const auto& f : d.frames) {
            if (f.subchannel != kStreamId) {
                continue;
            }
            ++seen;
            const std::size_t index = static_cast<std::size_t>(f.cif) + offset;
            if (index < e.stream_frames.size() && f.bytes == e.stream_frames[index]) {
                ++ok;
            }
        }
        total = seen;
        best = std::max(best, ok);
    }
    return best;
}

}  // namespace

// ---------------------------------------------------------------------------
// The document's printed values
// ---------------------------------------------------------------------------

TEST_CASE("the DAB PRBS starts as table 12 prints it", "[decode][dab]") {
    // EN 300 401 clause 10.1, table 12.
    const std::array<std::uint8_t, 16> table12{0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0};
    const auto prbs = decode::dab_prbs(16);
    CHECK(std::equal(prbs.begin(), prbs.end(), table12.begin()));
    // A maximal-length sequence of degree 9 repeats every 511 bits.
    const auto long_prbs = decode::dab_prbs(1022);
    CHECK(std::equal(long_prbs.begin(), long_prbs.begin() + 511, long_prbs.begin() + 511));
}

TEST_CASE("the DAB frequency interleaver reproduces table 25", "[decode][dab]") {
    const auto& f = decode::dab_frequency_interleaver();
    // EN 300 401 clause 14.6.1, table 25: n and k.
    const std::array<std::pair<std::size_t, int>, 16> table25{{{0, -513}, {1, -14}, {2, 329}, {3, 692},
                                                               {4, -733}, {5, 13}, {6, 680}, {7, 273},
                                                               {8, -36}, {9, 43}, {10, 85}, {11, -432},
                                                               {12, -318}, {1533, 652}, {1534, 606},
                                                               {1535, 197}}};
    for (const auto& [n, k] : table25) {
        INFO("n = " << n);
        CHECK(f[n] == k);
    }
    // A one-to-one map onto -768..768 without 0.
    std::vector<int> sorted(f.begin(), f.end());
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK(sorted.front() == -768);
    CHECK(sorted.back() == 768);
    CHECK(!std::binary_search(sorted.begin(), sorted.end(), 0));
}

TEST_CASE("DAB puncturing vectors have table 13's code rates", "[decode][dab]") {
    for (unsigned pi = 1; pi <= 24; ++pi) {
        const auto v = decode::dab_puncture_vector(pi);
        const auto ones = static_cast<unsigned>(std::count(v.begin(), v.end(), 1));
        INFO("PI = " << pi);
        // Code rate 8 / (8 + PI): 8 + PI bits kept of every 32.
        CHECK(ones == 8 + pi);
    }
}

TEST_CASE("the DAB phase reference symbol is unit modulus off DC", "[decode][dab]") {
    CHECK(std::abs(decode::dab_prs_value(0)) == 0.0F);
    CHECK(std::abs(decode::dab_prs_value(769)) == 0.0F);
    for (int k = -768; k <= 768; ++k) {
        if (k != 0) {
            CHECK(std::abs(std::abs(decode::dab_prs_value(k)) - 1.0F) < 1e-6F);
        }
    }
    // Table 23's first row, k = -768: i = 0, n = 1, h(0, 0) = 0, so phi = pi/2.
    CHECK(std::abs(decode::dab_prs_value(-768) - dsp::Complex32(0.0F, 1.0F)) < 1e-6F);
    // k = 1: k' = 1, i = 0, n = 3, h(0, 0) = 0, phi = 3 pi / 2.
    CHECK(std::abs(decode::dab_prs_value(1) - dsp::Complex32(0.0F, -1.0F)) < 1e-6F);
}

TEST_CASE("every EEP profile fills its sub-channel exactly", "[decode][dab]") {
    // Clause 11.3.2: the convolutional codeword of an EEP sub-channel is
    // exactly its size in CUs times 64 bits.
    constexpr std::array<unsigned, 4> kUnitA{12, 8, 6, 4};
    constexpr std::array<unsigned, 4> kUnitB{27, 21, 18, 15};
    for (unsigned level = 1; level <= 4; ++level) {
        for (unsigned n = 1; n <= 6; ++n) {
            for (const bool set_b : {false, true}) {
                DabSubchannel sc;
                sc.protection = set_b ? DabProtection::EepB : DabProtection::EepA;
                sc.level = level;
                sc.size_cu = n * (set_b ? kUnitB[level - 1] : kUnitA[level - 1]);
                auto p = decode::dab_eep_profile(sc);
                REQUIRE(p.has_value());
                const auto info = pseudo_bytes(p->information_bits, level * 100 + n);
                std::vector<std::uint8_t> bits(info.size());
                std::transform(info.begin(), info.end(), bits.begin(),
                               [](std::uint8_t b) { return static_cast<std::uint8_t>(b & 1U); });
                auto coded = decode::dab_convolutional_encode(bits, p->runs);
                REQUIRE(coded.has_value());
                INFO("level " << level << (set_b ? "-B" : "-A") << " n " << n);
                CHECK(coded->size() == sc.size_cu * decode::kDabCuBits);
                std::vector<float> soft(coded->size());
                std::transform(coded->begin(), coded->end(), soft.begin(),
                               [](std::uint8_t b) { return b != 0 ? -1.0F : 1.0F; });
                auto decoded = decode::dab_convolutional_decode(soft, bits.size(), p->runs);
                REQUIRE(decoded.has_value());
                CHECK(*decoded == bits);
            }
        }
    }
    // The FIC: 768 bits to 2304 (clause 11.2.1).
    std::vector<std::uint8_t> fic(768, 0);
    auto coded = decode::dab_convolutional_encode(fic, decode::dab_fic_runs());
    REQUIRE(coded.has_value());
    CHECK(coded->size() == decode::kDabFicCodewordBits);
}

// ---------------------------------------------------------------------------
// FIC, packet mode and MOT on their own
// ---------------------------------------------------------------------------

TEST_CASE("DAB FIGs round trip through a FIB", "[decode][dab]") {
    const auto subchannels = test_subchannels();
    std::vector<std::vector<std::uint8_t>> figs{siggen::dab_fig_ensemble(kEid, 1234),
                                                siggen::dab_fig_subchannels(subchannels)};
    auto fib = siggen::dab_pack_fib(figs);
    REQUIRE(fib.has_value());
    decode::DabEnsemble ens;
    CHECK(decode::dab_parse_fib(*fib, ens) == 2);
    CHECK(ens.eid == kEid);
    CHECK(ens.cif_count == 1234U);
    REQUIRE(ens.subchannels.size() == 2);
    CHECK(ens.subchannels.at(kPacketId).start_cu == 40);
    CHECK(ens.subchannels.at(kPacketId).size_cu == 32);
    CHECK(ens.subchannels.at(kPacketId).level == 2);

    std::vector<std::vector<std::uint8_t>> labels{siggen::dab_fig_label(1, kAudioSid, "Hello")};
    fib = siggen::dab_pack_fib(labels);
    REQUIRE(fib.has_value());
    CHECK(decode::dab_parse_fib(*fib, ens) == 1);
    REQUIRE(ens.services.at(kAudioSid).label.has_value());
    CHECK(ens.services.at(kAudioSid).label->text == "Hello");
}

TEST_CASE("DAB packets carry MSC data groups and MOT objects", "[decode][dab]") {
    const auto body = pseudo_bytes(500, 7);
    const auto header = siggen::mot_header(body.size(), 2, 1, "logo.png");
    std::vector<std::vector<std::uint8_t>> groups;
    groups.push_back(siggen::dab_data_group({3, 0, 0, true, true, 0, true, 0x0042},
                                                siggen::mot_segment(header, 0)));
    groups.push_back(siggen::dab_data_group(
        {4, 0, 0, true, false, 0, true, 0x0042},
        siggen::mot_segment(std::span<const std::uint8_t>(body).first(300), 0)));
    groups.push_back(siggen::dab_data_group(
        {4, 0, 0, true, true, 1, true, 0x0042},
        siggen::mot_segment(std::span<const std::uint8_t>(body).subspan(300), 0)));
    unsigned continuity = 0;
    std::vector<std::uint8_t> frame;
    for (const auto& g : groups) {
        for (const auto& p : siggen::dab_packetize(g, 7, 48, continuity)) {
            frame.insert(frame.end(), p.begin(), p.end());
        }
        const auto pad = siggen::dab_padding_packet(24);
        frame.insert(frame.end(), pad.begin(), pad.end());
    }
    decode::DabPacketAssembler packets(3);
    std::vector<decode::DabDataGroup> out;
    packets.push(frame, out);
    REQUIRE(out.size() == 3);
    CHECK(packets.packets_bad() == 0);
    CHECK(out[0].type == 3);
    CHECK(out[0].transport_id == std::uint16_t{0x0042});
    CHECK(out[2].last);
    decode::MotAssembler mot;
    std::vector<decode::MotObject> objects;
    for (const auto& g : out) {
        mot.push(g, objects);
    }
    REQUIRE(objects.size() == 1);
    CHECK(objects[0].content_name == "logo.png");
    CHECK(objects[0].content_type == 2);
    CHECK(objects[0].content_subtype == 1);
    CHECK(objects[0].body == body);

    // A corrupted packet loses its group and nothing else.
    frame[30] ^= 0x10;
    decode::DabPacketAssembler damaged(3);
    out.clear();
    damaged.push(frame, out);
    CHECK(damaged.packets_bad() == 1);
    CHECK(out.size() == 2);
}

// ---------------------------------------------------------------------------
// The air interface
// ---------------------------------------------------------------------------

TEST_CASE("a DAB ensemble round trips through noise, multipath and a carrier offset",
          "[decode][dab]") {
    const auto e = build_ensemble(12);
    Impairments imp;
    imp.snr_db = 10.0;
    imp.cfo_hz = 3412.0;  // three carriers and a bit
    imp.multipath = true;
    imp.lead_in = 70'001;
    const auto x = impair(e.samples, imp);
    const auto d = run(x, 65536);

    INFO("cfo " << d.sync.cfo_hz << " Hz, fibs " << d.sync.fibs_ok << " ok " << d.sync.fibs_bad
                << " bad, frames " << d.sync.frames);
    REQUIRE(d.sync.locked);
    CHECK(std::abs(d.sync.cfo_hz - imp.cfo_hz) < 20.0);
    CHECK(d.sync.fibs_bad == 0);
    REQUIRE(d.ensemble.label.has_value());
    CHECK(d.ensemble.label->text == "REVENANT TEST");
    CHECK(d.ensemble.eid == kEid);
    REQUIRE(d.ensemble.services.count(kAudioSid) == 1);
    REQUIRE(d.ensemble.services.at(kAudioSid).label.has_value());
    CHECK(d.ensemble.services.at(kAudioSid).label->text == "Stream Data");
    REQUIRE(d.ensemble.services.count(kDataSid) == 1);
    CHECK(d.ensemble.services.at(kDataSid).components.at(0).scid == 5);
    CHECK(d.ensemble.packet_components.at(5).address == kAddress);
    CHECK(d.ensemble.subchannels.size() == 2);

    std::size_t total = 0;
    const std::size_t ok = matching_stream_frames(d, e, total);
    WARN("stream logical frames right: " << ok << " of " << total << "; decode took "
                                        << d.seconds << " s for "
                                        << static_cast<double>(x.size()) / decode::kDabSampleRate
                                        << " s of signal");
    CHECK(total > 0);
    CHECK(ok == total);
    REQUIRE(!d.objects.empty());
    CHECK(d.objects[0].content_name == "slide1.jpg");
    CHECK(d.objects[0].body == e.mot_body);
}

TEST_CASE("DAB decodes the same in any blocking", "[decode][dab]") {
    const auto e = build_ensemble(6);
    Impairments imp;
    imp.snr_db = 12.0;
    imp.cfo_hz = -1730.0;
    imp.multipath = true;
    imp.lead_in = 12'345;
    imp.seed = 9;
    const auto x = impair(e.samples, imp);
    const auto whole = run(x, x.size());
    REQUIRE(!whole.frames.empty());
    for (const std::size_t block : {std::size_t{1000}, std::size_t{65537}, std::size_t{196'608}}) {
        INFO("block " << block);
        const auto split = run(x, block);
        REQUIRE(split.frames.size() == whole.frames.size());
        for (std::size_t i = 0; i < split.frames.size(); ++i) {
            CHECK(split.frames[i].bytes == whole.frames[i].bytes);
            CHECK(split.frames[i].cif == whole.frames[i].cif);
        }
        CHECK(split.sync.cfo_hz == whole.sync.cfo_hz);
        CHECK(split.sync.fibs_ok == whole.sync.fibs_ok);
    }
}

TEST_CASE("DAB FIC and sub-channel error rates against SNR", "[decode][dab][.measure]") {
    const auto e = build_ensemble(10);
    for (const double snr : {2.0, 4.0, 5.0, 6.0, 8.0, 10.0}) {
        Impairments imp;
        imp.snr_db = snr;
        imp.cfo_hz = 512.0;
        imp.multipath = true;
        imp.lead_in = 5000;
        imp.seed = static_cast<std::uint64_t>(snr * 10);
        const auto d = run(impair(e.samples, imp), 65536);
        std::size_t total = 0;
        const std::size_t ok = matching_stream_frames(d, e, total);
        WARN("SNR " << snr << " dB: FIBs " << d.sync.fibs_ok << " ok, " << d.sync.fibs_bad
                    << " bad; 3-A frames " << ok << " of " << total);
    }
}
