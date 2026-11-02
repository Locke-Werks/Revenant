// Z-Wave: the checks and the MAC frame against ITU-T G.9959 (01/2015), and the
// receiver against the transmitter in core/dsp/synth/zwave_mod.h.
//
// The Recommendation prints no complete sample frame, so the only printed
// worked example checked here is the multicast mask of 8.1.3.6.1. The rest is
// round trip. Error rates are measured and printed and the assertions around
// them are loose, as everywhere in this directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/decode/zwave.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/zwave_mod.h"

using namespace revenant;
using decode::ZWaveLayout;
using decode::ZWaveRate;

namespace {

std::uint64_t next(std::uint64_t& state) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 33U;
}

const char* name(ZWaveRate rate) {
    return rate == ZWaveRate::R1 ? "R1" : rate == ZWaveRate::R2 ? "R2" : "R3";
}

std::vector<std::vector<std::uint8_t>> make_frames(ZWaveRate rate, ZWaveLayout layout,
                                                   std::size_t count, std::uint64_t seed) {
    std::vector<std::vector<std::uint8_t>> frames;
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < count; ++i) {
        decode::ZWaveMpduFields f;
        f.home_id = static_cast<std::uint32_t>(next(state));
        f.source = static_cast<std::uint8_t>(1 + next(state) % 232);
        // Singlecast, ACK requested, sequence number 1..15 (8.1.3.3.7).
        f.frame_control = static_cast<std::uint16_t>(0x4100U | (1U + i % 15U));
        f.sequence_number = static_cast<std::uint8_t>(i);
        f.destination = {static_cast<std::uint8_t>(1 + next(state) % 232)};
        const std::size_t length = next(state) % 40;
        for (std::size_t b = 0; b < length; ++b) {
            f.payload.push_back(static_cast<std::uint8_t>(next(state)));
        }
        auto mpdu = decode::zwave_build_mpdu(f, rate, layout);
        REQUIRE(mpdu.has_value());
        frames.push_back(std::move(*mpdu));
    }
    return frames;
}

// Frames back to back with a gap of carrier-free silence between them.
std::vector<siggen::Complex32> render(const siggen::ZWaveModConfig& config,
                                      const std::vector<std::vector<std::uint8_t>>& frames) {
    const auto gap = static_cast<std::size_t>(config.sample_rate / 500);
    std::vector<siggen::Complex32> out(gap);
    for (const auto& frame : frames) {
        auto burst = siggen::zwave_render(config, frame);
        REQUIRE(burst.has_value());
        out.insert(out.end(), burst->begin(), burst->end());
        out.insert(out.end(), gap, siggen::Complex32{});
    }
    return out;
}

std::vector<decode::ZWaveFrame> decode_all(const decode::ZWaveConfig& config,
                                           std::span<const siggen::Complex32> samples,
                                           std::size_t block) {
    auto decoder = decode::ZWave::create(config);
    INFO((decoder.has_value() ? std::string{} : decoder.error().message));
    REQUIRE(decoder.has_value());
    std::vector<decode::ZWaveFrame> frames;
    for (std::size_t start = 0; start < samples.size(); start += block) {
        const std::size_t length = std::min(block, samples.size() - start);
        REQUIRE(decoder->process(samples.subspan(start, length), frames).has_value());
    }
    return frames;
}

std::size_t matched(const std::vector<decode::ZWaveFrame>& got,
                    const std::vector<std::vector<std::uint8_t>>& sent) {
    std::size_t hits = 0;
    std::size_t cursor = 0;
    for (const auto& frame : got) {
        for (std::size_t i = cursor; i < sent.size(); ++i) {
            if (frame.bytes == sent[i]) {
                ++hits;
                cursor = i + 1;
                break;
            }
        }
    }
    return hits;
}

// Unit-amplitude signal, so Eb is 1/Rb and the full-band noise power is
// fs / (Rb * Eb/N0).
void add_noise(std::vector<siggen::Complex32>& samples, double ebn0_db, ZWaveRate rate,
               dsp::SampleRate fs, std::uint64_t seed) {
    const double ebn0 = std::pow(10.0, ebn0_db / 10.0);
    const double power = static_cast<double>(fs) / (decode::zwave_bit_rate(rate) * ebn0);
    REQUIRE(siggen::add_awgn_at_power(samples, power, seed).has_value());
}

}  // namespace

// ---------------------------------------------------------------------------
// The checks and the frame, against the document
// ---------------------------------------------------------------------------

TEST_CASE("the Z-Wave checks follow 8.1.3.8 and 8.1.3.9", "[decode][zwave]") {
    // GenerateCheckSum starts at 0xFF.
    CHECK(decode::zwave_checksum({}) == 0xFF);
    const std::uint8_t bytes[] = {0x01, 0x02, 0x04};
    CHECK(decode::zwave_checksum(bytes) == (0xFF ^ 0x07));
    // The CRC generator "shall be initialized to 1D0Fh", no final XOR.
    CHECK(decode::zwave_crc16({}) == 0x1D0F);
    // A frame built with its CRC parses with the CRC valid, and fails with a
    // flipped bit.
    decode::ZWaveMpduFields f;
    f.home_id = 0xC0FFEE01;
    f.payload = {0x20, 0x01, 0xFF};
    auto mpdu = decode::zwave_build_mpdu(f, ZWaveRate::R3, ZWaveLayout::Config3);
    REQUIRE(mpdu.has_value());
    CHECK(decode::zwave_parse_mpdu(*mpdu, ZWaveRate::R3, ZWaveLayout::Config3)->fcs_valid);
    (*mpdu)[10] ^= 0x10;
    CHECK(!decode::zwave_parse_mpdu(*mpdu, ZWaveRate::R3, ZWaveLayout::Config3)->fcs_valid);
}

TEST_CASE("the Z-Wave multicast mask reproduces 8.1.3.6.1's example", "[decode][zwave]") {
    const std::uint8_t first[] = {0xC5};
    CHECK(decode::zwave_multicast_nodes(0, first) == std::vector<std::uint8_t>{1, 3, 7, 8});
    const std::uint8_t second[] = {0x00, 0xC5};
    CHECK(decode::zwave_multicast_nodes(0, second) == std::vector<std::uint8_t>{9, 11, 15, 16});
    // Table 8-16: offset 1 is 32 bits.
    CHECK(decode::zwave_multicast_nodes(1, first) == std::vector<std::uint8_t>{33, 35, 39, 40});
}

TEST_CASE("the Z-Wave MPDU header follows Figures 8-5 to 8-12", "[decode][zwave]") {
    decode::ZWaveMpduFields f;
    f.home_id = 0x01234567;
    f.source = 0x0F;
    // Figure 8-11: routed, ACK, low power, speed modified, header type 1;
    // beaming "10", sequence 0x9.
    f.frame_control = 0xF1'49;
    f.destination = {0x2A};
    f.payload = {0xAA, 0xBB};
    auto bytes = decode::zwave_build_mpdu(f, ZWaveRate::R2, ZWaveLayout::Config12);
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() == 12);
    CHECK((*bytes)[7] == 12);
    auto m = decode::zwave_parse_mpdu(*bytes, ZWaveRate::R2, ZWaveLayout::Config12);
    REQUIRE(m.has_value());
    CHECK(m->home_id == 0x01234567);
    CHECK(m->source == 0x0F);
    CHECK(m->frame_control.routed);
    CHECK(m->frame_control.ack_request);
    CHECK(m->frame_control.low_power);
    CHECK(m->frame_control.speed_modified);
    CHECK(m->frame_control.header_type == decode::kZWaveHeaderSinglecast);
    CHECK(m->frame_control.beaming == 2);
    CHECK(m->frame_control.sequence == 9);
    CHECK(m->destination == 0x2A);
    CHECK(m->payload == std::vector<std::uint8_t>{0xAA, 0xBB});
    CHECK(m->fcs_valid);

    // Figure 8-12 and 8-8: configuration 3 multicast, sequence number byte,
    // control byte with offset 0 and two mask bytes.
    f.frame_control = 0x8240;  // ACK bit 7, header type 2, beaming "100"
    f.sequence_number = 0x77;
    f.destination = {0x02, 0xC5, 0x01};
    bytes = decode::zwave_build_mpdu(f, ZWaveRate::R3, ZWaveLayout::Config3);
    REQUIRE(bytes.has_value());
    m = decode::zwave_parse_mpdu(*bytes, ZWaveRate::R3, ZWaveLayout::Config3);
    REQUIRE(m.has_value());
    CHECK(m->sequence_number == 0x77);
    CHECK(m->frame_control.ack_request);
    CHECK(m->frame_control.beaming == 4);
    CHECK(m->frame_control.header_type == decode::kZWaveHeaderMulticast);
    CHECK(m->multicast_nodes == std::vector<std::uint8_t>{1, 3, 7, 8, 9});
    CHECK(m->payload == std::vector<std::uint8_t>{0xAA, 0xBB});
    CHECK(m->fcs_valid);

    // Table 7-27: 64 bytes at R2.
    f.payload.assign(60, 0);
    CHECK(!decode::zwave_build_mpdu(f, ZWaveRate::R2, ZWaveLayout::Config12).has_value());
}

// ---------------------------------------------------------------------------
// The receiver against the transmitter
// ---------------------------------------------------------------------------

TEST_CASE("Z-Wave round trips at every rate in a 2.4 MS/s capture", "[decode][zwave]") {
    for (const ZWaveRate rate : {ZWaveRate::R1, ZWaveRate::R2, ZWaveRate::R3}) {
        INFO(name(rate));
        const ZWaveLayout layout = rate == ZWaveRate::R3 ? ZWaveLayout::Config3
                                                         : ZWaveLayout::Config12;
        const auto frames = make_frames(rate, layout, 6, 0x2A7E'0001ULL);
        siggen::ZWaveModConfig mod;
        mod.rate = rate;
        mod.channel_offset_hz = -350'000.0;
        auto samples = render(mod, frames);
        decode::ZWaveConfig config;
        config.rate = rate;
        config.layout = layout;
        config.channel_offset_hz = mod.channel_offset_hz;
        const auto got = decode_all(config, samples, 65536);
        REQUIRE(got.size() == frames.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].bytes == frames[i]);
            CHECK(got[i].mpdu.fcs_valid);
            CHECK(std::abs(got[i].frequency_error_hz) < 2000.0);
        }
    }
}

TEST_CASE("the Z-Wave receiver tolerates carrier error", "[decode][zwave]") {
    // 7.1.2.5.1 allows 27 ppm, about 24 kHz at 900 MHz, at each end.
    for (const ZWaveRate rate : {ZWaveRate::R1, ZWaveRate::R2, ZWaveRate::R3}) {
        for (const double error : {-20000.0, 15000.0}) {
            INFO(name(rate) << " carrier error " << error);
            const auto frames = make_frames(rate, ZWaveLayout::Config12, 4, 0x2A7E'0002ULL);
            siggen::ZWaveModConfig mod;
            mod.rate = rate;
            mod.sample_rate = 1'200'000;
            mod.channel_offset_hz = 100'000.0 + error;
            auto samples = render(mod, frames);
            decode::ZWaveConfig config;
            config.rate = rate;
            config.sample_rate = mod.sample_rate;
            config.channel_offset_hz = 100'000.0;
            const auto got = decode_all(config, samples, samples.size());
            CHECK(matched(got, frames) == frames.size());
            for (const auto& frame : got) {
                INFO("measured " << frame.frequency_error_hz);
                CHECK(std::abs(frame.frequency_error_hz - error) < 3000.0);
            }
        }
    }
}

TEST_CASE("the Z-Wave receiver does not depend on how its input is blocked", "[decode][zwave]") {
    for (const ZWaveRate rate : {ZWaveRate::R1, ZWaveRate::R2, ZWaveRate::R3}) {
        INFO(name(rate));
        const auto frames = make_frames(rate, ZWaveLayout::Config12, 5, 0x2A7E'0003ULL);
        siggen::ZWaveModConfig mod;
        mod.rate = rate;
        mod.sample_rate = 1'000'000;
        auto samples = render(mod, frames);
        add_noise(samples, 12.0, rate, mod.sample_rate, 0xB10C);
        decode::ZWaveConfig config;
        config.rate = rate;
        config.sample_rate = mod.sample_rate;
        const auto whole = decode_all(config, samples, samples.size());
        CHECK(!whole.empty());
        for (const std::size_t block : {std::size_t{4096}, std::size_t{777}, std::size_t{13}}) {
            INFO("block " << block);
            const auto split = decode_all(config, samples, block);
            REQUIRE(split.size() == whole.size());
            for (std::size_t i = 0; i < whole.size(); ++i) {
                CHECK(split[i].bytes == whole[i].bytes);
                CHECK(split[i].sof_end_sample == whole[i].sof_end_sample);
            }
        }
    }
}

TEST_CASE("Z-Wave packet error rates against noise, measured", "[decode][zwave]") {
    // Eb/N0 per information bit, 100 frames per point, payloads 0 to 39 bytes.
    // Measured on 2026-11-02 with these seeds:
    //
    //   Eb/N0   R1 (Manchester)   R2 (NRZ FSK)   R3 (GFSK)
    //   10 dB   1.00              0.55           0.39
    //   12 dB   0.73              0.07           0.09
    //   14 dB   0.13              0              0.01
    //   16 dB   0                 0              0.01
    //
    // R1 sits about 3 dB behind because Manchester spends each bit's energy
    // on two chips.
    struct RateCase {
        ZWaveRate rate;
        dsp::SampleRate fs;
    };
    const RateCase cases[] = {{ZWaveRate::R1, 192'000},
                              {ZWaveRate::R2, 400'000},
                              {ZWaveRate::R3, 1'000'000}};
    for (const RateCase& c : cases) {
        const auto frames = make_frames(c.rate, ZWaveLayout::Config12, 100, 0x2A7E'0004ULL);
        for (const double ebn0 : {8.0, 10.0, 12.0, 14.0, 16.0}) {
            siggen::ZWaveModConfig mod;
            mod.rate = c.rate;
            mod.sample_rate = c.fs;
            auto samples = render(mod, frames);
            add_noise(samples, ebn0, c.rate, c.fs, 0x5EED'2A7EULL);
            decode::ZWaveConfig config;
            config.rate = c.rate;
            config.sample_rate = c.fs;
            const auto got = decode_all(config, samples, 8192);
            const std::size_t hits = matched(got, frames);
            const double per = 1.0 - static_cast<double>(hits) / static_cast<double>(frames.size());
            INFO(name(c.rate) << " Eb/N0 " << ebn0 << " dB: PER " << per << ", decoded "
                              << got.size());
            // The 8-bit XOR of 8.1.3.8 misses any two errors in the same bit
            // column, so at low Eb/N0 a damaged R1 or R2 frame sometimes
            // passes. Counted, and held to a few in a hundred.
            const std::size_t undetected = got.size() - hits;
            INFO("undetected " << undetected);
            CHECK(undetected <= (c.rate == ZWaveRate::R3 ? 1U : 8U));
            if (ebn0 >= 16.0) {
                CHECK(per <= 0.1);
            }
        }
    }
}
