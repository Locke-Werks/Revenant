// CwBand: every keyed tone in a receiver's audio, found wherever it is and
// decoded as a stream of its own, against the keyer in
// core/dsp/synth/cw_mod.h.
//
// What these cases hold is what a receiver parked in a CW segment meets: a
// tone at any pitch, two stations at once, a steady carrier, noise with
// nothing in it, a station that stops and another that starts, speech, and a
// hand sender. Error rates are measured and printed, and the bounds around
// them are loose, as everywhere in this directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <string>
#include <vector>

#include "core/decode/cw.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/cw_mod.h"
#include "core/dsp/synth/psk31_mod.h"
#include "tools/siggen/speech.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 48000;

// What a minute of speech may print. Measured on 2026-09-23: 747 characters
// in 6 streams before CwBand judged its streams' marks, 57 in 3 after.
constexpr std::size_t kSpeechCharacters = 100;

std::size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1,
                               diagonal + (a[i - 1] == b[j - 1] ? 0U : 1U)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

struct Stream {
    std::string text;
    std::string codes;
    double pitch_hz = 0.0;
    double wpm = 0.0;
    dsp::SampleIndex first_sample = 0;
};

struct BandResult {
    // By stream number, the text each printed and where it last said its
    // tone was.
    std::map<std::uint32_t, Stream> streams;
    std::size_t characters = 0;

    // The text of the stream whose pitch is nearest `pitch`, or empty.
    [[nodiscard]] const Stream* nearest(double pitch) const {
        const Stream* best = nullptr;
        for (const auto& [id, stream] : streams) {
            if (best == nullptr ||
                std::abs(stream.pitch_hz - pitch) < std::abs(best->pitch_hz - pitch)) {
                best = &stream;
            }
        }
        return best;
    }

    // All the text within `within` Hz of `pitch`, streams in the order they
    // started.
    [[nodiscard]] std::string text_near(double pitch, double within = 40.0) const {
        std::string out;
        for (const auto& [id, stream] : streams) {
            if (std::abs(stream.pitch_hz - pitch) <= within) {
                if (!out.empty()) {
                    out.push_back(' ');
                }
                out += stream.text;
            }
        }
        return out;
    }

    [[nodiscard]] std::string describe() const {
        std::string out;
        for (const auto& [id, stream] : streams) {
            out += "stream " + std::to_string(id) + " at " + std::to_string(stream.pitch_hz) +
                   " Hz, " + std::to_string(stream.wpm) + " WPM: '" + stream.text + "' as " +
                   stream.codes + "\n";
        }
        return out;
    }
};

BandResult decode_band(std::span<const float> audio, std::size_t block,
                       decode::CwBandConfig config = {}) {
    auto decoder = decode::CwBand::create(config);
    INFO((decoder.has_value() ? std::string{} : decoder.error().message));
    REQUIRE(decoder.has_value());
    std::vector<decode::CwCharacter> out;
    for (std::size_t start = 0; start < audio.size(); start += block) {
        const std::size_t length = std::min(block, audio.size() - start);
        REQUIRE(decoder->process(audio.subspan(start, length), out).has_value());
    }
    decoder->flush(out);
    BandResult result;
    for (const auto& c : out) {
        Stream& stream = result.streams[c.stream];
        if (stream.text.empty() && c.text == " ") {
            continue;
        }
        if (stream.text.empty()) {
            stream.first_sample = c.first_sample;
        }
        stream.text += c.recognised ? c.text : std::string("#");
        stream.codes += (c.text == " " ? std::string("/") : c.code) + " ";
        stream.pitch_hz = c.pitch_hz;
        stream.wpm = c.wpm;
        ++result.characters;
    }
    for (auto& [id, stream] : result.streams) {
        while (!stream.text.empty() && stream.text.back() == ' ') {
            stream.text.pop_back();
        }
    }
    return result;
}

// The analytic keyed tone, before any noise.
std::vector<siggen::Complex32> keyed(double tone_hz, double wpm, const std::string& text,
                                     double lead = 0.5, double tail = 1.0, double jitter = 0.0,
                                     std::uint64_t seed = 1) {
    siggen::CwModConfig mod;
    mod.rate = kRate;
    mod.tone_hz = static_cast<dsp::Hertz>(std::lround(tone_hz));
    mod.wpm = wpm;
    mod.lead_in_seconds = lead;
    mod.tail_seconds = tail;
    mod.jitter = jitter;
    mod.seed = seed;
    auto analytic = siggen::cw_render_analytic(mod, text);
    INFO((analytic.has_value() ? std::string{} : analytic.error().message));
    REQUIRE(analytic.has_value());
    return *analytic;
}

// Audio at `snr` in 2500 Hz, the bench's convention: the signal's power over
// the whole render.
std::vector<float> noisy(const std::vector<siggen::Complex32>& analytic, double snr,
                         std::uint64_t seed) {
    auto audio = siggen::analytic_to_noisy_audio(analytic, snr, siggen::kWsjtxReferenceBandwidthHz,
                                                 kRate, seed);
    REQUIRE(audio.has_value());
    return *audio;
}

const std::string kText = "CQ CQ DE W7E W7E TEST K";

}  // namespace

TEST_CASE("CwBand finds a keyed tone anywhere in a sideband receiver's audio",
          "[decode][cw]") {
    for (const double pitch : {250.0, 300.0, 450.0, 700.0, 910.0, 1200.0, 1650.0, 2400.0}) {
        for (const double wpm : {12.0, 25.0, 40.0}) {
            const std::vector<float> audio = noisy(keyed(pitch, wpm, kText), 10.0, 0xBA0D);
            const BandResult got = decode_band(audio, 4800);
            INFO(pitch << " Hz at " << wpm << " WPM:\n" << got.describe());
            const Stream* stream = got.nearest(pitch);
            REQUIRE(stream != nullptr);
            CHECK(stream->text == kText);
            CHECK(std::abs(stream->pitch_hz - pitch) < 3.0);
            CHECK(std::abs(stream->wpm - wpm) < 0.05 * wpm);
            CHECK(got.streams.size() == 1);
        }
    }
}

namespace {

// Sums analytic signals that start at the given seconds, padding the ends.
std::vector<siggen::Complex32> mix(
    const std::vector<std::pair<double, std::vector<siggen::Complex32>>>& parts,
    double total_seconds = 0.0) {
    std::size_t length = static_cast<std::size_t>(total_seconds * kRate);
    for (const auto& [at, signal] : parts) {
        length = std::max(length, static_cast<std::size_t>(at * kRate) + signal.size());
    }
    std::vector<siggen::Complex32> out(length);
    for (const auto& [at, signal] : parts) {
        const auto offset = static_cast<std::size_t>(at * kRate);
        for (std::size_t n = 0; n < signal.size(); ++n) {
            out[offset + n] += signal[n];
        }
    }
    return out;
}

// Noise of a fixed power added to the real part, for cases whose signal
// power is not the reference.
std::vector<float> with_noise(const std::vector<siggen::Complex32>& analytic, double noise_power,
                              std::uint64_t seed) {
    std::vector<siggen::Complex32> copy = analytic;
    REQUIRE(siggen::add_awgn_at_power(copy, noise_power, seed).has_value());
    return siggen::analytic_to_audio(copy);
}

}  // namespace

TEST_CASE("CwBand decodes two stations keying at once as two streams", "[decode][cw]") {
    const std::string a = "CQ TEST DE K9BGL K9BGL TEST";
    const std::string b = "K9BGL DE W7E W7E 5NN CN88 K";
    for (const auto& [pa, pb] : {std::pair{620.0, 1080.0}, std::pair{540.0, 700.0}}) {
        const auto audio = with_noise(
            mix({{0.0, keyed(pa, 22.0, a, 1.0)}, {1.3, keyed(pb, 28.0, b, 1.0)}}), 0.02, 0x2A);
        const BandResult got = decode_band(audio, 4800);
        INFO(pa << " and " << pb << " Hz:\n" << got.describe());
        CHECK(got.text_near(pa) == a);
        CHECK(got.text_near(pb) == b);
        CHECK(got.streams.size() == 2);
    }
}

TEST_CASE("CwBand reads a conversation, each station at its own pitch", "[decode][cw]") {
    // Two stations taking turns two seconds apart, each off the other's
    // pitch by 170 Hz, which is what zero beating by ear leaves.
    const std::string a1 = "W7E DE K9BGL TU 5NN 04";
    const std::string b1 = "K9BGL DE W7E R TU 5NN CN88";
    const std::string a2 = "TU 73 DE K9BGL SK";
    const auto first = keyed(650.0, 25.0, a1, 0.5, 0.0);
    const double b_at = static_cast<double>(first.size()) / kRate + 2.0;
    const auto second = keyed(820.0, 25.0, b1, 0.0, 0.0);
    const double a_at = b_at + static_cast<double>(second.size()) / kRate + 2.0;
    const auto audio = with_noise(
        mix({{0.0, first}, {b_at, second}, {a_at, keyed(650.0, 25.0, a2, 0.0, 2.0)}}), 0.02, 0x3B);
    const BandResult got = decode_band(audio, 4800);
    INFO(got.describe());
    CHECK(got.text_near(650.0) == a1 + " " + a2);
    CHECK(got.text_near(820.0) == b1);
}

TEST_CASE("CwBand prints nothing from noise alone or from a steady carrier", "[decode][cw]") {
    std::vector<siggen::Complex32> silence(static_cast<std::size_t>(60 * kRate));
    const BandResult noise = decode_band(with_noise(silence, 1.0, 0xC0FF'0016ULL), 9600);
    INFO("noise:\n" << noise.describe());
    CHECK(noise.characters == 0);

    // A carrier 20 dB over the noise in 2500 Hz for a minute, which a keyed
    // tone would be decoded at, and on for the whole of it.
    std::vector<siggen::Complex32> carrier(static_cast<std::size_t>(60 * kRate));
    for (std::size_t n = 0; n < carrier.size(); ++n) {
        const double phase = 2.0 * std::numbers::pi * 910.0 * static_cast<double>(n) / kRate;
        carrier[n] = siggen::Complex32{static_cast<float>(std::cos(phase)),
                                       static_cast<float>(std::sin(phase))};
    }
    const auto audio = noisy(carrier, 20.0, 0xCA77);
    const BandResult steady = decode_band(audio, 9600);
    INFO("carrier:\n" << steady.describe());
    CHECK(steady.characters == 0);
}

TEST_CASE("CwBand falls silent after a transmission and finds the next one", "[decode][cw]") {
    // A station, a minute of noise, then another station at another pitch.
    // The first stream has to end without printing the noise, and the second
    // has to be found from nothing.
    const std::string a = "CQ CQ DE N0CALL N0CALL K";
    const std::string b = "QRZ DE G3PLX G3PLX K";
    const auto first = keyed(760.0, 20.0, a, 0.5, 0.0);
    const double b_at = static_cast<double>(first.size()) / kRate + 60.0;
    const auto audio =
        with_noise(mix({{0.0, first}, {b_at, keyed(1240.0, 30.0, b, 0.0, 3.0)}}), 0.05, 0x5117);
    const BandResult got = decode_band(audio, 4800);
    INFO(got.describe());
    CHECK(got.text_near(760.0) == a);
    CHECK(got.text_near(1240.0) == b);
    CHECK(got.streams.size() == 2);
}

TEST_CASE("CwBand follows a tone that drifts", "[decode][cw]") {
    // Twenty hertz across a transmission of about forty seconds, a drift a
    // cold transmitter can show. The stream starts where the tone was.
    const std::string text = "CQ CQ CQ DE W7GKF W7GKF W7GKF K PSE K";
    auto analytic = keyed(800.0, 15.0, text);
    const double seconds = static_cast<double>(analytic.size()) / kRate;
    const double slope = 20.0 / seconds;  // hertz a second
    for (std::size_t n = 0; n < analytic.size(); ++n) {
        const double t = static_cast<double>(n) / kRate;
        const double phase = std::numbers::pi * slope * t * t;
        analytic[n] *= siggen::Complex32{static_cast<float>(std::cos(phase)),
                                         static_cast<float>(std::sin(phase))};
    }
    const BandResult got = decode_band(noisy(analytic, 5.0, 0xD41F7), 4800);
    INFO(seconds << " s:\n" << got.describe());
    CHECK(got.text_near(810.0, 30.0) == text);
    const Stream* stream = got.nearest(820.0);
    REQUIRE(stream != nullptr);
    CHECK(std::abs(stream->pitch_hz - 820.0) < 3.0);
}

TEST_CASE("CwBand prints little from speech on a sideband receiver", "[decode][cw]") {
    // A minute of speech-shaped audio, tools/siggen/speech.h, 300 to 3000 Hz
    // at 20 dB over the noise: what a usb receiver hears parked on a talker.
    // Voiced syllables are harmonics that come and go at a syllable's rate,
    // which is on and off at a Morse-like rate in whichever bins they cross.
    siggen_speech::SpeechSpec spec;
    spec.rate = kRate;
    spec.samples = static_cast<std::size_t>(60 * kRate);
    spec.seed = 0x5EECULL;
    const siggen_speech::Speech speech = siggen_speech::synthesise_speech(spec);
    std::vector<siggen::Complex32> analytic(speech.audio.size());
    for (std::size_t n = 0; n < analytic.size(); ++n) {
        analytic[n] = siggen::Complex32{speech.audio[n], 0.0F};
    }
    const BandResult got = decode_band(noisy(analytic, 20.0, 0x5EED), 4800);
    INFO(got.describe());
    WARN("CW on a minute of speech: " << got.characters << " characters in "
                                      << got.streams.size() << " streams");
    CHECK(got.characters <= kSpeechCharacters);
}

TEST_CASE("CwBand reads a hand sender, and how it degrades", "[decode][cw]") {
    // cw_mod.h's jitter stretches every element and space by a seeded random
    // fraction; a hand sender's spread is of the order of the larger ones.
    const std::string text = "CQ CQ DE G3PLX G3PLX K GM OM TNX FER CALL UR RST 579 579";
    for (const double jitter : {0.05, 0.10, 0.15, 0.20}) {
        std::size_t errors = 0;
        std::size_t sent = 0;
        for (std::uint64_t seed = 1; seed <= 4; ++seed) {
            const auto audio =
                noisy(keyed(740.0, 18.0, text, 0.5, 1.0, jitter, seed), 10.0, 0x4A00 + seed);
            const BandResult got = decode_band(audio, 4800);
            errors += std::min(edit_distance(text, got.text_near(740.0)), text.size());
            sent += text.size();
        }
        const double cer = static_cast<double>(errors) / static_cast<double>(sent);
        WARN("CW hand sent at 18 WPM, jitter " << jitter << ", +10 dB: CER " << cer);
        if (jitter <= 0.10) {
            CHECK(cer <= 0.05);
        }
        CHECK(cer <= 1.0);
    }
}

TEST_CASE("CwBand does not depend on how its input is blocked", "[decode][cw]") {
    const auto audio = with_noise(
        mix({{0.0, keyed(610.0, 24.0, kText, 1.0)}, {2.0, keyed(1400.0, 32.0, kText, 1.0)}}), 0.05,
        0xB10C);
    const BandResult whole = decode_band(audio, audio.size());
    const BandResult small = decode_band(audio, 777);
    INFO("whole:\n" << whole.describe() << "small:\n" << small.describe());
    REQUIRE(whole.streams.size() == small.streams.size());
    for (const auto& [id, stream] : whole.streams) {
        REQUIRE(small.streams.contains(id));
        CHECK(small.streams.at(id).text == stream.text);
        CHECK(small.streams.at(id).pitch_hz == stream.pitch_hz);
    }
}

