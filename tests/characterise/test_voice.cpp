// Voice through the characteriser, one probe's extract at a time.
//
// WHAT THIS FILE IS THE RECORD OF
//
// tests/detect/test_voice_survey.cpp runs tools/siggen/voice.h's scene through
// the engine and finds what the span's labels say about speech. This is the
// half of that which needs no device: each modulation carrying
// tools/siggen/speech.h's speech, rendered at a probe bucket's rate, centred
// the way detect::TierTwo centres a probe on it, put in noise at an SNR in
// 2500 Hz, narrowed to the half of the bucket core/engine/probe.h passes, and
// characterised over the two seconds the pool reads. The controls are the
// signals a voice rule must not claim: a keyed carrier, a bare one, BPSK,
// PSK31, 2FSK and noise.
//
// The narrowing is a windowed sinc here, not the fine stage's polyphase
// filter, so a number here is the characteriser's answer to an extract shaped
// like a probe's rather than to a probe's own. The survey in tests/detect is
// the end-to-end record.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <print>
#include <string>
#include <vector>

#include "core/characterise/characterise.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/psk31_mod.h"
#include "tests/characterise/signal_lab.h"
#include "tools/siggen/speech.h"

using namespace revenant;
using characterise::ModulationFamily;

namespace {

constexpr std::uint64_t kSeed = 20260924;

// The pool's two-second dwell.
constexpr double kDwellSeconds = 2.0;

[[nodiscard]] std::vector<float> speech_at(dsp::SampleRate rate, std::size_t count,
                                           std::uint64_t seed) {
    siggen_speech::SpeechSpec spec;
    spec.rate = rate;
    spec.samples = count;
    spec.seed = seed;
    return siggen_speech::synthesise_speech(spec).audio;
}

// Noise at an SNR in 2500 Hz, then a low pass at a quarter of the rate, which
// is the half of the bucket a probe passes.
[[nodiscard]] std::vector<dsp::Complex32> probe_like(std::vector<dsp::Complex32> clean,
                                                     dsp::SampleRate rate, double snr_db,
                                                     std::uint64_t seed) {
    if (std::isfinite(snr_db)) {
        auto added = siggen::add_awgn(dsp::ComplexSpan(clean),
                                      siggen::NoiseLevel::snr_in_2500_hz_db(snr_db), rate, seed);
        REQUIRE(added.has_value());
    }
    constexpr int kHalf = 64;
    std::vector<double> taps(2 * kHalf + 1);
    double sum = 0.0;
    for (int k = -kHalf; k <= kHalf; ++k) {
        const double x = 2.0 * 0.25 * static_cast<double>(k);
        const double sinc = k == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        const double window = 0.54 + 0.46 * std::cos(std::numbers::pi * k / kHalf);
        taps[static_cast<std::size_t>(k + kHalf)] = sinc * window;
        sum += sinc * window;
    }
    std::vector<dsp::Complex32> out(clean.size());
    for (std::size_t n = 0; n < clean.size(); ++n) {
        std::complex<double> acc(0.0, 0.0);
        for (int k = -kHalf; k <= kHalf; ++k) {
            const auto j = static_cast<std::ptrdiff_t>(n) + k;
            if (j < 0 || j >= static_cast<std::ptrdiff_t>(clean.size())) {
                continue;
            }
            acc += taps[static_cast<std::size_t>(k + kHalf)] / sum *
                   std::complex<double>(clean[static_cast<std::size_t>(j)]);
        }
        out[n] = dsp::Complex32(static_cast<float>(acc.real()), static_cast<float>(acc.imag()));
    }
    return out;
}

struct Emitter {
    const char* name;
    // The width detect::TierTwo would hand the probe as the detection's.
    double detection_hz;
    std::function<std::vector<dsp::Complex32>(dsp::SampleRate, std::size_t, std::uint64_t)> make;
};

[[nodiscard]] std::vector<dsp::Complex32> must(Expected<siggen::GeneratedSignal> made) {
    REQUIRE(made.has_value());
    return std::move(made->samples);
}

[[nodiscard]] std::vector<Emitter> emitters() {
    std::vector<Emitter> out;
    out.push_back({"am", 7'000.0, [](dsp::SampleRate rate, std::size_t count, std::uint64_t seed) {
                       siggen::ModulatorConfig common;
                       common.rate = rate;
                       common.seed = seed;
                       siggen::AmParams am;
                       am.modulation_index = 0.8;
                       return must(siggen::generate_am(common, am, count, speech_at(rate, count, seed + 1)));
                   }});
    out.push_back({"nfm2.5", 11'000.0, [](dsp::SampleRate rate, std::size_t count, std::uint64_t seed) {
                       siggen::ModulatorConfig common;
                       common.rate = rate;
                       common.seed = seed;
                       siggen::NfmParams nfm;
                       nfm.deviation = 2'500;
                       return must(siggen::generate_nfm(common, nfm, count, speech_at(rate, count, seed + 2)));
                   }});
    out.push_back({"nfm5", 12'000.0, [](dsp::SampleRate rate, std::size_t count, std::uint64_t seed) {
                       siggen::ModulatorConfig common;
                       common.rate = rate;
                       common.seed = seed;
                       siggen::NfmParams nfm;
                       nfm.deviation = 5'000;
                       return must(siggen::generate_nfm(common, nfm, count, speech_at(rate, count, seed + 3)));
                   }});
    out.push_back({"usb", 3'000.0, [](dsp::SampleRate rate, std::size_t count, std::uint64_t seed) {
                       siggen::ModulatorConfig common;
                       common.rate = rate;
                       common.seed = seed;
                       common.carrier_offset = -1'650;
                       siggen::SsbParams ssb;
                       return must(siggen::generate_ssb(common, ssb, true, count,
                                                        speech_at(rate, count, seed + 4)));
                   }});
    out.push_back({"lsb", 3'000.0, [](dsp::SampleRate rate, std::size_t count, std::uint64_t seed) {
                       siggen::ModulatorConfig common;
                       common.rate = rate;
                       common.seed = seed;
                       common.carrier_offset = 1'650;
                       siggen::SsbParams ssb;
                       return must(siggen::generate_ssb(common, ssb, false, count,
                                                        speech_at(rate, count, seed + 5)));
                   }});
    out.push_back({"cw", 180.0, [](dsp::SampleRate rate, std::size_t count, std::uint64_t seed) {
                       siggen::ModulatorConfig common;
                       common.rate = rate;
                       common.seed = seed;
                       siggen::CwParams cw;
                       cw.words_per_minute = 20.0;
                       return must(siggen::generate_cw(common, cw, count));
                   }});
    out.push_back({"carrier", 180.0, [](dsp::SampleRate, std::size_t count, std::uint64_t) {
                       return std::vector<dsp::Complex32>(count, dsp::Complex32(1.0F, 0.0F));
                   }});
    out.push_back({"bpsk", 1'450.0, [](dsp::SampleRate rate, std::size_t count, std::uint64_t seed) {
                       siggen::ModulatorConfig common;
                       common.rate = rate;
                       common.seed = seed;
                       siggen::PskParams psk;
                       psk.symbol_rate = 1200.0;
                       psk.symbol_count = static_cast<std::size_t>(
                           1200.0 * static_cast<double>(count) / static_cast<double>(rate)) + 16;
                       return must(siggen::generate_bpsk(common, psk, count));
                   }});
    out.push_back({"fsk2", 3'600.0, [](dsp::SampleRate rate, std::size_t count, std::uint64_t seed) {
                       siggen::ModulatorConfig common;
                       common.rate = rate;
                       common.seed = seed;
                       siggen::Fsk2Params fsk;
                       fsk.symbol_rate = 1200.0;
                       fsk.deviation = 1'000;
                       fsk.symbol_count = static_cast<std::size_t>(
                           1200.0 * static_cast<double>(count) / static_cast<double>(rate)) + 16;
                       return must(siggen::generate_fsk2(common, fsk, count));
                   }});
    out.push_back({"psk31", 60.0, [](dsp::SampleRate rate, std::size_t count, std::uint64_t) {
                       siggen::Psk31ModConfig mod;
                       mod.rate = rate;
                       mod.tone_hz = 1000;
                       mod.preamble_symbols = 16;
                       auto bits = siggen::psk31_message_bits(
                           mod, "cq cq de n0call n0call pse k the quick brown fox ");
                       REQUIRE(bits.has_value());
                       auto analytic = siggen::psk31_render_analytic(mod, *bits);
                       REQUIRE(analytic.has_value());
                       std::vector<dsp::Complex32> out(count);
                       const double step = -2.0 * std::numbers::pi * 1000.0 / static_cast<double>(rate);
                       for (std::size_t n = 0; n < count; ++n) {
                           const auto sample = (*analytic)[n % analytic->size()];
                           out[n] = sample * std::polar(static_cast<float>(1.0),
                                                        static_cast<float>(std::fmod(
                                                            step * static_cast<double>(n),
                                                            2.0 * std::numbers::pi)));
                       }
                       return out;
                   }});
    out.push_back({"noise", 3'000.0, [](dsp::SampleRate, std::size_t count, std::uint64_t) {
                       return std::vector<dsp::Complex32>(count, dsp::Complex32(0.0F, 0.0F));
                   }});
    return out;
}

[[nodiscard]] characterise::Characterisation characterise_at(const std::vector<dsp::Complex32>& samples,
                                                             dsp::SampleRate rate, double detection_hz) {
    characterise::CharacteriseConfig config;
    config.rate = rate;
    config.detection_bandwidth_hz = detection_hz;
    auto result = characterise::characterise(dsp::ConstComplexSpan(samples), config);
    REQUIRE(result.has_value());
    return *result;
}

// One emitter's extract, by name, at a rate and a level, the way the survey
// below makes it.
[[nodiscard]] characterise::Characterisation extract_of(const char* name, dsp::SampleRate rate,
                                                        double level, std::uint64_t draw) {
    const auto count = static_cast<std::size_t>(kDwellSeconds * static_cast<double>(rate));
    const std::uint64_t seed = kSeed + 1000 * draw;
    for (const Emitter& emitter : emitters()) {
        if (std::string(emitter.name) != name) {
            continue;
        }
        const bool silent = std::string(emitter.name) == "noise";
        auto extract =
            silent ? probe_like(characterise_test::gaussian_noise(count, 1.0, seed + 7), rate,
                                std::nan(""), seed)
                   : probe_like(emitter.make(rate, count, seed), rate, level, seed + 7);
        return characterise_at(extract, rate, emitter.detection_hz);
    }
    FAIL("no emitter named " << name);
    return {};
}

}  // namespace

// REJECTS: a PSK call on a talker. Speech on a suppressed carrier lights the
// PSK branch at its pitch, 90 to 125 baud, which the symbol-rate rule lets
// through on a detection kilohertz wide; and it names no side.
TEST_CASE("speech on single sideband is a talker on its own side, and no family",
          "[characterise][voice]") {
    for (const dsp::SampleRate rate : {12'000, 24'000}) {
        for (const double level : {30.0, 10.0}) {
            for (const char* name : {"usb", "lsb"}) {
                const auto result = extract_of(name, rate, level, 0);
                INFO(name << " at " << rate << " S/s and " << level << " dB: " << result.summary);
                CHECK(result.voice);
                CHECK(result.family == ModulationFamily::Unknown);
                CHECK_FALSE(characterise::may_drive_detection(result));
                CHECK(result.voice_sideband == (std::string(name) == "usb"
                                                    ? characterise::VoiceSideband::Upper
                                                    : characterise::VoiceSideband::Lower));
            }
        }
    }
}

// REJECTS: the spectral sideband reading this replaced, which called a keyed
// carrier AM at 10 dB, and one that cannot tell AM from FM on speech.
TEST_CASE("speech on AM reads in phase and on FM in quadrature, and a carrier reads neither",
          "[characterise][voice]") {
    for (const double level : {30.0, 20.0, 10.0}) {
        const auto am = extract_of("am", 12'000, level, 0);
        INFO("am at " << level << " dB: " << am.summary);
        CHECK(am.family == ModulationFamily::Unmodulated);
        CHECK(am.double_sideband);

        const auto fm = extract_of("nfm2.5", 12'000, level, 0);
        INFO("nfm2.5 at " << level << " dB: " << fm.summary);
        CHECK(fm.family == ModulationFamily::AnalogueFm);
        CHECK_FALSE(fm.double_sideband);

        for (const char* name : {"cw", "carrier"}) {
            const auto carrier = extract_of(name, 12'000, level, 0);
            INFO(name << " at " << level << " dB: " << carrier.summary);
            CHECK(carrier.family == ModulationFamily::Unmodulated);
            CHECK_FALSE(carrier.double_sideband);
            CHECK_FALSE(carrier.low_index_fm);
            CHECK_FALSE(carrier.voice);
        }
    }
}

// REJECTS: 5 kHz deviation FM on speech called unknown or PSK, which is what
// it was before: its carrier holds under half its power, its deviation comes
// and goes with the talker, and the old analogue FM branch wanted the
// instantaneous frequency spread across a tenth of the band all the time.
// Which of the two new readings names it depends on the level: at 30 and
// 20 dB its carrier still holds a third of the extract and its sidebands read
// in quadrature, and at 10 dB the deviation's syllables do.
TEST_CASE("speech on wide deviation FM is analogue FM", "[characterise][voice]") {
    for (const double level : {30.0, 20.0, 10.0}) {
        const auto fm = extract_of("nfm5", 12'000, level, 0);
        INFO("nfm5 at " << level << " dB: " << fm.summary);
        CHECK(fm.family == ModulationFamily::AnalogueFm);
        CHECK((fm.voice || fm.low_index_fm));
        CHECK_FALSE(fm.double_sideband);
    }
}

// REJECTS: a voice rule that claims data or noise. None of these is a talker
// at any level or either rate.
TEST_CASE("the voice rules claim no data mode and no noise", "[characterise][voice]") {
    for (const dsp::SampleRate rate : {12'000, 24'000}) {
        for (const double level : {30.0, 20.0, 10.0, 5.0}) {
            for (const char* name : {"cw", "carrier", "bpsk", "fsk2", "psk31", "noise"}) {
                const auto result = extract_of(name, rate, level, 0);
                INFO(name << " at " << rate << " S/s and " << level << " dB: " << result.summary);
                CHECK_FALSE(result.voice);
            }
        }
    }
}

TEST_CASE("voice survey: what the characteriser says about speech", "[.voice-characterise]") {
    const dsp::SampleRate rates[] = {12'000, 24'000, 48'000};
    const double levels[] = {30.0, 20.0, 10.0, 5.0};
    for (const dsp::SampleRate rate : rates) {
        const auto count = static_cast<std::size_t>(kDwellSeconds * static_cast<double>(rate));
        std::println("at {} S/s:", rate);
        for (const Emitter& emitter : emitters()) {
            for (const double level : levels) {
                for (std::uint64_t draw = 0; draw < 2; ++draw) {
                    const std::uint64_t seed = kSeed + 1000 * draw;
                    const bool silent = std::string(emitter.name) == "noise";
                    auto extract =
                        silent ? probe_like(characterise_test::gaussian_noise(count, 1.0, seed + 7),
                                            rate, std::nan(""), seed)
                               : probe_like(emitter.make(rate, count, seed), rate, level, seed + 7);
                    const auto result = characterise_at(extract, rate, emitter.detection_hz);
                    const double noise = result.inband_noise;
                    const double signal = std::max(result.envelope.mean_power - noise, 0.0);
                    const double total = signal + noise;
                    const double net = result.envelope.normalised_power_variance -
                                       (total > 0.0 ? (2.0 * signal * noise + noise * noise) /
                                                          (total * total)
                                                    : 0.0);
                    std::println(
                        "  {:<7} {:>4.0f} dB #{}  {:<12} {:.2f} d{:d} r{:>6.1f} conc {:.3f} "
                        "env {:.3f} net {:+.3f} dsb {:d} lfm {:d} iq {:+.3f}/{:+.4f} syl {:.3f} "
                        "fast {:.3f} fsyl {:.3f} side {:+.3f} voice {:d}{}",
                        emitter.name, level, draw,
                        characterise::modulation_family_name(result.family),
                        result.family_confidence, characterise::may_drive_detection(result),
                        result.symbol_rate.found ? result.symbol_rate.symbol_rate_hz : 0.0,
                        result.spectral_concentration, result.envelope.normalised_power_variance,
                        net, result.double_sideband, result.low_index_fm,
                        result.carrier_iq_balance, result.carrier_in_phase_excess,
                        result.syllabic_depth, result.syllabic_fast,
                        result.frequency_syllabic_depth, result.side_centroid, result.voice,
                        result.voice_sideband == characterise::VoiceSideband::Upper   ? " usb"
                        : result.voice_sideband == characterise::VoiceSideband::Lower ? " lsb"
                                                                                      : "");
                }
            }
        }
    }
}
