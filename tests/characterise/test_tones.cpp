// Tone structure and envelope statistics.
//
// The case that matters most here is the one where the right answer is no.
// A broadcast FM carrier has two maxima in its instantaneous-frequency
// histogram with a dip between them, so every structural test a naive mode
// counter applies passes and the answer comes back "2-FSK". It is not
// 2-FSK, and reporting it as such is worse than reporting nothing: an
// operator acting on it tunes a receiver that will never lock, and the
// numbers under the label all look reasonable.
//
// Each case names the wrong implementation it rejects.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <print>
#include <string>
#include <vector>

#include "core/characterise/tones.h"
#include "core/characterise/transform.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wfm_mod.h"
#include "tests/characterise/signal_lab.h"

using namespace revenant;
using characterise::Complex64;
using Catch::Approx;

namespace {

constexpr std::uint64_t kSeed = 20260921;
constexpr dsp::SampleRate kRate = 48000;
constexpr std::size_t kSamples = 1U << 17;

[[nodiscard]] std::vector<Complex64> analysis_of(const std::vector<dsp::Complex32>& samples)
{
    return characterise::to_analysis(dsp::ConstComplexSpan(samples));
}

// Prints what the histogram found, because the numbers ARE the result a
// reader of a failure wants and Catch2 only shows a CAPTURE on the
// assertion that failed.
void report(const char* label, const characterise::ToneStructure& structure)
{
    std::println("{}: {} modes, spacing {:.1f} Hz, valley {:.3f}, widest {:.3f}, spread "
                 "{:.1f} Hz, found {}",
                 label, structure.tone_count, structure.spacing_hz, structure.valley_ratio,
                 structure.widest_tone_fraction, structure.frequency_spread_hz,
                 structure.found ? "yes" : "no");
    for (const characterise::ToneMode& mode : structure.tones) {
        std::println("    tone {:>10.1f} Hz  weight {:.3f}  width {:.1f} Hz", mode.offset_hz,
                     mode.weight, mode.width_hz);
    }
    if (!structure.refusal.empty()) {
        std::println("    refused: {}", structure.refusal);
    }
}

}  // namespace

// REJECTS: an estimator that reports the DEVIATION as the tone spacing.
// core/dsp/synth/modulators.h states Fsk2Params::deviation as the peak
// either side of the carrier, so the spacing is twice it, and a factor of
// two here maps a 4800 Hz shift onto every 2400 Hz catalogue row instead of
// the right ones. The two numbers are both in the spec and both plausible,
// which is what makes it the mistake to guard against rather than a typo.
TEST_CASE("2-FSK gives up its tone count and its shift", "[characterise]")
{
    siggen::ModulatorConfig common;
    common.rate = kRate;
    common.seed = kSeed;
    std::println("test_tones 2-fsk: seed {}", common.seed);

    siggen::Fsk2Params fsk;
    fsk.symbol_rate = 1200.0;
    fsk.deviation = 2400;
    fsk.symbol_count = 4096;

    const auto generated = siggen::generate_fsk2(common, fsk, kSamples);
    REQUIRE(generated.has_value());

    const auto structure =
        characterise::estimate_tone_structure(analysis_of(generated->samples), kRate);
    REQUIRE(structure.has_value());
    report("2-FSK", *structure);
    CAPTURE(structure->tone_count, structure->spacing_hz, structure->centre_offset_hz,
            structure->valley_ratio, structure->refusal);
    REQUIRE(structure->found);
    REQUIRE(structure->tone_count == 2);
    REQUIRE(structure->spacing_hz == Approx(2.0 * static_cast<double>(fsk.deviation)).epsilon(0.02));
    REQUIRE(structure->centre_offset_hz == Approx(0.0).margin(40.0));

    // An FSK signal is AT a tone except while it is moving between them, so
    // the space between the tones is nearly empty. This is the number the
    // broadcast FM case below is compared against.
    REQUIRE(structure->valley_ratio < 0.10);
    REQUIRE(structure->confidence > 0.85);
}

// REJECTS: a tone detector hard-wired to two tones, which is what a
// "detect FSK" function usually turns out to be, and which would report
// 2 tones at twice the spacing on this signal. Every 4FSK row in
// docs/modes.md depends on the difference: DMR, dPMR, NXDN, P25 Phase 1,
// M17 and FLEX are all four-level, and a characteriser that can only count
// to two puts every one of them in the wrong half of the catalogue.
TEST_CASE("4-FSK is four tones, not two at double the spacing", "[characterise]")
{
    characterise_test::MfskSpec spec;
    spec.rate = kRate;
    spec.tone_count = 4;
    // 1944 Hz between adjacent tones is the C4FM deviation set, +/-1944 and
    // +/-648 Hz, which docs/modes.md's P25 Phase 1 and DMR rows describe.
    // The symbol rate is 4800 here, which at 48 kS/s is a whole ten samples
    // a symbol.
    spec.spacing_hz = 1296;
    spec.symbol_rate = 4800.0;
    spec.seed = kSeed + 1;
    std::println("test_tones 4-fsk: seed {}", spec.seed);

    const auto samples = characterise_test::mfsk_signal(spec, kSamples);
    REQUIRE(samples.size() == kSamples);

    const auto structure = characterise::estimate_tone_structure(analysis_of(samples), kRate);
    REQUIRE(structure.has_value());
    report("4-FSK", *structure);
    CAPTURE(structure->tone_count, structure->spacing_hz, structure->spacing_spread_hz,
            structure->valley_ratio, structure->refusal);
    REQUIRE(structure->found);
    REQUIRE(structure->tone_count == 4);
    REQUIRE(structure->spacing_hz ==
            Approx(static_cast<double>(spec.spacing_hz)).epsilon(0.03));

    // Evenly spaced, which is what makes them a tone set rather than four
    // things that happened to stand up in a histogram.
    REQUIRE(structure->spacing_spread_hz < 0.15 * structure->spacing_hz);
    REQUIRE(structure->centre_offset_hz == Approx(0.0).margin(60.0));
}

// REJECTS: a mode counter that finds local maxima and counts them.
//
// This is the case the whole file is built around. A broadcast FM carrier's
// instantaneous frequency has an arcsine-like distribution: it spends
// longest at the extremes of its swing, so the histogram has a maximum at
// each end with a dip between. Two maxima, evenly placed, separated by a
// plausible shift. A counter with no valley rule reports 2-FSK and every
// number under the label reads as a measurement.
//
// The assertion is not only that it refuses. It is that the refusal names
// the valley, because a bare "not FSK" leaves the caller unable to tell
// this from a weak signal, and the two want different next moves.
TEST_CASE("a broadcast FM carrier is not two-tone FSK", "[characterise]")
{
    siggen::WfmSpec spec;
    spec.rate = 684000;
    spec.carrier_offset = 0;
    spec.rds.bits = siggen::random_bits(416, kSeed + 2);
    std::println("test_tones wfm: seed {}", kSeed + 2);

    const auto station = siggen::generate_wfm(spec, 1U << 18);
    REQUIRE(station.has_value());

    const auto structure =
        characterise::estimate_tone_structure(analysis_of(station->samples), spec.rate);
    REQUIRE(structure.has_value());
    report("broadcast FM", *structure);
    CAPTURE(structure->tone_count, structure->valley_ratio, structure->widest_tone_fraction,
            structure->spacing_hz, structure->frequency_spread_hz, structure->refusal);
    REQUIRE_FALSE(structure->found);
    REQUIRE_FALSE(structure->refusal.empty());

    // Not the valley rule. Measured on this station, the composite's
    // instantaneous frequency breaks into several modes whose deepest
    // valley is about 0.25, well inside the 0.40 a tone set may have, so
    // the valley rule alone passes it. What fails is the width: each mode
    // is a lump the signal sweeps through rather than a value it sits at.
    //
    // This is the assertion that would fail against a characteriser with
    // only the valley rule in it, and it is why the width rule exists.
    REQUIRE(structure->widest_tone_fraction > 0.25);
    REQUIRE(structure->refusal.find("standard deviation") != std::string::npos);

    // And the swing is wide, so nothing above can be explained away as the
    // extract having missed the station.
    REQUIRE(structure->frequency_spread_hz > 5000.0);
}

// REJECTS: an estimator that reports a tone set for anything it is handed.
// Two inputs with nothing to find, and each one's refusal has to say a
// different thing, because the caller's next move differs: a carrier wants
// a different measurement and noise wants a different extract.
TEST_CASE("a carrier and a channel of noise are both refused, differently", "[characterise]")
{
    std::println("test_tones carrier and noise: seed {}", kSeed + 3);

    const auto carrier = characterise_test::pure_tone(kSamples, kRate, 1200, 1.0);
    const auto carrier_structure =
        characterise::estimate_tone_structure(analysis_of(carrier), kRate);
    REQUIRE(carrier_structure.has_value());
    report("unmodulated carrier", *carrier_structure);
    CAPTURE(carrier_structure->tone_count, carrier_structure->frequency_spread_hz,
            carrier_structure->refusal);
    REQUIRE_FALSE(carrier_structure->found);
    REQUIRE(carrier_structure->tone_count == 0);
    // Unmodulated, so the instantaneous frequency is one value and the
    // spread is float32 rounding rather than a signal. The refusal says so
    // in those terms rather than reporting the modes that rounding breaks
    // into, which measured five with empty valleys between them.
    REQUIRE(carrier_structure->frequency_spread_hz < 1.0);
    REQUIRE(carrier_structure->refusal.find("this buffer can resolve") != std::string::npos);

    const auto noise = characterise_test::gaussian_noise(kSamples, 1.0, kSeed + 3);
    const auto noise_structure = characterise::estimate_tone_structure(analysis_of(noise), kRate);
    REQUIRE(noise_structure.has_value());
    report("noise", *noise_structure);
    CAPTURE(noise_structure->tone_count, noise_structure->valley_ratio,
            noise_structure->frequency_spread_hz, noise_structure->refusal);
    REQUIRE_FALSE(noise_structure->found);
    // The instantaneous frequency of complex white noise is uniform over
    // the whole band, so the histogram is flat and the prominence rule is
    // the one that has to hold. A flat histogram with shot noise on it has
    // maxima; none of them reaches 1.6 times the mean.
    REQUIRE(noise_structure->frequency_spread_hz > 0.1 * static_cast<double>(kRate));
}

// REJECTS: an envelope statistic that is not normalised by the mean power.
// The variance of |x|^2 scales as the fourth power of the gain, so an
// unnormalised figure says more about where the receiver's gain was set
// than about the waveform, and every threshold placed on it moves with the
// front end. The two amplitudes here are seven times apart and have to give
// the same number.
//
// Also rejects one that cannot tell a constant envelope from a shaped one,
// which is the discrimination the characteriser actually spends this on.
TEST_CASE("the envelope statistic is a property of the waveform, not the gain",
          "[characterise]")
{
    siggen::ModulatorConfig common;
    common.rate = kRate;
    common.seed = kSeed + 4;
    std::println("test_tones envelope: seed {}", common.seed);

    siggen::Fsk2Params fsk;
    fsk.symbol_rate = 1200.0;
    fsk.deviation = 2400;
    fsk.symbol_count = 4096;

    const auto quiet = siggen::generate_fsk2(common, fsk, kSamples);
    REQUIRE(quiet.has_value());
    siggen::ModulatorConfig loud_config = common;
    loud_config.amplitude = 7.0;
    const auto loud = siggen::generate_fsk2(loud_config, fsk, kSamples);
    REQUIRE(loud.has_value());

    const auto quiet_stats = characterise::envelope_stats(analysis_of(quiet->samples));
    const auto loud_stats = characterise::envelope_stats(analysis_of(loud->samples));
    CAPTURE(quiet_stats.mean_power, quiet_stats.normalised_power_variance, loud_stats.mean_power,
            loud_stats.normalised_power_variance);
    REQUIRE(loud_stats.mean_power == Approx(49.0 * quiet_stats.mean_power).epsilon(0.01));
    REQUIRE(loud_stats.normalised_power_variance ==
            Approx(quiet_stats.normalised_power_variance).margin(1.0e-6));
    REQUIRE(quiet_stats.normalised_power_variance < 1.0e-3);

    siggen::PskParams psk;
    psk.symbol_rate = 2400.0;
    psk.rolloff = 0.35;
    psk.symbol_count = 8192;
    const auto shaped = siggen::generate_bpsk(common, psk, kSamples);
    REQUIRE(shaped.has_value());
    const auto shaped_stats = characterise::envelope_stats(analysis_of(shaped->samples));
    CAPTURE(shaped_stats.normalised_power_variance, shaped_stats.peak_to_average_db);
    REQUIRE(shaped_stats.normalised_power_variance > 0.05);

    // Circularly symmetric complex Gaussian noise has an exponential
    // squared magnitude, whose variance is exactly the square of its mean,
    // so this figure is 1 by arithmetic rather than by measurement. It is
    // the calibration point for reading any other value.
    const auto noise_stats = characterise::envelope_stats(
        analysis_of(characterise_test::gaussian_noise(kSamples, 3.0, kSeed + 5)));
    CAPTURE(noise_stats.normalised_power_variance);
    REQUIRE(noise_stats.normalised_power_variance == Approx(1.0).epsilon(0.05));
}

// REJECTS: a working range for the tone estimator that nobody measured,
// and the failure mode that matters more than the range itself.
//
// Noise on the phase difference widens every mode, so as the extract gets
// weaker the histogram's four spikes become four lumps and then one. There
// are two ways out of that and only one of them is acceptable. The width
// rule can fire and the estimator refuses, which is right. Or two adjacent
// lumps can merge while the outer two stay separate, and the estimator
// then reports a clean, confident, WRONG tone count of three, with a
// spacing that is the average of two real spacings and matches a catalogue
// row that has nothing to do with the signal.
//
// The per-point assertion is what rules the second out: every answer the
// estimator gives across the sweep has to be four tones at the right
// spacing, and the points where it cannot manage that have to be
// refusals. The table is printed, so the range is a measurement in the log
// rather than a sentence in a header nobody re-runs.
TEST_CASE("the tone estimator degrades measurably, not silently", "[characterise]")
{
    characterise_test::MfskSpec spec;
    spec.rate = kRate;
    spec.tone_count = 4;
    spec.spacing_hz = 1296;
    spec.symbol_rate = 4800.0;
    spec.seed = kSeed + 6;
    std::println("test_tones snr sweep: seed {}", spec.seed);

    const auto clean = characterise_test::mfsk_signal(spec, kSamples);
    REQUIRE(clean.size() == kSamples);

    double lowest_working_db = 1000.0;
    bool refused_somewhere = false;
    for (const double snr_db : {55.0, 50.0, 45.0, 40.0, 35.0, 30.0, 20.0, 10.0}) {
        auto noisy = clean;
        const auto noise = siggen::add_awgn(dsp::ComplexSpan(noisy),
                                            siggen::NoiseLevel::snr_in_2500_hz_db(snr_db), kRate,
                                            siggen::derive_seed(spec.seed, 1));
        REQUIRE(noise.has_value());

        const auto structure = characterise::estimate_tone_structure(analysis_of(noisy), kRate);
        REQUIRE(structure.has_value());
        std::println("  snr_2500 {:>6.1f} dB  found {}  {} modes  spacing {:>7.1f} Hz  "
                     "widest {:.3f}  valley {:.3f}",
                     snr_db, structure->found ? "yes" : " no", structure->tone_count,
                     structure->spacing_hz, structure->widest_tone_fraction,
                     structure->valley_ratio);

        CAPTURE(snr_db, structure->tone_count, structure->spacing_hz, structure->refusal);
        if (structure->found) {
            REQUIRE(structure->tone_count == 4);
            REQUIRE(structure->spacing_hz ==
                    Approx(static_cast<double>(spec.spacing_hz)).epsilon(0.05));
            lowest_working_db = std::min(lowest_working_db, snr_db);
        } else {
            refused_somewhere = true;
            REQUIRE_FALSE(structure->refusal.empty());
        }
    }

    // 45 dB in 2500 Hz, which is a demanding figure and is the honest one.
    //
    // The instantaneous frequency is a per-sample quantity, so its noise
    // is set by the extract's FULL-BAND signal-to-noise ratio: 45 dB in
    // 2500 Hz is 32 dB across this 48 kS/s buffer, which puts about 190 Hz
    // of noise on a 1296 Hz spacing. Carrying 48 kHz of noise for an 8 kHz
    // signal throws away 7.8 dB of that before anything else happens, and
    // core/characterise/tones.h says what the two levers are. The
    // estimators in cyclostationary.h work thirty decibels lower on the
    // same signal, which is why this bound is recorded rather than
    // rounded.
    CAPTURE(lowest_working_db);
    REQUIRE(lowest_working_db <= 45.0);
    // And it has to stop somewhere in the swept range, or the per-point
    // assertion above never ran against a hard case.
    REQUIRE(refused_somewhere);
}
