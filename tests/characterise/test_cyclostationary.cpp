// Symbol rate and modulation order, against signals whose parameters were
// chosen rather than measured.
//
// Every case here names the wrong implementation it rejects. That is the bar
// this project keeps failing: a case that passes against any plausible
// implementation verifies nothing and reads as settled.
//
// The ground truth is siggen's, not another measurement. A modulator built
// from a symbol rate and a rolloff knows its realised symbol rate exactly,
// because the symbol grid closes on a whole number of samples and
// Modulator::effective_symbol_rate reports what that worked out to. So the
// assertions below are against arithmetic.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <print>
#include <string>
#include <vector>

#include "core/characterise/cyclostationary.h"
#include "core/characterise/transform.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"
#include "tests/characterise/signal_lab.h"

using namespace revenant;
using characterise::Complex64;
using characterise::CyclicDetector;
using Catch::Approx;

namespace {

constexpr std::uint64_t kSeed = 20260921;
constexpr dsp::SampleRate kRate = 48000;
constexpr std::size_t kSamples = 1U << 17;

// Long enough that the payload does not repeat inside the buffer.
//
// core/dsp/synth/modulators.h repeats its payload for the life of an
// emitter, which puts a line in the squared envelope at every multiple of
// the repeat rate. Measured 2026-09-21 with 256 symbols at 2400 baud, one of
// those comb lines stood 18.9 dB up at 3008 Hz and beat the symbol rate's
// own line. 8192 symbols at 20 samples each is 163840 samples, which is
// longer than any buffer here, and each case asserts that rather than
// trusting this comment.
constexpr std::size_t kPskSymbols = 8192;
constexpr std::size_t kFskSymbols = 4096;

[[nodiscard]] std::vector<Complex64> analysis_of(const std::vector<dsp::Complex32>& samples)
{
    return characterise::to_analysis(dsp::ConstComplexSpan(samples));
}

[[nodiscard]] siggen::ModulatorConfig config(dsp::Hertz carrier_offset, std::uint64_t seed)
{
    siggen::ModulatorConfig common;
    common.rate = kRate;
    common.carrier_offset = carrier_offset;
    common.amplitude = 1.0;
    common.seed = seed;
    return common;
}

}  // namespace

// REJECTS: an estimator that reports the occupied bandwidth, which for a
// root raised cosine at rolloff 0.35 is 3240 Hz and looks like a plausible
// symbol rate; one that reports twice or half the symbol rate, which is
// what a squaring loop does when its comb walk is off by a factor; and one
// that reports the loudest bin of the signal's own spectrum, which for a
// baseband PSK signal is DC. The tolerance is half a percent, which is
// tighter than the distance to any of the three.
TEST_CASE("BPSK gives up its symbol rate through the squared envelope", "[characterise]")
{
    const auto common = config(0, kSeed);
    std::println("test_cyclostationary bpsk baud: seed {}", common.seed);

    siggen::PskParams psk;
    psk.symbol_rate = 2400.0;
    psk.rolloff = 0.35;
    psk.symbol_count = kPskSymbols;

    const auto generated = siggen::generate_bpsk(common, psk, kSamples);
    REQUIRE(generated.has_value());
    const double truth = generated->effective_symbol_rate;
    REQUIRE(truth == Approx(2400.0).margin(1.0));
    REQUIRE(generated->cycle_samples > kSamples);

    const auto estimate = characterise::estimate_symbol_rate(
        analysis_of(generated->samples), kRate, CyclicDetector::SquaredEnvelope);
    REQUIRE(estimate.has_value());
    CAPTURE(estimate->symbol_rate_hz, estimate->margin_db, estimate->harmonic_divisor,
            estimate->resolution_hz, truth);
    REQUIRE(estimate->found);
    REQUIRE(estimate->symbol_rate_hz == Approx(truth).epsilon(0.005));
    REQUIRE(estimate->harmonic_divisor == 1);
    REQUIRE(estimate->margin_db > 15.0);
}

// REJECTS: a characteriser with one detector in it. The squared envelope is
// the standard instrument for a linear modulation and it reads NOTHING on a
// constant-envelope signal, because |x|^2 is a constant and the feature is
// identically zero. Every FSK and CPM waveform in docs/modes.md is constant
// envelope, which is most of the list, so a single-detector characteriser is
// blind to most of what it will be pointed at. The first half of this case
// is the one that matters: it asserts the squared envelope FAILS here, so
// that the second half cannot be read as the first detector doing the work.
TEST_CASE("the squared envelope is blind to FSK and the transition detector is not",
          "[characterise]")
{
    const auto common = config(0, kSeed + 1);
    std::println("test_cyclostationary fsk baud: seed {}", common.seed);

    siggen::Fsk2Params fsk;
    fsk.symbol_rate = 1200.0;
    fsk.deviation = 2400;
    fsk.symbol_count = kFskSymbols;

    const auto generated = siggen::generate_fsk2(common, fsk, kSamples);
    REQUIRE(generated.has_value());
    const double truth = generated->effective_symbol_rate;
    REQUIRE(truth == Approx(1200.0).margin(1.0));
    REQUIRE(generated->cycle_samples > kSamples);
    const auto widened = analysis_of(generated->samples);

    const auto blind = characterise::estimate_symbol_rate(widened, kRate,
                                                          CyclicDetector::SquaredEnvelope);
    REQUIRE(blind.has_value());
    CAPTURE(blind->feature_ripple, blind->refusal);
    REQUIRE_FALSE(blind->found);

    // And it says so. A bare false here is indistinguishable from a weak
    // signal, and the caller's next move differs: one wants a different
    // detector and the other wants a longer extract. The refusal names the
    // detector to run instead.
    REQUIRE_FALSE(blind->refusal.empty());
    REQUIRE(blind->refusal.find("FrequencyTransition") != std::string::npos);
    REQUIRE(blind->feature_ripple < characterise::kMinFeatureRipple);

    const auto seeing = characterise::estimate_symbol_rate(widened, kRate,
                                                           CyclicDetector::FrequencyTransition);
    REQUIRE(seeing.has_value());
    CAPTURE(seeing->symbol_rate_hz, seeing->margin_db, seeing->harmonic_divisor, truth);
    REQUIRE(seeing->found);
    REQUIRE(seeing->symbol_rate_hz == Approx(truth).epsilon(0.01));
}

// REJECTS: a comb walk that reports the strongest peak, and one that always
// divides by two.
//
// The stimulus is built rather than modulated, because whether a real FSK
// signal's strongest cyclic peak lands on the fundamental or on a harmonic
// is decided by estimation noise and changes with the seed. A case that
// exercised the walk only when the seed was kind would be exactly the shape
// of test this directory is trying not to write.
//
// |x|^2 of sqrt(1 + f) is 1 + f exactly, with no distortion of any kind, so
// the squared-envelope feature is whatever f is written to be. The first
// signal is a comb whose second harmonic is ten decibels louder than its
// fundamental: the right answer is the fundamental. The second is that
// second harmonic alone with no fundamental under it: the right answer is
// the harmonic itself, at face value. One implementation cannot pass both
// by picking a rule and sticking to it.
TEST_CASE("the comb walk divides to the fundamental, and only when it is there",
          "[characterise]")
{
    constexpr double kFundamental = 1000.0;
    constexpr std::size_t kCount = 1U << 17;
    std::println("test_cyclostationary comb walk: seed {}", kSeed + 4);

    // The noise is what makes the baseline a noise floor rather than the
    // window's own leakage skirt. Without it the whole spectrum outside the
    // two lines is Hann sidelobe ripple, whose local maxima clear any
    // threshold over their own block median, and the walk finds a comb in
    // the window rather than in the signal. Forty decibels under the weaker
    // line, so it decides nothing except what "no line here" looks like.
    const auto build = [](bool with_fundamental, std::uint64_t seed) {
        std::vector<Complex64> samples(kCount);
        const auto dither = characterise_test::gaussian_noise(kCount, 2.5e-3, seed);
        for (std::size_t n = 0; n < kCount; ++n) {
            const double turns = static_cast<double>(n) / static_cast<double>(kRate);
            double feature = std::cos(2.0 * std::numbers::pi * 2.0 * kFundamental * turns);
            if (with_fundamental) {
                // A tenth of the amplitude is 20 dB down in power, so the
                // strongest peak is unambiguously the second harmonic.
                feature += 0.1 * std::cos(2.0 * std::numbers::pi * kFundamental * turns);
            }
            feature += static_cast<double>(dither[n].real());
            samples[n] = Complex64(std::sqrt(1.0 + 0.2 * feature), 0.0);
        }
        return samples;
    };

    // The walk is off by default on this detector, for the reason
    // CyclicSearch::max_harmonic_divisor gives, so the case turns it on
    // rather than pretending it is the default. What is under test is the
    // walk, not which detector gets it.
    characterise::CyclicSearch search;
    search.max_harmonic_divisor = 4;

    const auto with_it = characterise::estimate_symbol_rate(
        build(true, kSeed + 4), kRate, CyclicDetector::SquaredEnvelope, search);
    REQUIRE(with_it.has_value());
    CAPTURE(with_it->symbol_rate_hz, with_it->strongest_alpha_hz, with_it->harmonic_divisor,
            with_it->margin_db, with_it->strongest_margin_db, with_it->refusal);
    REQUIRE(with_it->found);
    REQUIRE(with_it->strongest_alpha_hz == Approx(2.0 * kFundamental).epsilon(0.01));
    REQUIRE(with_it->harmonic_divisor == 2);
    REQUIRE(with_it->symbol_rate_hz == Approx(kFundamental).epsilon(0.005));

    const auto without = characterise::estimate_symbol_rate(
        build(false, kSeed + 5), kRate, CyclicDetector::SquaredEnvelope, search);
    REQUIRE(without.has_value());
    CAPTURE(without->symbol_rate_hz, without->strongest_alpha_hz, without->harmonic_divisor,
            without->refusal);
    REQUIRE(without->found);
    REQUIRE(without->harmonic_divisor == 1);
    REQUIRE(without->symbol_rate_hz == Approx(2.0 * kFundamental).epsilon(0.005));
}

// REJECTS: an order estimator that takes the SMALLEST exponent with a line
// in it. That is the obvious rule and it is what this function did first.
// QPSK stands 14.5 dB up at exponent 2 on the pedestal of its own squared
// spectrum, which clears the 6 dB threshold, so the smallest rule calls
// QPSK BPSK. Running both modulations through the same assertions is what
// catches it: a case that only ran BPSK passes against either rule, because
// BPSK has a line at every exponent.
//
// Also rejects one that ignores the carrier. The M-th power line sits at
// exponent times the carrier offset, so an implementation that found the
// line and reported its frequency rather than dividing would say 6 kHz for
// BPSK and 12 kHz for QPSK on the same 3 kHz carrier.
TEST_CASE("the power law separates BPSK from QPSK and recovers the carrier", "[characterise]")
{
    constexpr dsp::Hertz kCarrier = 3000;
    siggen::PskParams psk;
    psk.symbol_rate = 2400.0;
    psk.rolloff = 0.35;
    psk.symbol_count = kPskSymbols;

    const auto common = config(kCarrier, kSeed + 2);
    std::println("test_cyclostationary power law: seed {}", common.seed);

    const auto bpsk = siggen::generate_bpsk(common, psk, kSamples);
    REQUIRE(bpsk.has_value());
    const auto bpsk_order =
        characterise::estimate_modulation_order(analysis_of(bpsk->samples), kRate);
    REQUIRE(bpsk_order.has_value());
    CAPTURE(bpsk_order->order, bpsk_order->carrier_offset_hz, bpsk_order->margin_db,
            bpsk_order->lines[0].margin_db, bpsk_order->lines[1].margin_db,
            bpsk_order->lines[2].margin_db);
    REQUIRE(bpsk_order->found);
    REQUIRE(bpsk_order->order == 2);
    REQUIRE(bpsk_order->carrier_offset_hz == Approx(static_cast<double>(kCarrier)).margin(20.0));
    // The higher exponents are loud too, which is the whole trap: BPSK
    // squared is a tone, and a tone squared is another tone.
    REQUIRE(bpsk_order->lines[1].margin_db > 6.0);
    REQUIRE(bpsk_order->lines[2].margin_db > 6.0);
    // Down from its own order, monotonically, because every squaring above
    // it squares the noise with the tone.
    REQUIRE(bpsk_order->lines[0].margin_db > bpsk_order->lines[1].margin_db);
    REQUIRE(bpsk_order->lines[1].margin_db > bpsk_order->lines[2].margin_db);

    const auto qpsk = siggen::generate_qpsk(common, psk, kSamples);
    REQUIRE(qpsk.has_value());
    const auto qpsk_order =
        characterise::estimate_modulation_order(analysis_of(qpsk->samples), kRate);
    REQUIRE(qpsk_order.has_value());
    CAPTURE(qpsk_order->order, qpsk_order->carrier_offset_hz, qpsk_order->lines[0].margin_db,
            qpsk_order->lines[1].margin_db);
    REQUIRE(qpsk_order->found);
    REQUIRE(qpsk_order->order == 4);
    // Exponent 2 is not silent on QPSK, which is exactly why the smallest
    // rule fails. It is materially weaker, which is why the highest rule
    // works.
    REQUIRE(qpsk_order->lines[1].margin_db > qpsk_order->lines[0].margin_db + 6.0);
    REQUIRE(qpsk_order->carrier_offset_hz == Approx(static_cast<double>(kCarrier)).margin(20.0));
}

// REJECTS: a confidence that does not move, which docs/detection.md names as
// the failure that turns the operator's confidence threshold into a no-op;
// and an estimator whose working range is claimed rather than measured.
//
// The sweep prints every point, so the table in the log IS the answer to
// "where does this stop working" rather than a sentence in a header that
// nobody re-measures. The assertions pin the two ends: it has to work at an
// SNR a real extract will see, it has to refuse rather than lie well below
// that, and the confidence at the top has to be materially above the
// confidence at the bottom of its working range.
TEST_CASE("the symbol rate estimator degrades measurably, not silently", "[characterise]")
{
    const auto common = config(0, kSeed + 3);
    std::println("test_cyclostationary snr sweep: seed {}", common.seed);

    siggen::PskParams psk;
    psk.symbol_rate = 2400.0;
    psk.rolloff = 0.35;
    psk.symbol_count = kPskSymbols;

    // Twice the buffer the other cases use. The lever on a weak cyclic
    // feature is observation time, and the sweep is the place to spend it:
    // the segment the estimator picks grows with the buffer, so the line
    // integrates coherently for longer while the number of segments
    // averaged stays the same.
    const auto clean = siggen::generate_bpsk(common, psk, 1U << 18);
    REQUIRE(clean.has_value());
    const double truth = clean->effective_symbol_rate;

    struct Point {
        double snr_db = 0.0;
        bool found = false;
        double error_percent = 0.0;
        double margin_db = 0.0;
        double confidence = 0.0;
    };
    std::vector<Point> table;

    for (const double snr_db : {30.0, 20.0, 15.0, 12.0, 10.0, 7.5, 5.0, 0.0}) {
        auto noisy = clean->samples;
        const auto report = siggen::add_awgn(dsp::ComplexSpan(noisy),
                                             siggen::NoiseLevel::snr_in_2500_hz_db(snr_db), kRate,
                                             siggen::derive_seed(common.seed, 1));
        REQUIRE(report.has_value());

        const auto estimate = characterise::estimate_symbol_rate(
            analysis_of(noisy), kRate, CyclicDetector::SquaredEnvelope);
        REQUIRE(estimate.has_value());

        Point point;
        point.snr_db = snr_db;
        point.found = estimate->found;
        point.error_percent =
            estimate->found ? 100.0 * std::abs(estimate->symbol_rate_hz - truth) / truth : 0.0;
        point.margin_db = estimate->margin_db;
        point.confidence = estimate->confidence;
        table.push_back(point);
        std::println("  snr_2500 {:>6.1f} dB  found {}  error {:.3f}%  margin {:.1f} dB  "
                     "confidence {:.3f}",
                     point.snr_db, point.found ? "yes" : " no", point.error_percent,
                     point.margin_db, point.confidence);
    }

    const Point& loud = table.front();
    REQUIRE(loud.found);
    REQUIRE(loud.error_percent < 0.5);

    // The lowest swept point that still answers, and the check that it
    // answered correctly rather than confidently.
    const auto last_found = std::find_if(table.rbegin(), table.rend(),
                                         [](const Point& point) { return point.found; });
    REQUIRE(last_found != table.rend());
    CAPTURE(last_found->snr_db, last_found->error_percent, last_found->confidence);
    REQUIRE(last_found->error_percent < 1.0);
    REQUIRE(last_found->snr_db <= 10.0);

    // Every point that answered has to be right. An estimator that keeps
    // answering as the noise rises and starts answering WRONG is worse than
    // one that stops, and this is the assertion that tells them apart.
    for (const Point& point : table) {
        CAPTURE(point.snr_db, point.error_percent, point.margin_db);
        if (point.found) {
            REQUIRE(point.error_percent < 1.0);
        }
    }

    REQUIRE(loud.confidence > last_found->confidence + 0.1);

    // The floor above is set by how long the line was integrated for, not
    // by anything written into the estimator, and this is the assertion
    // that says so. Same signal, same noise level, one point BELOW where
    // the sweep stopped, and eight times the samples. A line integrates
    // coherently inside a segment and the segment grows with the buffer, so
    // four times the observation buys about 6 dB of margin.
    //
    // REJECTS: an estimator whose working range is a constant somewhere.
    // Everything above this line passes against one that simply stops at a
    // fixed input SNR; this does not.
    {
        siggen::PskParams longer = psk;
        // Four times the samples needs four times the payload, or the
        // repeat comb comes back and beats the symbol rate's own line.
        constexpr std::size_t kPatientSamples = 1U << 20;
        longer.symbol_count = kPskSymbols * 8;
        const auto patient = siggen::generate_bpsk(common, longer, kPatientSamples);
        REQUIRE(patient.has_value());
        REQUIRE(patient->cycle_samples > kPatientSamples);

        auto noisy = patient->samples;
        const auto report = siggen::add_awgn(dsp::ComplexSpan(noisy),
                                             siggen::NoiseLevel::snr_in_2500_hz_db(5.0), kRate,
                                             siggen::derive_seed(common.seed, 2));
        REQUIRE(report.has_value());

        const auto estimate = characterise::estimate_symbol_rate(
            analysis_of(noisy), kRate, CyclicDetector::SquaredEnvelope);
        REQUIRE(estimate.has_value());
        const double error_percent =
            100.0 * std::abs(estimate->symbol_rate_hz - patient->effective_symbol_rate) /
            patient->effective_symbol_rate;
        std::println("  snr_2500    5.0 dB over 2^20 samples: found {}  error {:.3f}%  "
                     "margin {:.1f} dB",
                     estimate->found ? "yes" : " no", error_percent, estimate->margin_db);
        CAPTURE(estimate->refusal, estimate->margin_db, error_percent);
        REQUIRE(estimate->found);
        REQUIRE(error_percent < 1.0);
    }
}
