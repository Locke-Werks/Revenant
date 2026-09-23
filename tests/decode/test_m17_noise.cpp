// M17: what the receiver makes of noise, and what the rules that refuse it
// cost a real transmission.
//
// Over the wire, 2.5 s of noise produced two link setup frames that failed
// their CRC, one BERT burst and two packet bursts. None of it is reported as
// data, but a false sync costs a Viterbi decode per frame and a false
// transmission can hold the tracker while a real one starts. This file
// counts the syncs noise produces per hour and holds the real frames at
// 12 dB in 9 kHz, the level test_m17.cpp's measured case calls the knee.
//
// WHAT IT WAS
//
// Measured 2026-09-23 with this file's seeds. An hour of noise through a
// 12.5 kHz channel, before: 1615 LSF bursts, none with a valid CRC, 4055
// stream, 1153 packet and 956 BERT bursts and 2029 end markers, 9808 frames
// an hour. After: none in two hours. core/decode/m17.h and m17.cpp have the rules
// that did it: 32 preamble symbols in front of an LSF or BERT burst rather
// than eight, the clause 2.4 bursts a transmission may carry, and two stream
// frames whose Frame Numbers count by one for a late join.
//
// Twelve transmissions of 50 stream frames, frames reported, before and
// after, and what came through right:
//
//     dB in 9 kHz  LSF      stream   packet  BERT  end    payloads  LSFs right
//     14 before    21       616      2       1     16     600       12
//     14 after     12       600      0       0     12     600       12
//     12 before    20       609      2       6     16     592       11
//     12 after     11       597      0       0     12     596       11
//     10 before    16       586      2       18    17     487       2
//     10 after     7        567      0       0     13     482       2
//
// At 10 dB, below the knee, five payloads of 600 went: a transmission lost
// mid-stream is rejoined only on two good frames in a row now.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <print>
#include <span>
#include <string>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/decode/m17.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/m17_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

double noise_hours(double fallback) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, at test start.
    const char* text = std::getenv("REVENANT_M17_NOISE_HOURS");
    if (text == nullptr) {
        return fallback;
    }
    const double value = std::atof(text);
    return value > 0.0 ? value : fallback;
}

// The channel the wire path hands M17 through: a p25p1 receiver's fine stage,
// 12.5 kHz wide, per core/rpc/decoders.h.
double channel_response(double hertz, const void*) {
    return std::abs(hertz) <= 6250.0 ? 1.0 : 0.0;
}

struct Counts {
    std::size_t link_setup = 0;
    std::size_t link_setup_crc = 0;
    std::size_t stream = 0;
    std::size_t stream_decoded = 0;
    std::size_t packet = 0;
    std::size_t bert = 0;
    std::size_t end = 0;

    void add(const decode::M17Frame& frame) {
        switch (frame.kind) {
            case decode::M17FrameKind::LinkSetup:
                ++link_setup;
                link_setup_crc += (frame.lsf && frame.lsf->crc_valid) ? 1U : 0U;
                break;
            case decode::M17FrameKind::Stream:
                ++stream;
                stream_decoded += frame.stream ? 1U : 0U;
                break;
            case decode::M17FrameKind::Packet: ++packet; break;
            case decode::M17FrameKind::Bert: ++bert; break;
            case decode::M17FrameKind::EndOfTransmission: ++end; break;
        }
    }

    [[nodiscard]] std::size_t total() const { return link_setup + stream + packet + bert + end; }

    [[nodiscard]] std::string text() const {
        return std::format("{} LSF ({} CRC valid), {} stream ({} decoded), {} packet, {} "
                           "BERT, {} end",
                           link_setup, link_setup_crc, stream, stream_decoded, packet, bert, end);
    }
};

std::vector<std::array<std::uint8_t, 16>> payloads(std::size_t frames, std::uint64_t seed) {
    std::vector<std::array<std::uint8_t, 16>> out(frames);
    std::uint64_t state = seed;
    for (auto& frame : out) {
        for (std::uint8_t& byte : frame) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            byte = static_cast<std::uint8_t>(state >> 45U);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("M17 finds no transmission in noise alone", "[decode][m17]") {
    constexpr std::uint64_t kSeed = 0x4D17'0000'0000'0001ULL;
    const double hours = noise_hours(0.1);
    const auto total = static_cast<std::uint64_t>(hours * 3600.0 * static_cast<double>(kRate));

    auto taps = decode::design_from_response(kRate, 127, channel_response, nullptr);
    REQUIRE(taps.has_value());
    // A stateful filter across chunks, so the noise has no seam at each one.
    std::vector<dsp::Complex32> history(taps->size() - 1, dsp::Complex32{});

    auto decoder = decode::M17::create(decode::M17Config{});
    REQUIRE(decoder.has_value());

    constexpr std::size_t kChunk = 1U << 18U;
    std::vector<dsp::Complex32> noise;
    std::vector<dsp::Complex32> extended;
    std::vector<dsp::Complex32> filtered;
    std::vector<decode::M17Frame> frames;
    Counts counts;
    std::uint64_t done = 0;
    for (std::uint64_t chunk = 0; done < total; ++chunk) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, total - done));
        noise.assign(count, dsp::Complex32{});
        REQUIRE(siggen::add_awgn_at_power(noise, 1.0, siggen::derive_seed(kSeed, chunk))
                    .has_value());
        extended = history;
        extended.insert(extended.end(), noise.begin(), noise.end());
        filtered.resize(extended.size());
        REQUIRE(decode::filter_complex(extended, *taps, filtered).has_value());
        history.assign(extended.end() - static_cast<std::ptrdiff_t>(taps->size() - 1),
                       extended.end());

        frames.clear();
        REQUIRE(decoder
                    ->process(std::span<const dsp::Complex32>(filtered).subspan(taps->size() - 1),
                              frames)
                    .has_value());
        for (const decode::M17Frame& frame : frames) {
            counts.add(frame);
        }
        done += count;
    }

    std::println("test_m17_noise {:.3f} h of noise: {}; {:.1f} frames per hour", hours,
                 counts.text(), static_cast<double>(counts.total()) / hours);
    CHECK(counts.total() == 0);
}

TEST_CASE("M17 keeps its real frames at 12 dB", "[decode][m17]") {
    // Twelve transmissions of 50 stream frames, each with a second of noise
    // either side, at 12 dB in the 9 kHz channel M17 1.1 names.
    constexpr std::size_t kRuns = 12;
    constexpr std::size_t kFrames = 50;
    const double points[] = {14.0, 12.0, 10.0};

    for (const double snr : points) {
        Counts counts;
        std::size_t payloads_right = 0;
        std::size_t lsf_right = 0;
        for (std::size_t run = 0; run < kRuns; ++run) {
            siggen::M17StreamMessage message;
            message.destination = decode::m17_encode_callsign("M17-M17 C").value();
            message.source = decode::m17_encode_callsign("SP5WWP").value();
            message.type = static_cast<std::uint16_t>(0x0005U | (5U << 7U));
            message.payloads = payloads(kFrames, 0x4D17'0000'0000'0100ULL + run);
            auto signal = siggen::m17_render_stream(siggen::M17ModConfig{}, message);
            REQUIRE(signal.has_value());

            const auto quiet = static_cast<std::size_t>(kRate);
            std::vector<dsp::Complex32> all(quiet, dsp::Complex32{});
            all.insert(all.end(), signal->begin(), signal->end());
            all.insert(all.end(), quiet, dsp::Complex32{});
            // Unit carrier power, so this puts the carrier `snr` above the
            // noise in 9000 Hz.
            const double noise_power =
                std::pow(10.0, -snr / 10.0) * static_cast<double>(kRate) / 9000.0;
            REQUIRE(siggen::add_awgn_at_power(all, noise_power, 0x4D17'0000'0000'0200ULL + run)
                        .has_value());

            auto decoder = decode::M17::create(decode::M17Config{});
            REQUIRE(decoder.has_value());
            std::vector<decode::M17Frame> frames;
            constexpr std::size_t kBlock = 8'000;
            for (std::size_t at = 0; at < all.size(); at += kBlock) {
                const std::size_t length = std::min(kBlock, all.size() - at);
                REQUIRE(decoder->process(std::span<const dsp::Complex32>(all).subspan(at, length),
                                         frames)
                            .has_value());
            }
            for (const decode::M17Frame& frame : frames) {
                counts.add(frame);
                if (frame.kind == decode::M17FrameKind::Packet ||
                    frame.kind == decode::M17FrameKind::Bert) {
                    std::println("test_m17_noise {} dB run {}: kind {} at sample {} of {}, the "
                                 "transmission from {} to {}",
                                 snr, run, static_cast<int>(frame.kind), frame.first_sample,
                                 all.size(), quiet, quiet + signal->size());
                }
                if (frame.kind == decode::M17FrameKind::LinkSetup && frame.lsf &&
                    frame.lsf->crc_valid) {
                    ++lsf_right;
                }
                if (frame.stream && frame.stream->frame_number < kFrames &&
                    frame.stream->payload == message.payloads[frame.stream->frame_number]) {
                    ++payloads_right;
                }
            }
        }
        std::println("test_m17_noise {} dB in 9 kHz, {} transmissions of {} stream frames: {}; "
                     "{} payloads right, {} LSFs right",
                     snr, kRuns, kFrames, counts.text(), payloads_right, lsf_right);
        CHECK(counts.packet == 0);
        CHECK(counts.bert == 0);
        if (snr >= 12.0) {
            // Before the rules: 592 payloads and 11 LSFs at 12 dB, 600 and
            // 12 at 14 dB. The rules must keep every one.
            CHECK(payloads_right >= (snr >= 14.0 ? 600U : 592U));
            CHECK(lsf_right >= (snr >= 14.0 ? 12U : 11U));
        }
    }
}
