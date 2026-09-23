// The two consistency rules of 2026-09-23: a PSK call that is two tones, and a
// family keyed faster than its detection is wide.
//
// WHAT THIS FILE IS THE RECORD OF
//
// docs/detection.md's probe survey found two wrong calls the characteriser
// made confidently and characterise::may_drive_detection accepted. Two-tone
// SSB came back PSK of order 2 at 1200.1 baud and 1.00, at every level. And
// every NFM and SSB line the detector reports separately came back PSK or FSK
// at 1000 or 2000 baud, the modulating tone and its double, from a probe sized
// to a track 146 to 183 Hz wide. The owner took both fixes. This file measures
// each rule either side of its bar and pins that the real families it must not
// touch still come through.
//
// Everything at 12000 S/s, the probe pool's floor bucket, which is where the
// survey's wrong calls were made.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <print>
#include <utility>
#include <vector>

#include "core/characterise/characterise.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"

using namespace revenant;
using characterise::ModulationFamily;

namespace {

constexpr std::uint64_t kSeed = 20260923;
constexpr dsp::SampleRate kRate = 12000;

// Two seconds, the probe pool's dwell at this bucket.
constexpr std::size_t kSamples = 24000;

[[nodiscard]] std::vector<dsp::Complex32> in_noise(std::vector<dsp::Complex32> clean,
                                                   double snr_2500_db, std::uint64_t seed)
{
    auto added = siggen::add_awgn(dsp::ComplexSpan(clean),
                                  siggen::NoiseLevel::snr_in_2500_hz_db(snr_2500_db), kRate, seed);
    REQUIRE(added.has_value());
    return clean;
}

[[nodiscard]] std::vector<dsp::Complex32> two_tone(bool upper, std::uint64_t seed)
{
    siggen::ModulatorConfig common;
    common.rate = kRate;
    common.seed = seed;
    siggen::SsbParams ssb;
    ssb.tone_hz = 700;
    ssb.tone2_hz = 1900;
    // The emitter the probe survey used sits at its own carrier; a probe
    // centred on the detection puts the middle of the two tones at DC.
    common.carrier_offset = upper ? -1300 : 1300;
    auto made = siggen::generate_ssb(common, ssb, upper, kSamples);
    REQUIRE(made.has_value());
    return made->samples;
}

[[nodiscard]] std::vector<dsp::Complex32> psk(bool quadrature, std::uint64_t seed)
{
    siggen::ModulatorConfig common;
    common.rate = kRate;
    common.seed = seed;
    siggen::PskParams params;
    params.symbol_rate = 1200.0;
    params.rolloff = 0.35;
    params.symbol_count = 4096;
    auto made = quadrature ? siggen::generate_qpsk(common, params, kSamples)
                           : siggen::generate_bpsk(common, params, kSamples);
    REQUIRE(made.has_value());
    return made->samples;
}

[[nodiscard]] characterise::Characterisation run(const std::vector<dsp::Complex32>& samples,
                                                 double detection_hz = 0.0)
{
    characterise::CharacteriseConfig config;
    config.rate = kRate;
    config.detection_bandwidth_hz = detection_hz;
    auto result = characterise::characterise(dsp::ConstComplexSpan(samples), config);
    REQUIRE(result.has_value());
    return *result;
}

void print_row(const char* label, double level, const characterise::Characterisation& result)
{
    std::println("  {:<6} {:>5.1f} dB  {:<20} conf {:.2f}  rate {:>7.1f}  share {:.3f}  third "
                 "{:.3f}  spacing {:>7.1f} Hz  pair {}",
                 label, level, characterise::modulation_family_name(result.family),
                 result.family_confidence,
                 result.symbol_rate.found ? result.symbol_rate.symbol_rate_hz : 0.0,
                 result.tone_pair_share, result.tone_pair_third, result.tone_pair_spacing_hz,
                 result.psk_tone_pair);
}

}  // namespace

// REJECTS: a PSK branch that trusts a symbol clock without looking at the
// spectrum. Two tones pass every test the branch makes, and the only thing
// that tells them from a keyed carrier is that their power is in two lines.
TEST_CASE("two tones are two carriers and not PSK, at every level", "[characterise]")
{
    std::println("test_consistency two tones: seed {}", kSeed);
    const double levels[] = {30.0, 20.0, 10.0, 5.0};
    for (const bool upper : {true, false}) {
        for (const double level : levels) {
            const auto samples =
                in_noise(two_tone(upper, kSeed), level, kSeed + static_cast<std::uint64_t>(level));
            const auto result = run(samples);
            print_row(upper ? "usb" : "lsb", level, result);
            INFO((upper ? "usb" : "lsb") << " at " << level << " dB: " << result.summary);
            CHECK(result.family != ModulationFamily::Psk);
            CHECK_FALSE(characterise::may_drive_detection(result));
        }
    }
}

// REJECTS: a tone-pair rule loose enough to take real PSK with it. The same
// two windows on a filled band hold a small share of its power, and the rule
// has to leave every one of these as PSK with its rate.
TEST_CASE("real BPSK and QPSK are untouched by the tone-pair rule", "[characterise]")
{
    const double levels[] = {30.0, 20.0, 10.0};
    for (const bool quadrature : {false, true}) {
        for (const double level : levels) {
            const auto samples = in_noise(psk(quadrature, kSeed + 1), level,
                                          kSeed + 100 + static_cast<std::uint64_t>(level));
            const auto result = run(samples);
            print_row(quadrature ? "qpsk" : "bpsk", level, result);
            INFO(result.summary);
            CHECK(result.family == ModulationFamily::Psk);
            CHECK_FALSE(result.psk_tone_pair);
            CHECK(result.tone_pair_share < 0.5);
            // BPSK at 10 dB loses its rate before its order, which is the
            // owner's flagged-and-capped case and not this rule's; see
            // kPskWithoutRateConfidence.
            CHECK(characterise::may_drive_detection(result) == result.symbol_rate.found);
        }
    }
}

// REJECTS: a family reported at a symbol rate the detection could not hold.
// The rule reads only the rate the family carried and the width it was handed,
// so the same BPSK signal passes at its own width and is refused at the width
// of one line of an NFM comb.
TEST_CASE("a symbol rate wider than the detection refuses the family", "[characterise]")
{
    const auto samples = in_noise(psk(false, kSeed + 2), 30.0, kSeed + 200);

    const auto own_width = run(samples, 1430.0);
    print_row("bpsk", 30.0, own_width);
    CHECK(own_width.family == ModulationFamily::Psk);
    CHECK_FALSE(own_width.symbol_rate_exceeds_detection);
    CHECK(characterise::may_drive_detection(own_width));

    const auto one_line = run(samples, 183.0);
    INFO(one_line.refusal);
    CHECK(one_line.family == ModulationFamily::Unknown);
    CHECK(one_line.symbol_rate_exceeds_detection);
    CHECK(one_line.symbol_rate.found);
    CHECK(one_line.refusal.find("wider than") != std::string::npos);
    CHECK_FALSE(characterise::may_drive_detection(one_line));

    // No detection behind the extract, which is a file characterised by hand:
    // the rule has nothing to read and stays out of the way.
    const auto by_hand = run(samples, 0.0);
    CHECK(by_hand.family == ModulationFamily::Psk);
}

// REJECTS: a label that calls every carrier CW, and one that calls a keyed
// carrier AM. All three are Unmodulated to the characteriser; the reading
// that separates them is whether power sits in sidebands mirrored about the
// carrier, away from where keying puts it.
TEST_CASE("an AM carrier reads double sideband and a keyed or bare one does not",
          "[characterise]")
{
    const double levels[] = {30.0, 20.0, 10.0};
    for (const double level : levels) {
        siggen::ModulatorConfig common;
        common.rate = kRate;
        common.seed = kSeed + 5;

        siggen::AmParams am;
        am.modulation_index = 0.8;
        am.tone_hz = 1000;
        auto am_made = siggen::generate_am(common, am, kSamples);
        REQUIRE(am_made.has_value());
        const auto am_result =
            run(in_noise(am_made->samples, level, kSeed + 300 + static_cast<std::uint64_t>(level)));

        siggen::CwParams cw;
        cw.words_per_minute = 25.0;
        auto cw_made = siggen::generate_cw(common, cw, kSamples);
        REQUIRE(cw_made.has_value());
        const auto cw_result =
            run(in_noise(cw_made->samples, level, kSeed + 400 + static_cast<std::uint64_t>(level)));

        std::vector<dsp::Complex32> bare(kSamples, dsp::Complex32(1.0F, 0.0F));
        const auto bare_result =
            run(in_noise(bare, level, kSeed + 500 + static_cast<std::uint64_t>(level)));

        // AM carrying something shaped like speech rather than a test tone:
        // twelve tones from 300 to 2950 Hz at fixed random phases, at an RMS
        // of 0.3, which is where speech sits against a full-scale peak. Its
        // sidebands hold far less of the power than a full-scale tone's.
        std::vector<float> speech(kSamples, 0.0F);
        for (int tone = 0; tone < 12; ++tone) {
            const double hz = 300.0 + 240.0 * tone + 10.0;
            const double phase = 0.7 * tone * tone;
            for (std::size_t n = 0; n < kSamples; ++n) {
                speech[n] += static_cast<float>(
                    std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRate + phase));
            }
        }
        double speech_power = 0.0;
        for (const float sample : speech) {
            speech_power += static_cast<double>(sample) * sample;
        }
        const auto speech_scale =
            static_cast<float>(0.3 / std::sqrt(speech_power / static_cast<double>(kSamples)));
        for (float& sample : speech) {
            sample *= speech_scale;
        }
        auto voice_made = siggen::generate_am(common, am, kSamples, speech);
        REQUIRE(voice_made.has_value());
        const auto voice_result = run(
            in_noise(voice_made->samples, level, kSeed + 800 + static_cast<std::uint64_t>(level)));

        for (const auto& [label, result] :
             {std::pair{"am", &am_result}, std::pair{"am voice", &voice_result},
              std::pair{"cw", &cw_result}, std::pair{"carrier", &bare_result}}) {
            std::println("  {:<8} {:>5.1f} dB  {:<20} sidebands {:.3f} of the excess, symmetry "
                         "{:.3f}, double sideband {}",
                         label, level, characterise::modulation_family_name(result->family),
                         result->sideband_share, result->sideband_symmetry,
                         result->double_sideband);
        }
        INFO(level << " dB");
        CHECK(am_result.family == ModulationFamily::Unmodulated);
        CHECK(am_result.double_sideband);
        CHECK(voice_result.double_sideband);
        CHECK_FALSE(cw_result.double_sideband);
        CHECK_FALSE(bare_result.double_sideband);
    }
}

// REJECTS: a carrier call on FM whose index is low enough to leave most of
// the power in the carrier, which is voice on narrowband FM, and an AM call
// on it, which is what the mirrored sidebands alone would say. The envelope
// is what separates the two: AM's sidebands are its envelope.
TEST_CASE("low-index FM is FM, and the same audio as AM is AM", "[characterise]")
{
    // Three tones at a tenth of full scale each, which with 2.5 kHz of
    // deviation per unit puts the index under half on every one of them.
    std::vector<float> audio(kSamples);
    for (std::size_t n = 0; n < kSamples; ++n) {
        const double t = static_cast<double>(n) / static_cast<double>(kRate);
        audio[n] = static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 500.0 * t) +
                                      0.1 * std::sin(2.0 * std::numbers::pi * 1100.0 * t) +
                                      0.1 * std::sin(2.0 * std::numbers::pi * 2300.0 * t));
    }
    for (const double level : {30.0, 20.0, 15.0}) {
        siggen::ModulatorConfig common;
        common.rate = kRate;
        common.seed = kSeed + 6;
        siggen::NfmParams nfm;
        nfm.deviation = 2500;
        auto fm_made = siggen::generate_nfm(common, nfm, kSamples, audio);
        REQUIRE(fm_made.has_value());
        const auto fm_result =
            run(in_noise(fm_made->samples, level, kSeed + 600 + static_cast<std::uint64_t>(level)));

        // The same three tones three times louder for the AM emitter: at a
        // tenth of full scale each, index 0.8 puts under a percent of the
        // excess in its sidebands, which is a carrier by any measure.
        std::vector<float> louder(audio);
        for (float& sample : louder) {
            sample *= 3.0F;
        }
        siggen::AmParams am;
        am.modulation_index = 0.8;
        auto am_made = siggen::generate_am(common, am, kSamples, louder);
        REQUIRE(am_made.has_value());
        const auto am_result =
            run(in_noise(am_made->samples, level, kSeed + 700 + static_cast<std::uint64_t>(level)));

        for (const auto& [label, result] :
             {std::pair{"fm", &fm_result}, std::pair{"am", &am_result}}) {
            std::println("  {:<3} {:>5.1f} dB  {:<20} concentration {:.3f}, sidebands {:.3f}, "
                         "symmetry {:.3f}, envelope variance {:.4f}, net of noise {:.4f}",
                         label, level, characterise::modulation_family_name(result->family),
                         result->spectral_concentration, result->sideband_share,
                         result->sideband_symmetry, result->envelope.normalised_power_variance,
                         result->envelope_variance_net);
        }
        INFO(level << " dB: " << fm_result.summary);
        CHECK(fm_result.family == ModulationFamily::AnalogueFm);
        CHECK(fm_result.low_index_fm);
        CHECK_FALSE(fm_result.double_sideband);
        CHECK(am_result.double_sideband);
        CHECK_FALSE(am_result.low_index_fm);
    }
}
