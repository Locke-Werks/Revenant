// P25 Phase 1: what comes out must not depend on how the input was blocked.
//
// WHY THIS FILE EXISTS
//
// The engine hands a receiver's stream to its decoder one block at a time,
// and the block length is whatever the engine was configured with. On
// 2026-09-22 the same synthetic P25 capture through the engine gave 4 of its 6
// headers at 16384-sample blocks and 1 of 6 at 65536, revenant-cli's default.
// A streaming decoder whose output moves with the blocking cannot be a
// reference for anything, which is the rule core/dsp/synth/modulators.h
// states for every generator and core/decode/dv_phy.h states for SymbolSync.
//
// So the first case below builds one capture the way a real channel arrives,
// a carrier a few hundred hertz off DC, a DC term in the baseband, noise, and
// quiet either side, and decodes it whole, in fixed blocks from 1024 to 65536
// samples, in random lengths, and one sample per call. Every blocking must
// produce the same data units, field for field and symbol for symbol, as the
// whole-capture call.
//
// WHAT IT WAS, AND WHICH STEP COST WHAT
//
// Three steps in P25Phase1::process depended on the blocking: the
// discriminator restarted against 1+0i at every call, the receive filter
// restarted from zeros, and each call's own mean was subtracted as the
// carrier offset. Measured on this file's capture, 2026-09-22, by putting
// each back on its own over the old sync search and slicer, which scored the
// sync word uncentred and sliced with no offset correction. Headers of 7,
// with the NID bits and Golay words the codes had to correct:
//
//     step put back            whole        1024-sample calls   2731-sample calls
//     all three (as it was)    6; 0, 0      5; 38, 28           5; 12, 10
//     filter restart only      6; 8, 14     4; 41, 35           5; 20, 13
//     discriminator only       6; 8, 14     6; 14, 33           6; 9, 17
//     per-call mean only       6; 0, 0      6; 2, 13            6; 0, 2
//     none, old search         6; 8, 14     6; 8, 14            6; 8, 14
//     none, as it is now       6; 0, 0      6; 0, 0             6; 0, 0
//
// 2731 samples is a 16384-sample engine block at 288000 S/s delivered at
// 48000. The filter restart was the largest cost and the mean the smallest.
// The mean was also doing a job: with it gone and nothing in its place, the
// old uncentred search framed nothing at all with the carrier 1200 Hz off
// (the second case below), where it had framed all seven whole-capture
// headers with 127 NID bits corrected. The sync search is now centred and the
// slicer is calibrated on each data unit's own sync word, which is blind to
// the blocking and does better than the mean did.
//
// The one header of seven missing in every row is the first, which starts at
// the carrier's first symbol. It is lost however the capture is blocked, and
// only with the baseband DC and the noise together. All seven decode with the
// noise and no DC, at no offset, at 350 Hz and at 1200 Hz; with the DC and no
// noise at 350 Hz; and with nothing added. Why that combination costs the
// first data unit is not established; the symbol timing estimator's pull-in
// after the quiet is the suspect.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/p25p1.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dv_mod.h"

using namespace revenant;
using decode::P25Duid;

namespace {

constexpr dsp::SampleRate kRate = 48'000;
constexpr std::uint16_t kNac = 0x293;
constexpr std::uint16_t kClearTalkgroup = 0x02A7;
constexpr std::uint16_t kSecureTalkgroup = 0x0100;
constexpr std::uint32_t kSource = 0x0012'D687;

// What a receiver mixed to a channel's nominal centre still has: the
// transmitter's own crystal puts the carrier a few hundred hertz off, and a
// front end's local oscillator leakage is a constant added to the baseband.
// Both are chosen to be well inside what C4FM tolerates and well outside
// zero, so a receiver that assumes either is zero shows it.
constexpr dsp::Hertz kCarrierOffsetHz = 350;
constexpr dsp::Complex32 kResidualDc{0.05F, -0.03F};

// Noise across the whole 48 kHz, against a unit-amplitude carrier, present in
// the quiet as well as under the signal: a real channel is never silent, and
// a discriminator fed noise alone reads a frequency anywhere in the band.
constexpr double kSnrFullBandDb = 20.0;
constexpr std::uint64_t kNoiseSeed = 0xB10C'0025'0000'0001ULL;
constexpr std::uint64_t kBlockingSeed = 0xB10C'0025'0000'0002ULL;

constexpr double kQuietSeconds = 0.5;

decode::P25Header header(bool encrypted) {
    decode::P25Header out;
    for (std::size_t i = 0; i < out.message_indicator.size(); ++i) {
        out.message_indicator[i] =
            encrypted ? static_cast<std::uint8_t>(0x11 * (i + 1)) : std::uint8_t{0};
    }
    // TIA-102.BAAC clause 2.8: 0x80 is unencrypted, and 0x84 is AES.
    out.algorithm_id = encrypted ? std::uint8_t{0x84} : decode::kP25AlgidUnencrypted;
    out.key_id = encrypted ? std::uint16_t{0x1234} : std::uint16_t{0};
    out.talkgroup_id = encrypted ? kSecureTalkgroup : kClearTalkgroup;
    return out;
}

// Six header messages alternating clear and encrypted, as
// tests/rpc/test_rpc_decode.cpp sends through the engine, then a voice call of
// two superframes so the LDUs and their Link Control are blocked too.
std::vector<std::uint8_t> transmission_dibits() {
    std::vector<std::uint8_t> dibits;
    for (int round = 0; round < 3; ++round) {
        for (const bool encrypted : {false, true}) {
            siggen::P25HeaderMessage message;
            message.network_access_code = kNac;
            message.header = header(encrypted);
            auto one = siggen::p25_header_message_dibits(message);
            REQUIRE(one.has_value());
            dibits.insert(dibits.end(), one->begin(), one->end());
        }
    }

    siggen::P25VoiceMessage voice;
    voice.network_access_code = kNac;
    voice.header = header(false);
    // Figure 5-6, format $00: LCF, MFID, emergency and reserved, TGID in
    // octets 4 and 5, source in 6 to 8.
    voice.link_control = {0x00,
                          0x00,
                          0x00,
                          0x00,
                          static_cast<std::uint8_t>(kClearTalkgroup >> 8U),
                          static_cast<std::uint8_t>(kClearTalkgroup & 0xFFU),
                          static_cast<std::uint8_t>(kSource >> 16U),
                          static_cast<std::uint8_t>((kSource >> 8U) & 0xFFU),
                          static_cast<std::uint8_t>(kSource & 0xFFU)};
    // Channel bits only; nothing here goes to a vocoder.
    std::uint64_t state = 0xB10C'0025'0000'0003ULL;
    voice.voice.resize(4 * decode::kP25VoiceFramesPerLdu);
    for (auto& frame : voice.voice) {
        for (std::uint8_t& bit : frame) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            bit = static_cast<std::uint8_t>((state >> 40U) & 1U);
        }
    }
    auto call = siggen::p25_voice_message_dibits(voice);
    REQUIRE(call.has_value());
    dibits.insert(dibits.end(), call->begin(), call->end());
    return dibits;
}

struct Condition {
    dsp::Hertz offset_hz = kCarrierOffsetHz;
    dsp::Complex32 dc = kResidualDc;
    bool noise = true;
};

std::vector<dsp::Complex32> capture(const Condition& condition = {}) {
    siggen::P25ModConfig mod;
    mod.rate = kRate;
    auto signal = siggen::p25_render_dibits(mod, transmission_dibits());
    REQUIRE(signal.has_value());

    siggen::FrequencyConfig offset;
    offset.doppler_shift_hz = condition.offset_hz;
    REQUIRE(siggen::apply_frequency_offset(*signal, offset, kRate, 0).has_value());

    const auto quiet = static_cast<std::size_t>(kQuietSeconds * static_cast<double>(kRate));
    std::vector<dsp::Complex32> all(quiet, dsp::Complex32{});
    all.insert(all.end(), signal->begin(), signal->end());
    all.insert(all.end(), quiet, dsp::Complex32{});
    for (dsp::Complex32& sample : all) {
        sample += condition.dc;
    }

    // Stated against the carrier's own power, one, rather than measured over
    // a buffer that is two thirds silence.
    if (condition.noise) {
        const double noise_power = std::pow(10.0, -kSnrFullBandDb / 10.0);
        REQUIRE(siggen::add_awgn_at_power(all, noise_power, kNoiseSeed).has_value());
    }
    return all;
}

// Everything a caller can read off a frame, as one comparable line.
std::string digest(const decode::P25Frame& frame) {
    std::string out = std::format(
        "{} nac {:03x} nid+{} at {} score {:.17g}{} failed {}", decode::p25_duid_name(frame.nid.duid),
        frame.nid.network_access_code, frame.nid.corrected_bits, frame.first_symbol,
        frame.sync_score, frame.inverted ? " inverted" : "", frame.code_word_failed);
    if (frame.header) {
        const decode::P25Header& h = *frame.header;
        out += std::format(" | hdu tg {:04x} algid {:02x} kid {:04x} mfid {:02x} golay {}/{}",
                           h.talkgroup_id, h.algorithm_id, h.key_id, h.manufacturer_id,
                           h.worst_golay_correction, h.golay_words_corrected);
    }
    if (frame.link_control) {
        const decode::P25LinkControl& lc = *frame.link_control;
        out += std::format(" | lc lcf {:02x} tg {:04x} src {:06x}", lc.format,
                           lc.talkgroup_id.value_or(0), lc.source_id.value_or(0));
    }
    if (frame.encryption_sync) {
        out += std::format(" | es algid {:02x}", frame.encryption_sync->algorithm_id);
    }
    std::uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;  // FNV-1a over the dibits
    for (const std::uint8_t dibit : frame.dibits) {
        hash = (hash ^ dibit) * 0x0000'0100'0000'01B3ULL;
    }
    out += std::format(" | {} dibits {:016x}", frame.dibits.size(), hash);
    return out;
}

struct Result {
    std::vector<std::string> frames;
    std::size_t headers = 0;
    std::size_t headers_correct = 0;
    std::size_t link_control_correct = 0;

    // What the codes had to repair across every data unit, which moves long
    // before a header is lost outright.
    std::size_t nid_bits_corrected = 0;
    std::size_t golay_words_corrected = 0;

    // What each data unit's sync word measured.
    std::vector<double> carrier_offsets_hz;
    std::vector<double> deviation_ratios;
};

// Decodes `samples` in calls of the lengths `next` hands out.
template <typename NextLength>
Result decode_blocked(std::span<const dsp::Complex32> samples, NextLength next) {
    decode::P25Config config;
    config.rate = kRate;
    auto decoder = decode::P25Phase1::create(config);
    REQUIRE(decoder.has_value());

    std::vector<decode::P25Frame> frames;
    std::size_t at = 0;
    while (at < samples.size()) {
        const std::size_t length = std::min<std::size_t>(next(), samples.size() - at);
        REQUIRE(decoder->process(samples.subspan(at, length), frames).has_value());
        at += length;
    }

    Result result;
    for (const decode::P25Frame& frame : frames) {
        result.frames.push_back(digest(frame));
        result.nid_bits_corrected += frame.nid.corrected_bits;
        result.carrier_offsets_hz.push_back(frame.carrier_offset_hz);
        result.deviation_ratios.push_back(frame.deviation_ratio);
        if (frame.nid.duid == static_cast<std::uint8_t>(P25Duid::HeaderDataUnit) &&
            frame.header) {
            ++result.headers;
            result.golay_words_corrected += frame.header->golay_words_corrected;
            // Every header sent is one of two, and each field has to agree
            // with the other three for the header to count.
            const bool encrypted = frame.header->encrypted;
            const bool correct =
                frame.nid.network_access_code == kNac &&
                frame.header->talkgroup_id == (encrypted ? kSecureTalkgroup : kClearTalkgroup) &&
                frame.header->algorithm_id == (encrypted ? 0x84 : decode::kP25AlgidUnencrypted) &&
                frame.header->key_id == (encrypted ? 0x1234 : 0);
            result.headers_correct += correct ? 1U : 0U;
        }
        if (frame.link_control) {
            const bool correct = frame.nid.network_access_code == kNac &&
                                 frame.link_control->talkgroup_id == kClearTalkgroup &&
                                 frame.link_control->source_id == kSource;
            result.link_control_correct += correct ? 1U : 0U;
        }
    }
    return result;
}

void report(std::string_view blocking, const Result& result) {
    std::println(
        "test_p25p1_blocking {}: {} data units, {} headers of 7 ({} correct), {} Link Control "
        "words of 2 correct, {} NID bits and {} Golay words corrected",
        blocking, result.frames.size(), result.headers, result.headers_correct,
        result.link_control_correct, result.nid_bits_corrected, result.golay_words_corrected);
}

}  // namespace

TEST_CASE("the streaming discriminator and filter match the one-call ones bit for bit",
          "[decode][p25]") {
    // core/decode/dv_phy.h claims both: that FmDiscriminator and RealFir give
    // a stream the output fm_discriminate and filter_real give it in one
    // call, and that the split does not matter. Checked with float equality,
    // not a tolerance, in 777-sample pieces, which divide nothing here.
    const std::vector<dsp::Complex32> samples = capture();
    const std::span<const dsp::Complex32> all(samples);

    std::vector<float> reference_hz(samples.size());
    REQUIRE(decode::fm_discriminate(all, reference_hz, kRate).has_value());
    // Any taps will do; a boxcar the length of P25's filter.
    constexpr std::size_t kTaps = decode::P25Config{}.filter_taps;
    const std::vector<float> taps(kTaps, 1.0F / static_cast<float>(kTaps));
    std::vector<float> reference_filtered(samples.size());
    REQUIRE(decode::filter_real(reference_hz, taps, reference_filtered).has_value());

    auto discriminator = decode::FmDiscriminator::create(kRate);
    REQUIRE(discriminator.has_value());
    auto filter = decode::RealFir::create(taps);
    REQUIRE(filter.has_value());

    constexpr std::size_t kPiece = 777;
    std::vector<float> hz(samples.size());
    std::vector<float> filtered(samples.size());
    for (std::size_t at = 0; at < samples.size(); at += kPiece) {
        const std::size_t count = std::min(kPiece, samples.size() - at);
        REQUIRE(discriminator->process(all.subspan(at, count),
                                       std::span(hz).subspan(at, count))
                    .has_value());
        REQUIRE(filter->process(std::span<const float>(hz).subspan(at, count),
                                std::span(filtered).subspan(at, count))
                    .has_value());
    }
    CHECK(hz == reference_hz);
    CHECK(filtered == reference_filtered);
}

TEST_CASE("P25 decodes the same capture identically however it is blocked", "[decode][p25]") {
    const std::vector<dsp::Complex32> samples = capture();
    const std::span<const dsp::Complex32> all(samples);

    const Result whole = decode_blocked(all, [&] { return samples.size(); });
    report("whole", whole);

    // The capture decodes at all, or identical output would be the trivial
    // kind. Six of the seven headers, for the reason the top of this file
    // gives, and every one of them right.
    CHECK(whole.headers == 6);
    CHECK(whole.headers_correct == whole.headers);
    CHECK(whole.link_control_correct == 2);

    // 2731 and 10923 are what a 16384 and a 65536-sample engine block at
    // 288000 S/s come to at 48000.
    struct Blocking {
        std::string name;
        std::size_t fixed;  // zero for random lengths
    };
    const Blocking blockings[] = {
        {"1024", 1'024},   {"2731", 2'731},   {"4096", 4'096},   {"10923", 10'923},
        {"16384", 16'384}, {"65536", 65'536}, {"random", 0},     {"one sample", 1},
    };

    std::println("test_p25p1_blocking random lengths from seed {:#x}", kBlockingSeed);
    for (const Blocking& blocking : blockings) {
        std::mt19937_64 random(kBlockingSeed);
        std::uniform_int_distribution<std::size_t> length(1, 20'000);
        const Result blocked = decode_blocked(all, [&] {
            return blocking.fixed != 0 ? blocking.fixed : length(random);
        });
        report(blocking.name, blocked);

        INFO("blocking " << blocking.name);
        REQUIRE(blocked.frames.size() == whole.frames.size());
        for (std::size_t i = 0; i < whole.frames.size(); ++i) {
            INFO("data unit " << i << "\n  whole:   " << whole.frames[i] << "\n  blocked: "
                              << blocked.frames[i]);
            CHECK(blocked.frames[i] == whole.frames[i]);
        }
    }
}

TEST_CASE("P25 frames a carrier 1200 Hz off DC from each data unit's own sync word",
          "[decode][p25]") {
    // Two symbol units of offset, which the old uncentred sync search could
    // not see through without the per-call mean it had to give up; the top
    // of this file has what that cost. 1200 Hz is where a transmitter whose
    // crystal is 1.5 parts per million off puts an 800 MHz carrier. No
    // baseband DC here, so all seven headers are expected.
    constexpr dsp::Hertz kOffsetHz = 1'200;
    const std::vector<dsp::Complex32> samples =
        capture(Condition{kOffsetHz, dsp::Complex32{}, true});
    const std::span<const dsp::Complex32> all(samples);

    const Result whole = decode_blocked(all, [&] { return samples.size(); });
    report("1200 Hz off, whole", whole);
    CHECK(whole.headers == 7);
    CHECK(whole.headers_correct == 7);
    CHECK(whole.link_control_correct == 2);
    CHECK(whole.nid_bits_corrected == 0);

    // What the sync words measured. The offset comes through the receive
    // filter, whose gain at DC is one, so it reads as sent. Measured
    // 2026-09-22: 1187.2 to 1196.5 Hz, and a deviation of 0.919 to 0.974 of
    // nominal, which is the outer symbols landing a little inside +/-3; the
    // fit's gain is what takes that out before slicing.
    REQUIRE_FALSE(whole.carrier_offsets_hz.empty());
    const auto [low, high] = std::ranges::minmax(whole.carrier_offsets_hz);
    const auto [narrow, wide] = std::ranges::minmax(whole.deviation_ratios);
    std::println("test_p25p1_blocking 1200 Hz off: sync words measured {:.1f} to {:.1f} Hz, "
                 "deviation {:.4f} to {:.4f} of nominal",
                 low, high, narrow, wide);
    CHECK(low > static_cast<double>(kOffsetHz) - 60.0);
    CHECK(high < static_cast<double>(kOffsetHz) + 60.0);
    CHECK(narrow > 0.85);
    CHECK(wide < 1.1);
}

