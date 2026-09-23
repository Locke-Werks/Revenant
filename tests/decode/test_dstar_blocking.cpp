// D-STAR DV: what comes out must not depend on how the input was blocked.
//
// The same property tests/decode/test_p25p1_blocking.cpp holds P25 to, and
// for the same reason: the engine feeds a decoder one block at a time, the
// block length is configuration, and a decoder whose output moves with it
// cannot be a reference for anything. One capture the way a real channel
// arrives, a carrier a few hundred hertz off, baseband DC and noise, decoded
// whole, in fixed blocks from 1024 to 65536 samples, in random lengths and
// one sample a call, and every blocking must give the same transmissions,
// header, voice frames and all, as the whole-capture call.
//
// WHAT IT WAS
//
// Measured on this file's capture, 2026-09-23, three transmissions of 30
// voice frames each, before the fix: headers of 3 with a valid frame check
// sequence, and voice frames reported of 90.
//
//     blocking      headers   voice frames
//     whole         1         161
//     1024          2         0
//     2731          3         2
//     4096          2         4
//     10923         3         13
//     16384         3         31
//     65536         2         85
//     random        3         21, 4 of their bits wrong
//     one sample    0         0
//
// Four things did it. DStar::process restarted its discriminator and its
// receive filter at every call, subtracted each call's mean as the carrier
// offset and divided by each call's peak, so one-sample calls decoded
// nothing. A transmission was reported once, as soon as its header decoded,
// with whichever voice frames had already arrived, and the rest were never
// read. And the clause 4.1.2 h last frame was looked for 40 bits past where
// the clause puts it, so no transmission ever ended: the whole capture's one
// header took the voice frames of all three, and the other two headers were
// read as voice. Now every blocking gives 3 headers, 90 frames, 3 ended and
// no voice bit wrong, identically.

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

#include "core/decode/dstar.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dv_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

// The conditions test_p25p1_blocking.cpp uses, so the two read side by side.
constexpr dsp::Hertz kCarrierOffsetHz = 350;
constexpr dsp::Complex32 kResidualDc{0.05F, -0.03F};
constexpr double kSnrFullBandDb = 20.0;
constexpr std::uint64_t kNoiseSeed = 0xB10C'05A7'0000'0001ULL;
constexpr std::uint64_t kBlockingSeed = 0xB10C'05A7'0000'0002ULL;

constexpr double kQuietSeconds = 0.5;
constexpr double kGapSeconds = 0.3;

// Three transmissions of 30 voice frames each: past the 21-frame
// resynchronisation interval, so the second superframe's resync is read too.
constexpr std::size_t kTransmissions = 3;
constexpr std::size_t kVoiceFrames = 30;

const std::array<const char*, kTransmissions> kCallsigns = {"JA1RL", "JH1XYZ", "JR1AB"};

decode::DStarHeader header(std::size_t index) {
    decode::DStarHeader out;
    out.flag1 = 0b0100'0000;
    out.destination_repeater = "JP1YIU A";
    out.departure_repeater = "JP1YIU G";
    out.companion = "CQCQCQ";
    out.own_callsign = kCallsigns[index];
    out.own_suffix = "MOBL";
    return out;
}

using VoiceBits = std::array<std::uint8_t, decode::kDStarVoiceBits>;

std::vector<VoiceBits> voice(std::size_t index) {
    std::vector<VoiceBits> out(kVoiceFrames);
    std::uint64_t state = 0xB10C'05A7'0000'0010ULL + index;
    for (VoiceBits& frame : out) {
        for (std::uint8_t& bit : frame) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            bit = static_cast<std::uint8_t>((state >> 40U) & 1ULL);
        }
    }
    return out;
}

struct Condition {
    dsp::Hertz offset_hz = kCarrierOffsetHz;
    dsp::Complex32 dc = kResidualDc;
    bool noise = true;
};

std::vector<dsp::Complex32> capture(const Condition& condition = {}) {
    const auto quiet = static_cast<std::size_t>(kQuietSeconds * static_cast<double>(kRate));
    const auto gap = static_cast<std::size_t>(kGapSeconds * static_cast<double>(kRate));

    std::vector<dsp::Complex32> all(quiet, dsp::Complex32{});
    for (std::size_t t = 0; t < kTransmissions; ++t) {
        siggen::DStarMessage message;
        message.header = header(t);
        message.voice_frames = voice(t);
        siggen::DStarModConfig mod;
        mod.rate = kRate;
        auto signal = siggen::dstar_render(mod, message);
        REQUIRE(signal.has_value());
        all.insert(all.end(), signal->begin(), signal->end());
        all.insert(all.end(), t + 1 < kTransmissions ? gap : quiet, dsp::Complex32{});
    }

    siggen::FrequencyConfig offset;
    offset.doppler_shift_hz = condition.offset_hz;
    REQUIRE(siggen::apply_frequency_offset(all, offset, kRate, 0).has_value());
    for (dsp::Complex32& sample : all) {
        sample += condition.dc;
    }
    if (condition.noise) {
        const double noise_power = std::pow(10.0, -kSnrFullBandDb / 10.0);
        REQUIRE(siggen::add_awgn_at_power(all, noise_power, kNoiseSeed).has_value());
    }
    return all;
}

// A transmission is its header record and every record without a header that
// follows it, which is how a decoder reporting a transmission in pieces hands
// it over.
struct Transmission {
    decode::DStarTransmission first;
    std::vector<decode::DStarVoiceFrame> frames;
    bool ended = false;
};

std::vector<Transmission> gather(const std::vector<decode::DStarTransmission>& records) {
    std::vector<Transmission> out;
    for (const decode::DStarTransmission& record : records) {
        if (record.header.has_value() || out.empty()) {
            out.push_back(Transmission{record, {}, false});
        }
        out.back().frames.insert(out.back().frames.end(), record.frames.begin(),
                                 record.frames.end());
        out.back().ended = out.back().ended || record.ended;
    }
    return out;
}

std::string digest(const Transmission& t) {
    std::string out = std::format("at {} score {:.17g}{}", t.first.first_bit, t.first.sync_score,
                                  t.first.inverted ? " inverted" : "");
    if (t.first.header) {
        const decode::DStarHeader& h = *t.first.header;
        out += std::format(" | {} {} {} {} {} fcs {:04x}{}", h.own_callsign, h.own_suffix,
                           h.companion, h.departure_repeater, h.destination_repeater, h.fcs,
                           h.fcs_valid ? "" : " failed");
    }
    std::uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;
    std::size_t resyncs = 0;
    for (const decode::DStarVoiceFrame& frame : t.frames) {
        for (const std::uint8_t bit : frame.voice) {
            hash = (hash ^ bit) * 0x0000'0100'0000'01B3ULL;
        }
        for (const std::uint8_t bit : frame.data) {
            hash = (hash ^ bit) * 0x0000'0100'0000'01B3ULL;
        }
        resyncs += frame.carried_resync ? 1U : 0U;
    }
    out += std::format(" | {} frames, {} resync, {}, bits {:016x}", t.frames.size(), resyncs,
                       t.ended ? "ended" : "open", hash);
    return out;
}

struct Result {
    std::vector<std::string> transmissions;
    std::vector<double> carrier_offsets_hz;
    std::vector<double> deviation_ratios;
    std::size_t headers_correct = 0;
    std::size_t frames = 0;
    std::size_t ended = 0;
    std::size_t voice_bits_compared = 0;
    std::size_t voice_bit_errors = 0;
};

template <typename NextLength>
Result decode_blocked(std::span<const dsp::Complex32> samples, NextLength next) {
    decode::DStarConfig config;
    config.rate = kRate;
    auto decoder = decode::DStar::create(config);
    REQUIRE(decoder.has_value());

    std::vector<decode::DStarTransmission> records;
    std::size_t at = 0;
    while (at < samples.size()) {
        const std::size_t length = std::min<std::size_t>(next(), samples.size() - at);
        REQUIRE(decoder->process(samples.subspan(at, length), records).has_value());
        at += length;
    }

    Result result;
    for (const Transmission& t : gather(records)) {
        result.transmissions.push_back(digest(t));
        result.carrier_offsets_hz.push_back(t.first.carrier_offset_hz);
        result.deviation_ratios.push_back(t.first.deviation_ratio);
        result.frames += t.frames.size();
        result.ended += t.ended ? 1U : 0U;
        if (!t.first.header) {
            continue;
        }
        std::size_t index = kTransmissions;
        for (std::size_t i = 0; i < kTransmissions; ++i) {
            if (t.first.header->own_callsign == kCallsigns[i]) {
                index = i;
            }
        }
        if (index == kTransmissions || !t.first.header->fcs_valid) {
            continue;
        }
        ++result.headers_correct;
        const std::vector<VoiceBits> sent = voice(index);
        for (std::size_t f = 0; f < t.frames.size() && f < sent.size(); ++f) {
            for (std::size_t b = 0; b < decode::kDStarVoiceBits; ++b) {
                ++result.voice_bits_compared;
                result.voice_bit_errors += (t.frames[f].voice[b] != sent[f][b]) ? 1U : 0U;
            }
        }
    }
    return result;
}

void report(std::string_view blocking, const Result& result) {
    std::println(
        "test_dstar_blocking {}: {} transmissions, {} headers of {} correct, {} voice frames of "
        "{}, {} ended, {} voice bit errors in {}",
        blocking, result.transmissions.size(), result.headers_correct, kTransmissions,
        result.frames, kTransmissions * kVoiceFrames, result.ended, result.voice_bit_errors,
        result.voice_bits_compared);
}

}  // namespace

TEST_CASE("D-STAR decodes the same capture identically however it is blocked",
          "[decode][dstar]") {
    const std::vector<dsp::Complex32> samples = capture();
    const std::span<const dsp::Complex32> all(samples);

    const Result whole = decode_blocked(all, [&] { return samples.size(); });
    report("whole", whole);

    // It decodes at all, or identical output would be the trivial kind.
    CHECK(whole.headers_correct == kTransmissions);
    CHECK(whole.frames == kTransmissions * kVoiceFrames);
    CHECK(whole.ended == kTransmissions);
    CHECK(whole.voice_bit_errors == 0);

    struct Blocking {
        std::string name;
        std::size_t fixed;  // zero for random lengths
    };
    const Blocking blockings[] = {
        {"1024", 1'024},   {"2731", 2'731},   {"4096", 4'096},   {"10923", 10'923},
        {"16384", 16'384}, {"65536", 65'536}, {"random", 0},     {"one sample", 1},
    };

    std::println("test_dstar_blocking random lengths from seed {:#x}", kBlockingSeed);
    for (const Blocking& blocking : blockings) {
        std::mt19937_64 random(kBlockingSeed);
        std::uniform_int_distribution<std::size_t> length(1, 20'000);
        const Result blocked = decode_blocked(all, [&] {
            return blocking.fixed != 0 ? blocking.fixed : length(random);
        });
        report(blocking.name, blocked);

        INFO("blocking " << blocking.name);
        CHECK(blocked.transmissions.size() == whole.transmissions.size());
        for (std::size_t i = 0; i < std::min(whole.transmissions.size(), blocked.transmissions.size()); ++i) {
            INFO("transmission " << i << "\n  whole:   " << whole.transmissions[i]
                                 << "\n  blocked: " << blocked.transmissions[i]);
            CHECK(blocked.transmissions[i] == whole.transmissions[i]);
        }
    }
}

TEST_CASE("D-STAR slices a carrier 1000 Hz off from each frame sync's own fit",
          "[decode][dstar]") {
    // Most of a symbol's deviation: GMSK at 4800 bit/s deviates 1200 Hz. A
    // receiver that assumed the carrier sat on DC would slice most of the
    // zeros as ones. No baseband DC, so every transmission is expected whole.
    constexpr dsp::Hertz kOffsetHz = 1'000;
    const std::vector<dsp::Complex32> samples =
        capture(Condition{kOffsetHz, dsp::Complex32{}, true});
    const std::span<const dsp::Complex32> all(samples);

    const Result whole = decode_blocked(all, [&] { return samples.size(); });
    report("1000 Hz off, whole", whole);
    CHECK(whole.headers_correct == kTransmissions);
    CHECK(whole.frames == kTransmissions * kVoiceFrames);
    CHECK(whole.ended == kTransmissions);
    CHECK(whole.voice_bit_errors == 0);

    REQUIRE_FALSE(whole.carrier_offsets_hz.empty());
    const auto [low, high] = std::ranges::minmax(whole.carrier_offsets_hz);
    const auto [narrow, wide] = std::ranges::minmax(whole.deviation_ratios);
    std::println("test_dstar_blocking 1000 Hz off: frame syncs measured {:.1f} to {:.1f} Hz, "
                 "deviation {:.4f} to {:.4f} of nominal",
                 low, high, narrow, wide);
    // Measured 2026-09-23: 920.6 to 960.5 Hz, and a deviation of 0.646 to
    // 0.693 of nominal. Both read low for one reason: at BT 0.5 the Gaussian
    // filters at both ends leave a lone bit short of full deviation, the
    // frame sync is mostly lone bits, and a straight-line fit through levels
    // that are not straight reads that as a smaller gain and moves the offset
    // with it. The slicer only needs the offset well inside a bit's
    // deviation, and 80 Hz of 1200 is.
    CHECK(low > static_cast<double>(kOffsetHz) - 120.0);
    CHECK(high < static_cast<double>(kOffsetHz) + 120.0);
}
