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

#include <cstddef>
#include <cstdint>
#include <print>
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
