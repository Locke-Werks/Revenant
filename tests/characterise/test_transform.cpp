// The spectral primitives the rest of core/characterise is built on.
//
// These cases exist because every estimator above them reports a margin over
// a local baseline, and a margin is only a measurement if the transform, the
// normalisation and the baseline are each right on their own. A symbol-rate
// estimator tested end to end against a clean signal passes with a broken
// baseline, because a clean signal's line stands 30 dB up and clears any
// threshold. The cases that matter are the ones with no signal in them.
//
// Each case names, in its own comment, the wrong implementation it rejects.
// docs/conventions.md does not ask for that; the project has written nine
// tests that passed against any plausible implementation and this directory
// is trying not to write the tenth.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <print>
#include <vector>

#include "core/characterise/transform.h"
#include "core/dsp/synth/modulators.h"
#include "tests/characterise/signal_lab.h"

using namespace revenant;
using characterise::Complex64;
using Catch::Approx;

namespace {

// Fixed and printed, per docs/conventions.md. A failure that cannot be
// reproduced from its printed seed is an anecdote.
constexpr std::uint64_t kSeed = 20260921;

constexpr dsp::SampleRate kRate = 48000;

[[nodiscard]] std::vector<Complex64> analysis_of(const std::vector<dsp::Complex32>& samples)
{
    return characterise::to_analysis(dsp::ConstComplexSpan(samples));
}

}  // namespace

// REJECTS: a transform with the twiddle sign reversed, which is an inverse
// transform wearing a forward transform's name and lands the line at
// size - bin; and one that omits the bit-reversed load, which lands it at
// the bit reversal of the bin. Both produce a spectrum that looks entirely
// reasonable and is wrong about where everything is. A test that only
// checked the peak magnitude would pass against both.
TEST_CASE("the transform puts a complex sinusoid in the bin it belongs to", "[characterise]")
{
    constexpr std::size_t kSize = 1024;
    constexpr std::size_t kBin = 37;

    for (const int sign : {1, -1}) {
        std::vector<Complex64> data(kSize);
        for (std::size_t n = 0; n < kSize; ++n) {
            const double phase = 2.0 * std::numbers::pi * static_cast<double>(sign) *
                                 static_cast<double>(kBin) * static_cast<double>(n) /
                                 static_cast<double>(kSize);
            data[n] = Complex64(std::cos(phase), std::sin(phase));
        }
        characterise::fft_in_place(data);

        const std::size_t expected = sign > 0 ? kBin : kSize - kBin;
        std::size_t loudest = 0;
        for (std::size_t k = 1; k < kSize; ++k) {
            if (std::norm(data[k]) > std::norm(data[loudest])) {
                loudest = k;
            }
        }
        CAPTURE(sign, expected, loudest);
        REQUIRE(loudest == expected);
        REQUIRE(std::abs(data[expected]) == Approx(static_cast<double>(kSize)).epsilon(1e-9));

        double worst_other = 0.0;
        for (std::size_t k = 0; k < kSize; ++k) {
            if (k != expected) {
                worst_other = std::max(worst_other, std::abs(data[k]));
            }
        }
        REQUIRE(worst_other < 1e-8 * static_cast<double>(kSize));
    }
}

// REJECTS: a normalisation that divides by the segment length, or by the
// window's coherent gain, instead of by sum(w^2). Either one makes the same
// white noise read a different level at a different segment length, and
// every peak-over-baseline threshold in this directory is then a threshold
// that moves when the buffer length does. A single-segment-length test
// cannot see this at all.
TEST_CASE("the noise floor reads the same at any segment length", "[characterise]")
{
    std::println("test_transform noise floor: seed {}", kSeed);
    constexpr double kVariance = 4.0;
    const auto noise = characterise_test::gaussian_noise(1U << 18, kVariance, kSeed);
    const auto widened = analysis_of(noise);

    for (const std::size_t segment : {std::size_t{1024}, std::size_t{16384}}) {
        const auto spectrum = characterise::welch_spectrum(widened, kRate, segment);
        REQUIRE(spectrum.has_value());
        double sum = 0.0;
        for (const double bin : spectrum->bins) {
            sum += bin;
        }
        const double mean = sum / static_cast<double>(spectrum->bins.size());
        CAPTURE(segment, spectrum->segments_averaged, mean);
        REQUIRE(mean == Approx(kVariance).epsilon(0.03));
    }
}

// REJECTS: an analysis_segment that takes the whole buffer as one segment,
// which is the obvious choice because it gives the finest cycle-frequency
// resolution. The second half of this case is what makes the first half mean
// something: on that choice the largest bin in pure noise stands more than
// 6 dB over its own baseline, which is the detection threshold in
// cyclostationary.h, so the characteriser would report a symbol rate for an
// empty channel. A case that only asserted the good configuration would pass
// against the bad one too, because it never runs it.
TEST_CASE("pure noise produces no margin the estimators would call a feature", "[characterise]")
{
    std::println("test_transform noise margin: seed {}", kSeed + 1);
    constexpr std::size_t kCount = 1U << 16;
    const auto noise = characterise_test::gaussian_noise(kCount, 1.0, kSeed + 1);
    const auto widened = analysis_of(noise);

    const std::size_t segment = characterise::analysis_segment(kCount);
    REQUIRE(segment == 4096);

    const auto averaged = characterise::welch_spectrum(widened, kRate, segment);
    REQUIRE(averaged.has_value());
    REQUIRE(averaged->segments_averaged >= 24);
    const auto baseline = characterise::local_baseline(averaged->bins, 256);
    const auto peak = characterise::strongest_peak(*averaged, baseline,
                                                   -static_cast<double>(kRate),
                                                   static_cast<double>(kRate));
    REQUIRE(peak.found);
    CAPTURE(segment, averaged->segments_averaged, peak.margin_db);
    REQUIRE(peak.margin_db < 4.0);

    const auto single = characterise::welch_spectrum(widened, kRate, kCount);
    REQUIRE(single.has_value());
    REQUIRE(single->segments_averaged == 1);
    const auto single_baseline = characterise::local_baseline(single->bins, 256);
    const auto single_peak = characterise::strongest_peak(*single, single_baseline,
                                                          -static_cast<double>(kRate),
                                                          static_cast<double>(kRate));
    REQUIRE(single_peak.found);
    CAPTURE(single_peak.margin_db);
    REQUIRE(single_peak.margin_db > 6.0);
}

// REJECTS: a peak search by raw power. The signal here is a broad lump of
// energy near DC with a weak line out on its shoulder, which is the shape
// the squared envelope of a shaped PSK signal actually has: the lump is the
// pulse spectrum and the line is the symbol rate. Measured on this case, the
// loudest bin sits at 41 Hz and carries about seven times the power of the
// line at 5 kHz, so a raw-power search reports 41 Hz and a margin search
// reports the symbol rate. The last two assertions are what stop the first
// one being free: they check the loudest bin really is somewhere else and
// really is louder, so the two searches are being told apart rather than
// agreeing.
//
// The line is deliberately weak. Its margin here is under 10 dB, against the
// 6 dB threshold cyclostationary.h uses, so this case runs near the
// operating point rather than at a level any search would find.
TEST_CASE("the peak search finds a line on a pedestal, not the pedestal", "[characterise]")
{
    std::println("test_transform pedestal: seed {}", kSeed + 2);
    constexpr std::size_t kCount = 1U << 17;
    constexpr double kLineHz = 5000.0;

    const auto noise = characterise_test::gaussian_noise(kCount + 16, 1.0, kSeed + 2);
    std::vector<double> signal(kCount, 0.0);
    for (std::size_t n = 0; n < kCount; ++n) {
        // A sixteen-tap moving average, which is a lowpass with its first
        // null at rate/16 and most of its energy under 1.5 kHz.
        double smoothed = 0.0;
        for (std::size_t tap = 0; tap < 16; ++tap) {
            smoothed += static_cast<double>(noise[n + tap].real());
        }
        const double phase = 2.0 * std::numbers::pi * kLineHz * static_cast<double>(n) /
                             static_cast<double>(kRate);
        signal[n] = smoothed / 16.0 + 0.010 * std::cos(phase);
    }

    const auto spectrum = characterise::welch_spectrum_real(signal, kRate,
                                                            characterise::analysis_segment(kCount));
    REQUIRE(spectrum.has_value());
    const auto baseline = characterise::local_baseline(spectrum->bins, 128);
    const auto peak = characterise::strongest_peak(*spectrum, baseline, 100.0, 20000.0);
    REQUIRE(peak.found);
    CAPTURE(peak.frequency_hz, peak.margin_db);
    REQUIRE(peak.frequency_hz == Approx(kLineHz).margin(10.0));
    REQUIRE(peak.margin_db > 7.0);

    std::size_t loudest = 0;
    for (std::size_t k = 1; k < spectrum->bins.size(); ++k) {
        if (spectrum->bins[k] > spectrum->bins[loudest]) {
            loudest = k;
        }
    }
    const double loudest_hz = spectrum->frequency_at(static_cast<double>(loudest));
    CAPTURE(loudest_hz);
    REQUIRE(loudest_hz < 2000.0);
    REQUIRE(loudest != peak.bin);
    REQUIRE(spectrum->bins[loudest] > 5.0 * peak.power);
}

// REJECTS: a concentration measured from the single loudest bin. The tone
// here sits half a bin off centre, which is the worst case for a Hann
// window and costs 1.4 dB of scalloping, so a one-bin reading is about 0.52
// and would put an unmodulated carrier below any threshold worth setting.
// The three-bin window is the whole content of that function and a
// bin-centred tone would not exercise it.
TEST_CASE("an off-centre carrier still reads as concentrated", "[characterise]")
{
    constexpr std::size_t kSegment = 4096;
    constexpr std::size_t kCount = kSegment * 16;
    // 4096 points at 48 kS/s is 11.71875 Hz a bin, so 1002 Hz lands 0.504 of
    // a bin above centre. Integer hertz, per docs/conventions.md, which is
    // why the offset is chosen by picking the rate and the bin rather than
    // by writing a fractional frequency.
    const auto tone = characterise_test::pure_tone(kCount, kRate, 1002, 1.0);
    const auto spectrum = characterise::welch_spectrum(analysis_of(tone), kRate, kSegment);
    REQUIRE(spectrum.has_value());
    const double concentration = characterise::spectral_concentration(*spectrum);
    CAPTURE(concentration);
    REQUIRE(concentration > 0.9);

    const auto noise = characterise_test::gaussian_noise(kCount, 1.0, kSeed + 3);
    const auto noise_spectrum = characterise::welch_spectrum(analysis_of(noise), kRate, kSegment);
    REQUIRE(noise_spectrum.has_value());
    const double noise_concentration = characterise::spectral_concentration(*noise_spectrum);
    CAPTURE(noise_concentration);
    REQUIRE(noise_concentration < 0.01);
}

// REJECTS: an occupied-bandwidth measurement that returns the span it was
// given, which is what a containment search returns when the noise floor is
// subtracted wrongly or not at all. The ground truth here is not another
// measurement: siggen::occupied_extent states the band from the spec's own
// parameters, which for a root raised cosine is (1 + rolloff) times the
// symbol rate, so the assertion is against arithmetic.
TEST_CASE("occupied bandwidth recovers a shaped signal's own extent", "[characterise]")
{
    siggen::ModulatorConfig common;
    common.rate = kRate;
    common.carrier_offset = 0;
    common.seed = kSeed + 4;
    std::println("test_transform occupied band: seed {}", common.seed);

    siggen::PskParams psk;
    psk.symbol_rate = 2400.0;
    psk.rolloff = 0.35;
    psk.symbol_count = 256;

    const auto generated = siggen::generate_bpsk(common, psk, 1U << 17);
    REQUIRE(generated.has_value());
    const double nominal = static_cast<double>(generated->extent.bandwidth_hz());
    REQUIRE(nominal == Approx(3240.0).margin(10.0));

    const auto spectrum = characterise::welch_spectrum(
        analysis_of(generated->samples), kRate,
        characterise::analysis_segment(generated->samples.size()));
    REQUIRE(spectrum.has_value());
    const auto band = characterise::occupied_band(*spectrum, 0.99);
    REQUIRE(band.found);
    CAPTURE(band.low_hz, band.high_hz, band.bandwidth_hz, nominal);
    REQUIRE(band.bandwidth_hz == Approx(nominal).epsilon(0.20));
    REQUIRE(band.centre_hz == Approx(0.0).margin(150.0));
}

// REJECTS: a Welch that zero-pads a short buffer up to one segment and
// returns a spectrum anyway. That spectrum is a single unaveraged
// periodogram of mostly zeros, its largest bin stands 10 dB over its own
// median, and the caller has no way to know. The refusal has to name both
// numbers or the caller cannot act on it, which is the rule the project
// learned on a real radio: a refusal names the number, the cause and the
// fix.
TEST_CASE("a buffer shorter than one segment is refused, out loud", "[characterise]")
{
    const std::vector<Complex64> tiny(100, Complex64(1.0, 0.0));
    const auto spectrum = characterise::welch_spectrum(tiny, kRate, 1024);
    REQUIRE_FALSE(spectrum.has_value());
    const std::string& message = spectrum.error().message;
    CAPTURE(message);
    REQUIRE(message.find("100") != std::string::npos);
    REQUIRE(message.find("1024") != std::string::npos);
}
