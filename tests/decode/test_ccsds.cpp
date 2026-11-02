// CCSDS telemetry, core/decode/ccsds.h, against the documents and against the
// transmitter in core/dsp/synth/ccsds_mod.h.
//
// The first group checks the library's codes against what 131.0-B-5 prints:
// the first 40 bits of both pseudo-random sequences (10.4.3 note 2), the
// conventional-to-dual-basis matrix of 4.3.9.3, and Annex G's generator
// coefficients for both values of E. Those are what let the code carry
// generators instead of copied tables. The second group is the transfer
// frame, packet and Reed-Solomon layers on bytes. The third is the whole
// chain through BPSK with white noise, Doppler and its drift, which is the
// case a satellite pass gives.
//
// Error rates are measured and printed; the assertions around them are loose.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "core/decode/ccsds.h"
#include "core/decode/psk_demod.h"
#include "core/dsp/synth/ccsds_mod.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/psk_mod.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

std::vector<std::uint8_t> random_octets(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::uint8_t> out(n);
    for (auto& o : out) {
        o = static_cast<std::uint8_t>(rng() & 0xFFU);
    }
    return out;
}

std::string bits_string(const std::vector<std::uint8_t>& bits) {
    std::string s;
    for (auto b : bits) {
        s.push_back(b != 0U ? '1' : '0');
    }
    return s;
}

}  // namespace

TEST_CASE("CCSDS pseudo-randomisers match the 40 bits 131.0 10.4.3 prints", "[decode][ccsds]") {
    CHECK(bits_string(ccsds_pn_bits(CcsdsRandomizer::Long131071, 40)) ==
          "0001110001110001101110010001101110101001");
    CHECK(bits_string(ccsds_pn_bits(CcsdsRandomizer::Legacy255, 40)) ==
          "1111111101001000000011101100000010011010");
    // Periods, 10.4.1 and 10.4.2.
    const auto legacy = ccsds_pn_bits(CcsdsRandomizer::Legacy255, 600);
    CHECK(std::equal(legacy.begin(), legacy.begin() + 255, legacy.begin() + 255));
    const auto longer = ccsds_pn_bits(CcsdsRandomizer::Long131071, 131071 + 64);
    CHECK(std::equal(longer.begin(), longer.begin() + 64, longer.begin() + 131071));
    // Randomising twice restores the data.
    auto data = random_octets(1275, 3);
    const auto copy = data;
    ccsds_randomize(data, CcsdsRandomizer::Long131071);
    CHECK(data != copy);
    ccsds_randomize(data, CcsdsRandomizer::Long131071);
    CHECK(data == copy);
}

TEST_CASE("CCSDS dual basis matches the matrix 131.0 4.3.9.3 prints", "[decode][ccsds]") {
    // [z0..z7] = [u7..u0] M: row r is the image of u_(7-r).
    constexpr std::string_view kRows[8] = {"10001101", "11101111", "11101100", "10000110",
                                           "11111010", "10011001", "10101111", "01111011"};
    for (unsigned r = 0; r < 8; ++r) {
        const auto z = CcsdsReedSolomon::conventional_to_dual(static_cast<std::uint8_t>(1U << (7U - r)));
        std::string got;
        for (unsigned j = 0; j < 8; ++j) {
            got.push_back(((z >> (7U - j)) & 1U) != 0U ? '1' : '0');
        }
        CHECK(got == kRows[r]);
    }
    for (unsigned w = 0; w < 256; ++w) {
        const auto u = static_cast<std::uint8_t>(w);
        CHECK(CcsdsReedSolomon::dual_to_conventional(CcsdsReedSolomon::conventional_to_dual(u)) == u);
    }
}

TEST_CASE("CCSDS Reed-Solomon generators match Annex G", "[decode][ccsds]") {
    // Annex G gives G_i as powers of alpha; the polynomial is symmetric.
    const unsigned e16[17] = {0, 249, 59, 66, 4, 43, 126, 251, 97, 30, 3, 213, 50, 66, 170, 5, 24};
    const auto g16 = CcsdsReedSolomon::generator(16);
    REQUIRE(g16.size() == 33);
    for (unsigned i = 0; i <= 16; ++i) {
        CHECK(g16[i] == CcsdsReedSolomon::alpha_power(e16[i]));
        CHECK(g16[32 - i] == CcsdsReedSolomon::alpha_power(e16[i]));
    }
    const unsigned e8[9] = {0, 30, 230, 49, 235, 129, 81, 76, 173};
    const auto g8 = CcsdsReedSolomon::generator(8);
    REQUIRE(g8.size() == 17);
    for (unsigned i = 0; i <= 8; ++i) {
        CHECK(g8[i] == CcsdsReedSolomon::alpha_power(e8[i]));
        CHECK(g8[16 - i] == CcsdsReedSolomon::alpha_power(e8[i]));
    }
}

TEST_CASE("CCSDS Reed-Solomon corrects E errors per codeword at every depth", "[decode][ccsds]") {
    std::mt19937_64 rng(77);
    for (const unsigned e : {16U, 8U}) {
        for (const unsigned depth : {1U, 2U, 3U, 4U, 5U, 8U}) {
            for (const std::size_t fill_per_word : {std::size_t{0}, std::size_t{30}}) {
                CcsdsRsConfig c{e, depth, fill_per_word * depth};
                auto rs = CcsdsReedSolomon::create(c).value();
                const auto data = random_octets(rs.data_octets(), e * 100 + depth);
                const auto block = rs.encode(data);
                REQUIRE(block.size() == rs.codeblock_octets());
                CHECK(std::equal(data.begin(), data.end(), block.begin()));

                auto clean = block;
                CHECK(rs.decode(clean).total_corrected() == 0);

                // E errors in every codeword: interleaved positions s*I + c.
                auto hit = block;
                const std::size_t len = 255 - fill_per_word;
                for (unsigned cw = 0; cw < depth; ++cw) {
                    std::vector<std::size_t> pos(len);
                    for (std::size_t s = 0; s < len; ++s) {
                        pos[s] = s;
                    }
                    std::shuffle(pos.begin(), pos.end(), rng);
                    for (unsigned k = 0; k < e; ++k) {
                        hit[pos[k] * depth + cw] ^= static_cast<std::uint8_t>(1U + (rng() % 255U));
                    }
                }
                const auto r = rs.decode(hit);
                CHECK(r.ok());
                CHECK(r.total_corrected() == static_cast<std::size_t>(e) * depth);
                CHECK(hit == block);
            }
        }
    }
    // A burst of 16 * 5 octets is corrected at depth 5, the point of
    // interleaving (131.0 4.4.1).
    auto rs = CcsdsReedSolomon::create({16, 5, 0}).value();
    const auto block = rs.encode(random_octets(rs.data_octets(), 9));
    auto hit = block;
    for (std::size_t i = 300; i < 380; ++i) {
        hit[i] ^= 0xA5;
    }
    CHECK(rs.decode(hit).ok());
    CHECK(hit == block);
    // E + 1 errors are refused, not miscorrected, nearly always.
    auto over = block;
    for (std::size_t s = 0; s < 17; ++s) {
        over[s * 5] ^= 0x3C;
    }
    CHECK_FALSE(rs.decode(over).corrected[0].has_value());

    CHECK_FALSE(CcsdsReedSolomon::create({12, 1, 0}).has_value());
    CHECK_FALSE(CcsdsReedSolomon::create({16, 6, 0}).has_value());
    CHECK_FALSE(CcsdsReedSolomon::create({16, 2, 3}).has_value());
}

TEST_CASE("CCSDS frame and packet headers round trip, FECF is CRC-16/IBM-3740", "[decode][ccsds]") {
    const std::uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(ccsds_fecf(check) == 0x29B1);

    TmFrameHeader tm;
    tm.spacecraft_id = 0x2A5;
    tm.virtual_channel = 5;
    tm.ocf_present = true;
    tm.master_frame_count = 200;
    tm.vc_frame_count = 17;
    tm.first_header_pointer = 0x123;
    std::uint8_t buf[6];
    write_tm_frame_header(tm, buf);
    const auto tm2 = parse_tm_frame_header(buf).value();
    CHECK(tm2.spacecraft_id == 0x2A5);
    CHECK(tm2.virtual_channel == 5);
    CHECK(tm2.ocf_present);
    CHECK(tm2.master_frame_count == 200);
    CHECK(tm2.vc_frame_count == 17);
    CHECK(tm2.segment_length_id == 3);
    CHECK(tm2.first_header_pointer == 0x123);

    AosFrameHeader aos;
    aos.spacecraft_id = 0xC3;
    aos.virtual_channel = 0x2B;
    aos.vc_frame_count = 0xABCDEF;
    aos.replay = true;
    write_aos_frame_header(aos, buf);
    const auto aos2 = parse_aos_frame_header(buf).value();
    CHECK(aos2.spacecraft_id == 0xC3);
    CHECK(aos2.virtual_channel == 0x2B);
    CHECK(aos2.vc_frame_count == 0xABCDEFU);
    CHECK(aos2.replay);
    CHECK_FALSE(parse_tm_frame_header(buf).has_value());  // version 01 is AOS

    SpacePacketHeader sp;
    sp.apid = 0x3FF;
    sp.secondary_header = true;
    sp.sequence_flags = 1;
    sp.sequence_count = 0x2ABC;
    sp.data_length = 999;
    write_space_packet_header(sp, buf);
    const auto sp2 = parse_space_packet_header(buf).value();
    CHECK(sp2.apid == 0x3FF);
    CHECK(sp2.secondary_header);
    CHECK(sp2.sequence_flags == 1);
    CHECK(sp2.sequence_count == 0x2ABC);
    CHECK(sp2.total_octets() == 1006);
}

namespace {

std::vector<std::vector<std::uint8_t>> make_packets(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::vector<std::uint8_t>> out;
    for (std::size_t i = 0; i < count; ++i) {
        SpacePacketHeader h;
        h.apid = static_cast<std::uint16_t>(100 + i % 3);
        h.sequence_count = static_cast<std::uint16_t>(i);
        out.push_back(siggen::ccsds_space_packet(h, random_octets(20 + rng() % 700, seed + i)));
    }
    return out;
}

}  // namespace

TEST_CASE("CCSDS packets are reassembled across frames and survive a lost frame", "[decode][ccsds]") {
    const auto packets = make_packets(40, 5);
    TmFrameHeader h;
    h.spacecraft_id = 42;
    const auto frames = siggen::ccsds_pack_tm_frames(h, 223, true, packets);
    REQUIRE(frames.size() > 10);

    SpacePacketExtractor ex;
    std::vector<SpacePacket> got;
    for (const auto& f : frames) {
        CHECK(ccsds_fecf_ok(f));
        const auto fh = parse_tm_frame_header(f).value();
        ex.push(std::span(f).subspan(6, 223 - 8), fh.first_header_pointer, true, got);
    }
    REQUIRE(got.size() == packets.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].octets == packets[i]);
    }

    // Drop frame 5: the packets it touched are lost, the rest come through.
    SpacePacketExtractor ex2;
    std::vector<SpacePacket> got2;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (i == 5) {
            continue;
        }
        const auto fh = parse_tm_frame_header(frames[i]).value();
        ex2.push(std::span(frames[i]).subspan(6, 223 - 8), fh.first_header_pointer, i != 6, got2);
    }
    CHECK(got2.size() < packets.size());
    CHECK(got2.size() + 4 >= packets.size());
    for (const auto& p : got2) {
        CHECK(std::find(packets.begin(), packets.end(), p.octets) != packets.end());
    }
}

namespace {

std::vector<float> to_soft(const std::vector<std::uint8_t>& bits, bool invert) {
    std::vector<float> s(bits.size());
    for (std::size_t i = 0; i < bits.size(); ++i) {
        s[i] = ((bits[i] != 0U) != invert) ? -1.0F : 1.0F;
    }
    return s;
}

std::vector<CcsdsFrame> receive(const CcsdsReceiverConfig& c, const std::vector<float>& soft, std::size_t chunk) {
    auto rx = CcsdsReceiver::create(c).value();
    std::vector<CcsdsFrame> out;
    for (std::size_t i = 0; i < soft.size(); i += chunk) {
        const std::size_t n = std::min(chunk, soft.size() - i);
        rx.push(std::span(soft).subspan(i, n), out);
    }
    rx.flush(out);
    return out;
}

}  // namespace

TEST_CASE("CCSDS receiver recovers frames in every coding configuration and blocking",
          "[decode][ccsds]") {
    struct Case {
        bool rs;
        unsigned depth;
        bool conv;
        bool nrzm;
        CcsdsRandomizer rand;
    };
    const Case cases[] = {
        {false, 1, false, false, CcsdsRandomizer::None},
        {true, 1, false, false, CcsdsRandomizer::Legacy255},
        {true, 4, true, false, CcsdsRandomizer::Legacy255},
        {true, 5, true, false, CcsdsRandomizer::Long131071},
        {false, 1, true, true, CcsdsRandomizer::Long131071},
    };
    for (const Case& k : cases) {
        siggen::CcsdsTxConfig tx;
        tx.reed_solomon = k.rs;
        tx.rs = {16, k.depth, 0};
        tx.randomizer = k.rand;
        tx.convolutional = k.conv;
        tx.nrzm = k.nrzm;
        const std::size_t frame_octets = k.rs ? 223 * k.depth : 300;
        std::vector<std::vector<std::uint8_t>> frames;
        for (int i = 0; i < 6; ++i) {
            frames.push_back(random_octets(frame_octets, 1000 + i));
        }
        const auto bits = siggen::ccsds_channel_bits(tx, frames).value();

        CcsdsReceiverConfig rc;
        rc.frame_octets = frame_octets;
        rc.reed_solomon = k.rs;
        rc.rs = tx.rs;
        rc.randomizer = k.rand;
        rc.convolutional = k.conv;
        rc.nrzm = k.nrzm;

        // Both polarities, and with an odd number of symbols of junk in front
        // so the convolutional pairs start on the other alignment.
        for (const bool invert : {false, true}) {
            for (const std::size_t lead : {std::size_t{0}, std::size_t{37}}) {
                std::vector<std::uint8_t> stream(lead, 0);
                for (std::size_t i = 0; i < lead; ++i) {
                    stream[i] = static_cast<std::uint8_t>((i * 7 + 3) % 5 < 2);
                }
                stream.insert(stream.end(), bits.begin(), bits.end());
                const auto soft = to_soft(stream, invert);
                const auto whole = receive(rc, soft, soft.size());
                const auto pieces = receive(rc, soft, 113);
                const auto single = receive(rc, soft, 1);
                INFO("rs " << k.rs << " depth " << k.depth << " conv " << k.conv << " nrzm " << k.nrzm
                           << " invert " << invert << " lead " << lead);
                // NRZ-M loses the first frame's first bit to the unknown
                // starting level, and the marker before it is what absorbs it.
                REQUIRE(whole.size() == frames.size());
                for (std::size_t i = 0; i < frames.size(); ++i) {
                    CHECK(whole[i].frame == frames[i]);
                    if (!k.nrzm) {
                        CHECK(whole[i].inverted == invert);
                    }
                }
                REQUIRE(pieces.size() == whole.size());
                REQUIRE(single.size() == whole.size());
                for (std::size_t i = 0; i < whole.size(); ++i) {
                    CHECK(pieces[i].frame == whole[i].frame);
                    CHECK(single[i].frame == whole[i].frame);
                    CHECK(pieces[i].bit_position == whole[i].bit_position);
                }
            }
        }
    }
}

TEST_CASE("CCSDS concatenated code over BPSK with noise, Doppler and drift", "[decode][ccsds]") {
    constexpr dsp::SampleRate kRate = 48000;
    constexpr double kSymbolRate = 4800.0;  // channel symbols: 2400 bit/s decoded

    siggen::CcsdsTxConfig tx;
    tx.reed_solomon = true;
    tx.rs = {16, 1, 0};
    tx.randomizer = CcsdsRandomizer::Legacy255;
    tx.convolutional = true;

    const auto packets = make_packets(15, 11);
    TmFrameHeader h;
    h.spacecraft_id = 0x1AB;
    const auto frames = siggen::ccsds_pack_tm_frames(h, 223, true, packets);
    auto bits = siggen::ccsds_channel_bits(tx, frames).value();
    // A preamble of idle symbols so the demodulator acquires before the first
    // marker; a real downlink is a continuous carrier.
    std::vector<std::uint8_t> symbols(1024);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        symbols[i] = static_cast<std::uint8_t>((i * 2654435761U >> 7U) & 1U);
    }
    symbols.insert(symbols.end(), bits.begin(), bits.end());

    for (const double eb_n0_db : {4.0, 6.0, 8.0}) {
        siggen::PskModConfig mod;
        mod.rate = kRate;
        mod.symbol_rate = kSymbolRate;
        mod.modulation = PskModulation::Bpsk;
        mod.initial_phase = 1.1;
        mod.timing_offset_samples = 0.4;
        mod.clock_error = 20e-6;
        auto samples = siggen::psk_render(mod, symbols).value();

        // Doppler on a LEO pass at 437 MHz: a 300 Hz offset sweeping at
        // 20 Hz/s, as it does well before closest approach.
        siggen::FrequencyConfig fc;
        fc.doppler_shift_hz = 300;
        fc.drift_hz_per_second = -20.0;
        REQUIRE(siggen::apply_frequency_offset(samples, fc, kRate, 7).has_value());
        // Eb is per decoded information bit: two channel symbols each, and the
        // RS code rate 223/255 on top.
        const double info_rate = kSymbolRate / 2.0 * 223.0 / 255.0;
        REQUIRE(siggen::add_awgn(samples, siggen::NoiseLevel::eb_over_n0_db(eb_n0_db, info_rate), kRate,
                                 static_cast<std::uint64_t>(eb_n0_db * 10))
                    .has_value());

        PskDemodConfig dc;
        dc.rate = kRate;
        dc.symbol_rate = kSymbolRate;
        dc.modulation = PskModulation::Bpsk;
        auto demod = PskDemod::create(dc).value();
        CcsdsReceiverConfig rc;
        rc.reed_solomon = true;
        rc.rs = tx.rs;
        rc.randomizer = tx.randomizer;
        rc.convolutional = true;
        auto rx = CcsdsReceiver::create(rc).value();

        std::vector<CcsdsFrame> got;
        std::vector<float> soft;
        for (std::size_t i = 0; i < samples.size(); i += 4096) {
            const auto n = std::min<std::size_t>(4096, samples.size() - i);
            soft.clear();
            for (const auto& s : demod.process(std::span(samples).subspan(i, n))) {
                soft.push_back(s.soft[0]);
            }
            rx.push(soft, got);
        }
        rx.flush(got);

        std::size_t good = 0;
        std::size_t corrected = 0;
        SpacePacketExtractor ex;
        std::vector<SpacePacket> out;
        int last_count = -1;
        for (const auto& f : got) {
            if (!f.rs.ok() || !ccsds_fecf_ok(f.frame)) {
                continue;
            }
            ++good;
            corrected += f.rs.total_corrected();
            const auto fh = parse_tm_frame_header(f.frame).value();
            CHECK(fh.spacecraft_id == 0x1AB);
            const bool continuous = last_count >= 0 && ((last_count + 1) & 0xFF) == fh.vc_frame_count;
            last_count = fh.vc_frame_count;
            ex.push(std::span(f.frame).subspan(6, 223 - 8), fh.first_header_pointer, continuous, out);
        }
        std::size_t matched = 0;
        for (const auto& p : out) {
            matched += std::find(packets.begin(), packets.end(), p.octets) != packets.end() ? 1 : 0;
        }
        std::printf("CCSDS r=1/2 K=7 + RS(255,223) BPSK, Eb/N0 %.1f dB, Doppler 300 Hz at -20 Hz/s: "
                    "%zu/%zu frames, %zu RS symbols corrected, %zu/%zu packets\n",
                    eb_n0_db, good, frames.size(), corrected, matched, packets.size());
        CHECK(matched == out.size());
        if (eb_n0_db >= 6.0) {
            CHECK(good == frames.size());
            CHECK(matched == packets.size());
        } else {
            CHECK(2 * good >= frames.size());
        }
    }
}
