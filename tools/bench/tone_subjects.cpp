// PSK31, PSK63, QPSK31 and CW: the modes read from receiver audio through
// core/decode/tone_frontend.h, each through its transmitter in
// core/dsp/synth and analytic_to_noisy_audio, which calibrates the audio's
// SNR in 2500 Hz rather than the analytic signal's; psk31_mod.h says why the
// two differ by 3 dB.
//
// Settings are those of the error-rate cases in tests/decode/test_psk31.cpp
// and test_cw.cpp: the tone a few hertz off the receiver's centre, so the AFC
// is in the measurement.

// Floating-point discipline first, for the reason sweep.cpp gives.
#include "core/dsp/reference_fp.h"

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "core/decode/cw.h"
#include "core/decode/psk31.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/cw_mod.h"
#include "core/dsp/synth/psk31_mod.h"
#include "tools/bench/mode_support.h"

namespace revenant::bench::detail {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "tools/bench/tone_subjects.cpp must include core/dsp/reference_fp.h first");

namespace {

constexpr dsp::SampleRate kAudioRate = 48000;

// ---------------------------------------------------------------------------
// PSK31, PSK63 and QPSK31
// ---------------------------------------------------------------------------

// Lower case letters and spaces, the text PSK31 carries most, as the test
// draws it, so the character error rate reflects the code lengths that
// dominate on air.
constexpr std::string_view kPskPool = "etaoin shrdlu cmfwyp vbgkqjxz ";

// 8 Hz off the receiver's 1500 Hz centre, the test's tuning error.
constexpr dsp::Hertz kPskToneHz = 1508;
constexpr dsp::Hertz kPskCentreHz = 1500;

ModeSubject psk(decode::Psk31Mode kind, std::string_view name, std::string_view description, double start,
                double stop, std::uint64_t trials) {
    ModeSubject mode;
    mode.mode = std::string(name);
    mode.subject = std::format("{} at 48000 S/s, tone 8 Hz off a 1500 Hz centre; character error rate against "
                               "SNR in 2500 Hz",
                               description);
    mode.unit = "character";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 100;
    mode.minimum_payload_bytes = 16;
    mode.snr_start_db = start;
    mode.snr_stop_db = stop;
    mode.trials = trials;
    mode.generator = [kind](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        siggen::Psk31ModConfig mod;
        mod.rate = kAudioRate;
        mod.tone_hz = kPskToneHz;
        mod.mode = kind;
        auto bits = siggen::psk31_message_bits(mod, text_from_payload(payload, kPskPool));
        if (!bits) {
            return std::vector<dsp::Complex32>{};
        }
        auto analytic = siggen::psk31_render_analytic(mod, *bits);
        if (!analytic) {
            return std::vector<dsp::Complex32>{};
        }
        auto audio = siggen::analytic_to_noisy_audio(*analytic, snr_db, siggen::kWsjtxReferenceBandwidthHz,
                                                     kAudioRate, seed);
        if (!audio) {
            return std::vector<dsp::Complex32>{};
        }
        return audio_as_baseband(*audio);
    };
    mode.score = [kind](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const std::string sent = text_from_payload(payload, kPskPool);
        decode::Psk31Config config;
        config.rate = kAudioRate;
        config.centre_hz = kPskCentreHz;
        config.mode = kind;
        auto decoder = decode::Psk31::create(config);
        if (samples.empty() || !decoder) {
            return score_text(sent, "");
        }
        const std::vector<float> audio = real_part(samples);
        std::vector<decode::Psk31Character> characters;
        if (!decoder->process(dsp::ConstRealSpan(audio), characters)) {
            return score_text(sent, "");
        }
        std::string got;
        for (const decode::Psk31Character& character : characters) {
            if (character.recognised) {
                got.push_back(static_cast<char>(character.ascii));
            }
        }
        return score_text(sent, got);
    };
    mode.truth = [](std::span<const std::uint8_t> payload) { return text_from_payload(payload, kPskPool) + "\n"; };
    return mode;
}

// ---------------------------------------------------------------------------
// CW
// ---------------------------------------------------------------------------

// Letters and figures with words of varying length. A payload byte picks a
// character; one byte in seven asks for a word space, taken only where the
// text has a character on both sides of it, because a space at either end or
// two together is not something a Morse decoder can print.
constexpr std::string_view kCwCharacters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

std::string cw_text(std::span<const std::uint8_t> payload) {
    std::string text;
    for (const std::uint8_t byte : payload) {
        const bool space = byte % 7 == 0;
        if (space && !text.empty() && text.back() != ' ') {
            text.push_back(' ');
        } else {
            text.push_back(kCwCharacters[(byte / 7U) % kCwCharacters.size()]);
        }
    }
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

// 20 WPM, standard spacing, tone 20 Hz above the decoder's 700 Hz default
// centre: the test's middle speed and its tuning.
constexpr double kCwWpm = 20.0;
constexpr dsp::Hertz kCwToneHz = 720;

ModeSubject cw() {
    ModeSubject mode;
    mode.mode = "cw";
    mode.subject = "cw 20 WPM standard spacing at 48000 S/s, tone 20 Hz off a 700 Hz centre; character error rate "
                   "against SNR in 2500 Hz";
    mode.unit = "character";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 60;
    mode.minimum_payload_bytes = 16;
    // Read at 0.01 like the other character modes. It was read at 0.05 while
    // the curve had a floor near 0.01 from -5 dB up, which docs/sensitivity.md
    // records with the three causes and the commits that removed them.
    mode.snr_start_db = -13.0;
    mode.snr_stop_db = 3.0;
    mode.trials = 256;
    mode.generator = [](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        siggen::CwModConfig mod;
        mod.rate = kAudioRate;
        mod.tone_hz = kCwToneHz;
        mod.wpm = kCwWpm;
        auto analytic = siggen::cw_render_analytic(mod, cw_text(payload));
        if (!analytic) {
            return std::vector<dsp::Complex32>{};
        }
        auto audio = siggen::analytic_to_noisy_audio(*analytic, snr_db, siggen::kWsjtxReferenceBandwidthHz,
                                                     kAudioRate, seed);
        if (!audio) {
            return std::vector<dsp::Complex32>{};
        }
        return audio_as_baseband(*audio);
    };
    mode.score = [](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const std::string sent = cw_text(payload);
        decode::CwConfig config;
        config.rate = kAudioRate;
        auto decoder = decode::Cw::create(config);
        if (samples.empty() || !decoder) {
            return score_text(sent, "");
        }
        const std::vector<float> audio = real_part(samples);
        std::vector<decode::CwCharacter> characters;
        if (!decoder->process(dsp::ConstRealSpan(audio), characters)) {
            return score_text(sent, "");
        }
        decoder->flush(characters);
        std::string got;
        for (const decode::CwCharacter& character : characters) {
            got += character.recognised ? character.text : std::string("#");
        }
        return score_text(sent, got);
    };
    mode.truth = [](std::span<const std::uint8_t> payload) { return cw_text(payload) + "\n"; };
    return mode;
}

}  // namespace

Expected<ModeSubject> make_tone_subject(std::string_view mode) {
    // QPSK31 at 256 trials rather than 512 because its Viterbi decoder makes
    // a failing trial several times dearer than a BPSK one, and its curve
    // falls from all lost to 0.01 in 2 dB, so the extra trials would buy
    // little where they cost most.
    if (mode == "psk31") {
        return psk(decode::Psk31Mode::Bpsk31, mode, "bpsk31", -15.0, -3.0, 512);
    }
    if (mode == "psk63") {
        return psk(decode::Psk31Mode::Bpsk63, mode, "bpsk63", -13.0, -1.0, 512);
    }
    if (mode == "qpsk31") {
        return psk(decode::Psk31Mode::Qpsk31, mode, "qpsk31", -14.0, -4.0, 256);
    }
    if (mode == "cw") {
        return cw();
    }
    return fail(std::format("'{}' is not a tone mode", mode));
}

}  // namespace revenant::bench::detail
