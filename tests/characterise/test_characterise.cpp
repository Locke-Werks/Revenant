// The characteriser end to end: signals in, a family and a candidate list
// out.
//
// Every signal here has parameters that were chosen rather than measured,
// so each assertion is against arithmetic. Every case names the wrong
// implementation it rejects, and two of the six have "no" as the right
// answer, because a characteriser that always answers is a characteriser
// that is sometimes confidently wrong.
//
// The summaries are printed. They are what an operator reads, and a test
// that asserted on them without showing them would be checking a string
// nobody had looked at.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <cstddef>
#include <cstdint>
#include <print>
#include <string>
#include <vector>

#include "core/characterise/characterise.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wfm_mod.h"
#include "tests/characterise/signal_lab.h"

using namespace revenant;
using characterise::ModulationFamily;
using Catch::Approx;

namespace {

constexpr std::uint64_t kSeed = 20260921;
constexpr dsp::SampleRate kRate = 48000;
constexpr std::size_t kSamples = 1U << 17;

[[nodiscard]] characterise::CharacteriseConfig config(dsp::SampleRate rate)
{
    characterise::CharacteriseConfig out;
    out.rate = rate;
    return out;
}

void report(const char* label, const characterise::Characterisation& result)
{
    std::println("{}: family {}, confidence {:.3f}", label,
                 characterise::modulation_family_name(result.family), result.family_confidence);
    std::println("    {}", result.summary);
}

[[nodiscard]] bool names(const characterise::Characterisation& result, std::string_view wanted)
{
    return std::any_of(result.candidates.begin(), result.candidates.end(),
                       [wanted](const characterise::ProtocolCandidate& candidate) {
                           return candidate.row.name == wanted;
                       });
}

}  // namespace

// REJECTS: a characteriser that names one protocol. The five systems on
// this physical layer are indistinguishable from the numbers, so the list
// is the answer and the count is the assertion that catches a single
// guess.
//
// Also rejects one that reads the tone spacing as a deviation, one that
// cannot count past two tones, and one that reports the occupied bandwidth
// as a symbol rate: the spacing is 1296 Hz, the count is four, the rate is
// 4800 and the bandwidth is about 9 kHz, and no two of those are within
// the tolerances below of each other.
TEST_CASE("a 4FSK signal comes back as every system that shares its layer", "[characterise]")
{
    characterise_test::MfskSpec spec;
    spec.rate = kRate;
    spec.tone_count = 4;
    spec.spacing_hz = 1296;
    spec.symbol_rate = 4800.0;
    spec.seed = kSeed;
    std::println("test_characterise 4fsk: seed {}", spec.seed);

    const auto samples = characterise_test::mfsk_signal(spec, kSamples);
    REQUIRE(samples.size() == kSamples);

    const auto result = characterise::characterise(dsp::ConstComplexSpan(samples), config(kRate));
    REQUIRE(result.has_value());
    report("4FSK 4800", *result);
    CAPTURE(result->refusal, result->tones.tone_count, result->tones.spacing_hz,
            result->symbol_rate.symbol_rate_hz, result->band.bandwidth_hz);

    REQUIRE_FALSE(result->refused);
    REQUIRE(result->family == ModulationFamily::Fsk);
    REQUIRE(result->tones.tone_count == 4);
    REQUIRE(result->tones.spacing_hz == Approx(1296.0).epsilon(0.03));
    REQUIRE(result->symbol_rate.found);
    REQUIRE(result->symbol_rate.symbol_rate_hz == Approx(4800.0).epsilon(0.02));
    REQUIRE(result->family_confidence > 0.5);

    REQUIRE(result->candidates.size() >= 4);
    REQUIRE(names(*result, "DMR Tier I/II/III"));
    REQUIRE(names(*result, "NXDN at 12.5 kHz"));
    REQUIRE(names(*result, "P25 Phase 1 C4FM"));
    REQUIRE(result->summary.find(" or ") != std::string::npos);
}

// REJECTS: a characteriser that reads the M-th power line without ruling
// out a multi-tone signal first. Squaring 2-FSK produces two lines that
// clear every threshold, so an order estimator read directly reports BPSK
// for the signal above and for this one, and only this one is right.
TEST_CASE("a BPSK signal comes back as PSK with its order and its rate", "[characterise]")
{
    siggen::ModulatorConfig common;
    common.rate = kRate;
    common.carrier_offset = 0;
    common.seed = kSeed + 1;
    std::println("test_characterise bpsk: seed {}", common.seed);

    siggen::PskParams psk;
    psk.symbol_rate = 2400.0;
    psk.rolloff = 0.35;
    psk.symbol_count = 8192;

    const auto generated = siggen::generate_bpsk(common, psk, kSamples);
    REQUIRE(generated.has_value());
    REQUIRE(generated->cycle_samples > kSamples);

    const auto result =
        characterise::characterise(dsp::ConstComplexSpan(generated->samples), config(kRate));
    REQUIRE(result.has_value());
    report("BPSK 2400", *result);
    CAPTURE(result->refusal, result->order.order, result->symbol_rate.symbol_rate_hz,
            result->envelope.normalised_power_variance);

    REQUIRE_FALSE(result->refused);
    REQUIRE(result->family == ModulationFamily::Psk);
    REQUIRE(result->order.order == 2);
    REQUIRE(result->symbol_rate.found);
    REQUIRE(result->symbol_rate.symbol_rate_hz == Approx(2400.0).epsilon(0.01));

    // Root raised cosine, so the envelope is not constant, which is what
    // kept this out of the analogue branch ahead of it.
    REQUIRE(result->envelope.normalised_power_variance > 0.05);
    REQUIRE(names(*result, "MIL-STD-188-110 serial tone"));
    REQUIRE_FALSE(result->tones.found);
}

// REJECTS: the whole naive design. A broadcast FM carrier is constant
// envelope with maxima in its instantaneous-frequency histogram, so a
// characteriser built out of a mode counter and a power-law order reports
// it as 2-FSK or as BPSK, and every number under either label is a real
// measurement. This case asserts it is neither, that no symbol rate is
// claimed for it, and that the one candidate it does produce is the right
// one.
TEST_CASE("a broadcast FM station is analogue FM, and no symbol rate", "[characterise]")
{
    siggen::WfmSpec spec;
    spec.rate = 684000;
    spec.carrier_offset = 0;
    spec.rds.bits = siggen::random_bits(416, kSeed + 2);
    std::println("test_characterise wfm: seed {}", kSeed + 2);

    const auto station = siggen::generate_wfm(spec, 1U << 18);
    REQUIRE(station.has_value());

    const auto result =
        characterise::characterise(dsp::ConstComplexSpan(station->samples), config(spec.rate));
    REQUIRE(result.has_value());
    report("broadcast FM", *result);
    CAPTURE(result->refusal, result->tones.tone_count, result->tones.widest_tone_fraction,
            result->envelope.normalised_power_variance, result->band.bandwidth_hz,
            result->squared_envelope.found, result->frequency_transition.found);

    REQUIRE_FALSE(result->refused);
    REQUIRE(result->family == ModulationFamily::AnalogueFm);
    REQUIRE_FALSE(result->tones.found);

    // The key assertion. The frequency-transition detector DOES find cycle
    // frequencies in this signal, because the composite's own tones put
    // them there, and reporting one as a symbol rate would be a
    // measurement attached to the wrong thing.
    REQUIRE_FALSE(result->symbol_rate.found);
    REQUIRE(result->summary.find("not reported as a symbol rate") != std::string::npos);

    REQUIRE(names(*result, "FM broadcast"));
    REQUIRE_FALSE(names(*result, "Narrowband FM land mobile"));

    // Half and no more, because this branch is an elimination rather than
    // a positive finding and cannot rule out a low-index digital mode.
    REQUIRE(result->family_confidence == Approx(0.5));
}

// REJECTS: a characteriser that reports an OFDM burst's TOTAL symbol
// period, and one that has no OFDM reading at all and falls through to the
// analogue branch, which is where a multicarrier signal lands: its
// envelope is not constant, its instantaneous frequency has no tones in it
// and nothing else would claim it.
//
// The empty candidate list is deliberate. 5.33 ms is not DAB Mode I's
// 1 ms, and the honest output is a measured symbol period with no name on
// it.
TEST_CASE("an OFDM burst comes back as OFDM with its own symbol period", "[characterise]")
{
    characterise_test::OfdmSpec spec;
    spec.useful_samples = 256;
    spec.prefix_samples = 32;
    spec.used_carriers = 100;
    spec.symbol_count = 600;
    spec.seed = kSeed + 3;
    std::println("test_characterise ofdm: seed {}", spec.seed);

    const auto samples = characterise_test::ofdm_signal(spec);
    REQUIRE(samples.size() == 288 * 600);

    const auto result = characterise::characterise(dsp::ConstComplexSpan(samples), config(kRate));
    REQUIRE(result.has_value());
    report("OFDM", *result);
    CAPTURE(result->refusal, result->ofdm.symbol_samples, result->ofdm.prefix_samples,
            result->ofdm.subcarrier_spacing_hz);

    REQUIRE_FALSE(result->refused);
    REQUIRE(result->family == ModulationFamily::Ofdm);
    REQUIRE(result->ofdm.symbol_samples == 256);
    REQUIRE(result->ofdm.subcarrier_spacing_hz == Approx(187.5).epsilon(0.01));
    REQUIRE(result->candidates.empty());
    REQUIRE(result->summary.find("nothing in the catalogue") != std::string::npos);
}

// REJECTS: a characteriser that keeps naming a tone count as its extract
// gets weaker. This is the SAME 4FSK signal as the first case, in enough
// noise that the histogram's four modes have merged, and it is the case
// where a mode counter starts reporting three tones at the average of two
// real spacings, which matches a catalogue row that has nothing to do with
// the signal.
//
// What the stage has to do instead is fall back to the weaker claim it can
// still support and say so. That claim is constant envelope with a
// continuous instantaneous frequency, which IS what a 4FSK signal whose
// tones cannot be separated looks like, at half confidence, with the
// cycle frequency reported and explicitly not called a symbol rate.
//
// tests/characterise/test_tones.cpp measures where the separation goes, at
// 45 dB in 2500 Hz on an unfiltered 48 kS/s extract, and core/characterise/
// tones.h says why that figure is as high as it is and what the lever is.
TEST_CASE("a 4FSK signal too weak to separate falls back rather than guessing",
          "[characterise]")
{
    characterise_test::MfskSpec spec;
    spec.rate = kRate;
    spec.tone_count = 4;
    spec.spacing_hz = 1296;
    spec.symbol_rate = 4800.0;
    spec.seed = kSeed + 6;
    std::println("test_characterise weak 4fsk: seed {}", spec.seed);

    auto samples = characterise_test::mfsk_signal(spec, kSamples);
    REQUIRE(samples.size() == kSamples);
    const auto noise = siggen::add_awgn(dsp::ComplexSpan(samples),
                                        siggen::NoiseLevel::snr_in_2500_hz_db(30.0), kRate,
                                        siggen::derive_seed(spec.seed, 1));
    REQUIRE(noise.has_value());

    const auto result = characterise::characterise(dsp::ConstComplexSpan(samples), config(kRate));
    REQUIRE(result.has_value());
    report("4FSK at 30 dB in 2500 Hz", *result);
    CAPTURE(result->tones.tone_count, result->tones.refusal, result->refusal,
            result->frequency_transition.symbol_rate_hz);

    // Not a tone set, and not a WRONG tone set either, which is the
    // failure this case exists for.
    REQUIRE_FALSE(result->tones.found);
    REQUIRE(result->family != ModulationFamily::Fsk);
    REQUIRE(result->family != ModulationFamily::Psk);

    REQUIRE(result->family == ModulationFamily::AnalogueFm);
    REQUIRE(result->family_confidence == Approx(0.5));
    REQUIRE_FALSE(result->symbol_rate.found);

    // And the symbol rate is still in there, reported as the unattributed
    // cycle frequency it is rather than thrown away.
    REQUIRE(result->frequency_transition.found);
    REQUIRE(result->frequency_transition.symbol_rate_hz == Approx(4800.0).epsilon(0.02));
    REQUIRE(result->summary.find("cycle frequency") != std::string::npos);
    REQUIRE(result->summary.find("Nothing here decides which") != std::string::npos);
}

// REJECTS: a characteriser with no unmodulated branch, which reports a
// bare carrier as whatever its noise happens to look like. The catalogue
// has no row for a carrier, so the candidate list is empty and that is the
// right answer: the finding is the family.
TEST_CASE("an unmodulated carrier is named as one", "[characterise]")
{
    std::println("test_characterise carrier: seed {}", kSeed + 4);
    const auto carrier = characterise_test::pure_tone(kSamples, kRate, 1200, 1.0);

    const auto result = characterise::characterise(dsp::ConstComplexSpan(carrier), config(kRate));
    REQUIRE(result.has_value());
    report("unmodulated carrier", *result);
    CAPTURE(result->spectral_concentration, result->refusal);

    REQUIRE_FALSE(result->refused);
    REQUIRE(result->family == ModulationFamily::Unmodulated);
    REQUIRE(result->spectral_concentration > 0.5);
    REQUIRE_FALSE(result->symbol_rate.found);
    REQUIRE(result->candidates.empty());
}

// REJECTS: a characteriser whose answer about one signal depends on how much
// of it was handed over, with no way for a caller to hold that still.
//
// analysis_segment takes a sixteenth of the extract, so a longer extract gets
// finer bins, and spectral_concentration is the power in the strongest THREE
// of them: its window is a frequency that shrinks as the extract grows. Two
// runs over different lengths of the same carrier are then not comparable,
// which was measured on real 40 m on 2026-09-22 and is written up on
// CharacteriseConfig::segment.
//
// Here the same tone is measured at two lengths, first with the segment left
// to the sample count and then with it named. The first pair must differ,
// because that is the behaviour the knob exists for; the second must not,
// because that is the knob working.
TEST_CASE("a named segment makes two extract lengths comparable", "[characterise]")
{
    constexpr std::size_t kShort = 1U << 15;
    constexpr std::size_t kLong = 1U << 17;
    static_assert(kShort >= characterise::kMinCharacteriseSamples);

    const auto tone = characterise_test::pure_tone(kLong, kRate, 1200, 1.0);
    const auto shorter = dsp::ConstComplexSpan(tone.data(), kShort);
    const auto longer = dsp::ConstComplexSpan(tone);

    // Left to the sample count: a sixteenth of each, so 2048 bins against
    // 8192, and the three-bin window is four times narrower on the long one.
    const auto loose_short = characterise::characterise(shorter, config(kRate));
    const auto loose_long = characterise::characterise(longer, config(kRate));
    REQUIRE(loose_short.has_value());
    REQUIRE(loose_long.has_value());

    CAPTURE(loose_short->spectral_concentration, loose_long->spectral_concentration);
    CHECK(loose_short->spectral_concentration != loose_long->spectral_concentration);

    // Named: the same transform length whatever the extract, so the same
    // three bins span the same hertz and the number is about the signal.
    characterise::CharacteriseConfig fixed = config(kRate);
    fixed.segment = 2048;

    const auto held_short = characterise::characterise(shorter, fixed);
    const auto held_long = characterise::characterise(longer, fixed);
    REQUIRE(held_short.has_value());
    REQUIRE(held_long.has_value());

    CAPTURE(held_short->spectral_concentration, held_long->spectral_concentration);

    // Not bit-identical: the long extract averages more segments, so the
    // estimate is the same quantity measured better. Within a percent is the
    // claim, against a spread that is four times wider without the knob.
    CHECK(std::abs(held_short->spectral_concentration -
                   held_long->spectral_concentration) < 0.01);

    // And both still name the carrier, which is what makes the agreement
    // worth anything: two matching refusals would also be consistent.
    CHECK(held_short->family == ModulationFamily::Unmodulated);
    CHECK(held_long->family == ModulationFamily::Unmodulated);
}

// REJECTS: a characteriser that always answers, which is the one this
// project's rule about silence is aimed at. There is no signal in this
// buffer at all, and the required behaviour is a refusal that NAMES the
// numbers: what each estimator measured, what bar it failed, and what to
// do about it. A bare false leaves an operator unable to tell an empty
// channel from a receiver pointed at the wrong place.
TEST_CASE("a channel of noise is refused, and the refusal carries the numbers",
          "[characterise]")
{
    std::println("test_characterise noise: seed {}", kSeed + 5);
    const auto noise = characterise_test::gaussian_noise(kSamples, 1.0, kSeed + 5);

    const auto result = characterise::characterise(dsp::ConstComplexSpan(noise), config(kRate));
    REQUIRE(result.has_value());
    report("noise", *result);
    std::println("    refusal: {}", result->refusal);

    REQUIRE(result->refused);
    REQUIRE(result->family == ModulationFamily::Unknown);
    REQUIRE(result->candidates.empty());
    REQUIRE_FALSE(result->symbol_rate.found);

    // Circularly symmetric complex Gaussian noise has an exponential
    // squared magnitude, so this is 1 by arithmetic. It is the number the
    // refusal has to quote for a reader to see that the extract was noise
    // rather than a weak signal.
    REQUIRE(result->envelope.normalised_power_variance == Approx(1.0).epsilon(0.05));
    REQUIRE(result->refusal.find("normalised power variance") != std::string::npos);
    REQUIRE(result->refusal.find("The tone histogram said") != std::string::npos);
    REQUIRE(result->refusal.find("The squared envelope said") != std::string::npos);
    REQUIRE(result->refusal.find("The frequency transition said") != std::string::npos);
    REQUIRE(result->refusal.find("The cyclic prefix search said") != std::string::npos);
}

// REJECTS: a reader who takes a PSK call at face value, and a future change
// that makes the family alone drive detection.
//
// This is the measured limit, not a bug being reported. Buried far enough in
// noise an unmodulated carrier is called 2-PSK, and the mechanism is written
// into ModulationOrder's own comment: squaring a tone
// gives another tone. A carrier lights the M-th power line at exponent 2
// exactly the way BPSK does. What separates them is the envelope, and the
// envelope is what the noise takes away first: at enough noise the normalised
// power variance climbs past constant_envelope_variance, the unmodulated
// branch stops being reachable, and the PSK branch is the next one down.
//
// It is not theoretical. A 31-point sweep of 20 m at 1603 UT on 2026-09-22
// called ten channels 2-PSK, at 0.52 to 0.98 confidence, with symbol rates of
// 0, 11, 104, 298 and 738 baud or none at all, in channels the detector had
// found nothing in. docs/detection.md has the table.
//
// So the case walks a carrier down through the noise and asserts the flip
// happens, which fixes where it happens rather than leaving it to be
// rediscovered. The last assertion is the useful half: spectral_concentration
// falls monotonically across the same sweep, so the number that still carries
// the answer is the one the family call has stopped carrying.
TEST_CASE("a carrier buried in noise is called PSK, and concentration still is not",
          "[characterise]")
{
    // Total noise power against a unit-amplitude carrier, so this is 1/SNR.
    // Chosen to bracket the flip rather than to be round: the first is a
    // clean carrier and the last is well under what an HF channel gives you.
    constexpr std::array<double, 5> kNoise{0.0, 0.05, 0.5, 2.0, 8.0};

    std::vector<ModulationFamily> families;
    std::vector<double> concentrations;

    for (std::size_t i = 0; i < kNoise.size(); ++i) {
        const double variance = kNoise[i];
        auto samples = characterise_test::pure_tone(kSamples, kRate, 1200, 1.0);
        if (variance > 0.0) {
            const auto noise =
                characterise_test::gaussian_noise(kSamples, variance, kSeed + i);
            for (std::size_t n = 0; n < samples.size(); ++n) {
                samples[n] += noise[n];
            }
        }

        const auto result =
            characterise::characterise(dsp::ConstComplexSpan(samples), config(kRate));
        REQUIRE(result.has_value());

        std::println("  noise {:.2f} ({}): family {}, confidence {:.2f}, "
                     "concentration {:.3f}, power variance {:.3f}",
                     variance,
                     variance > 0.0 ? std::format("{:.1f} dB SNR", -10.0 * std::log10(variance))
                                    : std::string("no noise"),
                     characterise::modulation_family_name(result->family),
                     result->family_confidence, result->spectral_concentration,
                     result->envelope.normalised_power_variance);

        families.push_back(result->family);
        concentrations.push_back(result->spectral_concentration);
    }

    // The clean carrier is named. Anything else and the rest of this case is
    // measuring the wrong thing.
    REQUIRE(families.front() == ModulationFamily::Unmodulated);

    // And somewhere down the sweep it stops being named. THE POINT OF THE
    // CASE: the same signal, called something else, because the noise took
    // the envelope rather than because the signal changed.
    REQUIRE(families.back() != ModulationFamily::Unmodulated);

    // Once it leaves, it does not come back. A family that flickered between
    // carrier and PSK with falling SNR would be a different defect and would
    // want a different fix.
    const auto first_other =
        std::find_if(families.begin(), families.end(), [](ModulationFamily family) {
            return family != ModulationFamily::Unmodulated;
        });
    REQUIRE(std::none_of(first_other, families.end(), [](ModulationFamily family) {
        return family == ModulationFamily::Unmodulated;
    }));

    // The number that still means something. Concentration is S/(S+N) for a
    // carrier, so it falls with the noise and keeps falling after the family
    // call has given up: monotone across every step, which a discriminator
    // needs and a family name does not have.
    for (std::size_t i = 1; i < concentrations.size(); ++i) {
        CAPTURE(i, concentrations[i - 1], concentrations[i]);
        CHECK(concentrations[i] < concentrations[i - 1]);
    }
}

// REJECTS: a stage that runs on a buffer too short for its own estimators
// and returns whatever comes out. Each of the three has a different
// appetite and the refusal names all three, because a caller shortening an
// extract to save time needs to know which one bit.
TEST_CASE("an extract too short to analyse is refused with the figure", "[characterise]")
{
    const auto carrier = characterise_test::pure_tone(4000, kRate, 1200, 1.0);
    const auto result = characterise::characterise(dsp::ConstComplexSpan(carrier), config(kRate));
    REQUIRE_FALSE(result.has_value());
    const std::string& message = result.error().message;
    CAPTURE(message);
    REQUIRE(message.find("4000") != std::string::npos);
    REQUIRE(message.find("16384") != std::string::npos);
    REQUIRE(message.find("4096 sample pairs") != std::string::npos);

    const auto no_rate =
        characterise::characterise(dsp::ConstComplexSpan(carrier), characterise::CharacteriseConfig{});
    REQUIRE_FALSE(no_rate.has_value());
    REQUIRE(no_rate.error().message.find("rate must be positive") != std::string::npos);
}
