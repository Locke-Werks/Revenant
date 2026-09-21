#include "core/characterise/cyclostationary.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace revenant::characterise {
namespace {

// Bins either side of a nominal harmonic that the comb walk will accept as
// that harmonic. Two, because the parabolic interpolation on the strongest
// peak moves it by up to half a bin and the harmonic itself is subject to
// the same, so a tolerance of one can miss by rounding alone.
constexpr std::size_t kHarmonicToleranceBins = 2;

// Largest divisor the comb walk applies when the detector produces a comb.
//
// Twelve, which is high, and the risk of a large divisor runs the other way
// from the obvious guess. Taking divisor m requires every harmonic from the
// first to the m-th to clear the threshold, so the rule gets STRICTER as m
// grows: divisor 12 asks for twelve peaks in the right places and divisor 2
// asks for two. The false halving this guards against lives at m = 2, and
// it is bounded by kCombShortfallDb rather than by this.
//
// It has to reach this far because an impulse train's harmonics are equal
// in expectation, so the strongest one is chosen by estimation noise.
// Measured 2026-09-21 on a clean 1200 baud 2-FSK signal at 48 kS/s: the
// strongest peak landed on the FIFTH harmonic, 6000 Hz, and a divisor
// capped at four reported exactly that.
constexpr std::size_t kCombDetectorDivisor = 12;

// How far below the strongest peak a candidate fundamental may sit and still
// be taken as the fundamental.
//
// Twenty-five decibels. A real comb's fundamental is within a few decibels
// of its harmonics, so this is generous by a wide margin against the case it
// is meant to admit, and it excludes the case it is meant to reject: a bin
// that clears the threshold on pedestal roughness alone, tens of decibels
// under the line it is being asked to halve. Both bounds are the same
// argument from the other end, which is why one number does both jobs.
constexpr double kCombShortfallDb = 25.0;

// Bins in one local-baseline block. At a 8192-point segment and 48 kS/s this
// is 375 Hz, which follows the pulse spectrum's slope closely enough that a
// line on its shoulder still reads its own height, and is wide enough that a
// median over it is a percentile rather than a small sample.
constexpr std::size_t kBaselineBlock = 64;

// Minimum separation between two peaks counted as separate lines in the
// M-th power spectrum.
constexpr std::size_t kLineSeparationBins = 4;

// How far under the strongest line a second line may sit and still be
// counted as a companion of it.
//
// Ten decibels. The count exists to tell one carrier from several, and the
// case it has to get right is a two-tone FSK signal squared, whose two
// lines are within a decibel of each other because the two tones carry the
// same power. Counting every local maximum over the detection threshold
// instead counts the pedestal's own roughness: measured 2026-09-21 on a
// clean 2400 baud BPSK signal, the squared spectrum's line stood 53 dB up
// and the count came back well above one, so the order test refused a
// signal it had already identified.
constexpr double kCompanionRangeDb = 10.0;

// How far the winning exponent has to stand above every other exponent
// before the order is taken as settled.
//
// Three decibels. Measured, the real separations are 13.7 dB for BPSK and
// 11.9 dB for QPSK, so this is a guard against a tie rather than a
// discriminator doing work: below it the two exponents are saying the same
// thing and the honest answer is that the order is not established.
constexpr double kOrderSeparationDb = 3.0;

[[nodiscard]] double resolve_low(const CyclicSearch& search, SampleRate rate) {
    return search.min_symbol_rate_hz > 0.0 ? search.min_symbol_rate_hz
                                           : static_cast<double>(rate) / 1024.0;
}

[[nodiscard]] double resolve_high(const CyclicSearch& search, SampleRate rate) {
    return search.max_symbol_rate_hz > 0.0 ? search.max_symbol_rate_hz
                                           : static_cast<double>(rate) / 4.0;
}

[[nodiscard]] std::size_t resolve_divisor(const CyclicSearch& search, CyclicDetector detector) {
    if (search.max_harmonic_divisor > 0) {
        return search.max_harmonic_divisor;
    }
    switch (detector) {
        case CyclicDetector::SquaredEnvelope:
            return 1;
        case CyclicDetector::FrequencyTransition:
            return kCombDetectorDivisor;
    }
    return 1;
}

// What a caller is told to do when a detector's feature is empty. Named per
// detector because the two empty for different reasons and the fix differs.
[[nodiscard]] std::string_view empty_feature_cause(CyclicDetector detector) {
    switch (detector) {
        case CyclicDetector::SquaredEnvelope:
            return "the envelope is constant, so the symbol rate is not in the amplitude at "
                   "all. This is every FSK and CPM waveform in docs/modes.md. Run "
                   "CyclicDetector::FrequencyTransition instead";
        case CyclicDetector::FrequencyTransition:
            return "the instantaneous frequency is steady, so nothing is stepping between "
                   "tones. An unmodulated carrier looks exactly like this, and so does an "
                   "extract that missed the signal. Check the extract is on the signal, then "
                   "run CyclicDetector::SquaredEnvelope for a linear modulation";
    }
    return "the feature carries nothing";
}

}  // namespace

std::string_view cyclic_detector_name(CyclicDetector detector)
{
    switch (detector) {
        case CyclicDetector::SquaredEnvelope:
            return "squared envelope";
        case CyclicDetector::FrequencyTransition:
            return "frequency transition";
    }
    return "unknown";
}

CyclicFeature squared_envelope_feature(std::span<const Complex64> samples)
{
    CyclicFeature feature;
    feature.units = "fraction of mean power";
    feature.values.assign(samples.size(), 0.0);
    if (samples.empty()) {
        return feature;
    }

    double mean = 0.0;
    for (std::size_t n = 0; n < samples.size(); ++n) {
        feature.values[n] = std::norm(samples[n]);
        mean += feature.values[n];
    }
    mean /= static_cast<double>(samples.size());
    if (!(mean > 0.0)) {
        std::fill(feature.values.begin(), feature.values.end(), 0.0);
        return feature;
    }

    // Divided by the mean power as well as demeaned, so the ripple below is
    // a dimensionless fraction and the floor it is compared against does not
    // move with the extract's level.
    double sum_squares = 0.0;
    for (double& value : feature.values) {
        value = (value - mean) / mean;
        sum_squares += value * value;
    }
    feature.ripple = std::sqrt(sum_squares / static_cast<double>(feature.values.size()));
    return feature;
}

CyclicFeature frequency_transition_feature(std::span<const Complex64> samples)
{
    CyclicFeature feature;
    feature.units = "radians of phase curvature a sample";
    // Two differences, so the feature is two samples shorter than the input.
    if (samples.size() < 3) {
        return feature;
    }

    // Instantaneous frequency as the argument of the one-sample product,
    // which is the phase advance per sample and wraps at pi. A deviation
    // beyond rate/2 cannot be represented in a buffer at that rate in any
    // case, so the wrap is the Nyquist limit rather than a defect here.
    std::vector<double> instantaneous(samples.size() - 1, 0.0);
    for (std::size_t n = 1; n < samples.size(); ++n) {
        instantaneous[n - 1] = std::arg(samples[n] * std::conj(samples[n - 1]));
    }

    feature.values.assign(instantaneous.size() - 1, 0.0);
    double mean = 0.0;
    for (std::size_t n = 1; n < instantaneous.size(); ++n) {
        // Absolute value, so a transition upward and one downward both
        // produce a positive impulse. Without it the impulse train carries
        // the sign of the data and averages away rather than adding up.
        double step = instantaneous[n] - instantaneous[n - 1];
        // The difference of two arguments wraps at 2*pi. Folding it back is
        // what stops a signal whose instantaneous frequency crosses the
        // Nyquist edge producing an impulse on every sample.
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        while (step > kTwoPi / 2.0) {
            step -= kTwoPi;
        }
        while (step < -kTwoPi / 2.0) {
            step += kTwoPi;
        }
        feature.values[n - 1] = std::abs(step);
        mean += feature.values[n - 1];
    }
    mean /= static_cast<double>(feature.values.size());
    double sum_squares = 0.0;
    for (double& value : feature.values) {
        value -= mean;
        sum_squares += value * value;
    }
    feature.ripple = std::sqrt(sum_squares / static_cast<double>(feature.values.size()));
    return feature;
}

Expected<SymbolRateEstimate> estimate_symbol_rate(std::span<const Complex64> samples,
                                                  SampleRate rate,
                                                  CyclicDetector detector,
                                                  const CyclicSearch& search)
{
    if (rate <= 0) {
        return fail(std::format("estimate_symbol_rate: sample rate must be positive, was {}",
                                rate));
    }

    const double low = resolve_low(search, rate);
    const double high = resolve_high(search, rate);
    if (!(low > 0.0) || !(high > low)) {
        return fail(std::format(
            "estimate_symbol_rate: the search band {:.1f} to {:.1f} Hz is empty. Set "
            "CyclicSearch::min_symbol_rate_hz below max_symbol_rate_hz, or leave both at zero "
            "for the default band of rate/1024 to rate/4.",
            low, high));
    }
    if (high > static_cast<double>(rate) / 2.0) {
        return fail(std::format(
            "estimate_symbol_rate: the search reaches {:.1f} Hz, above the {:.1f} Hz Nyquist "
            "limit of a {} S/s buffer. Lower CyclicSearch::max_symbol_rate_hz, or characterise "
            "an extract taken at a higher rate.",
            high, static_cast<double>(rate) / 2.0, rate));
    }

    CyclicFeature feature;
    switch (detector) {
        case CyclicDetector::SquaredEnvelope:
            feature = squared_envelope_feature(samples);
            break;
        case CyclicDetector::FrequencyTransition:
            feature = frequency_transition_feature(samples);
            break;
    }

    SymbolRateEstimate estimate;
    estimate.detector = detector;
    estimate.searched_low_hz = low;
    estimate.searched_high_hz = high;
    estimate.feature_ripple = feature.ripple;
    estimate.feature_units = feature.units;

    if (feature.ripple < kMinFeatureRipple) {
        estimate.refusal = std::format(
            "{} detector: the feature measures {:.3g} {} against a floor of {:.3g}, so {}. "
            "Below that floor the spectrum is float32 rounding structure, and a margin over "
            "it is a real ratio of nothing to nothing.",
            cyclic_detector_name(detector), feature.ripple, feature.units, kMinFeatureRipple,
            empty_feature_cause(detector));
        return estimate;
    }

    const std::size_t segment = analysis_segment(feature.values.size());
    auto spectrum = welch_spectrum_real(feature.values, rate, segment);
    if (!spectrum.has_value()) {
        return std::unexpected(with_context(spectrum.error(), "estimate_symbol_rate"));
    }
    estimate.resolution_hz = spectrum->bin_width_hz;

    const std::vector<double> baseline = local_baseline(spectrum->bins, kBaselineBlock);
    const SpectralPeak strongest = strongest_peak(*spectrum, baseline, low, high);
    if (!strongest.found) {
        estimate.refusal = std::format(
            "{} detector: no bin at all fell inside the search band {:.1f} to {:.1f} Hz, at a "
            "resolution of {:.2f} Hz. Widen CyclicSearch, or characterise a longer extract.",
            cyclic_detector_name(detector), low, high, spectrum->bin_width_hz);
        return estimate;
    }
    estimate.strongest_alpha_hz = strongest.frequency_hz;
    estimate.strongest_margin_db = strongest.margin_db;

    if (strongest.margin_db < search.threshold_db) {
        estimate.refusal = std::format(
            "{} detector: the strongest cycle frequency in {:.1f} to {:.1f} Hz is {:.1f} Hz at "
            "{:.1f} dB over its own local baseline, under the {:.1f} dB threshold. Either "
            "nothing is transmitting, or the symbol rate is outside that band, or the extract "
            "is too weak. Widen CyclicSearch, lengthen the extract past its {} samples, or "
            "narrow the receiver onto the signal.",
            cyclic_detector_name(detector), low, high, strongest.frequency_hz,
            strongest.margin_db, search.threshold_db, samples.size());
        return estimate;
    }

    // The comb walk. Largest divisor first, so the first divisor whose whole
    // comb is present is the smallest fundamental consistent with the
    // evidence. A divisor is taken only when every harmonic under the
    // strongest peak is there AND the candidate fundamental is within
    // kCombShortfallDb of the strongest peak, which is what separates a real
    // comb from a bin that cleared the threshold on pedestal roughness.
    const std::size_t max_divisor = resolve_divisor(search, detector);
    for (std::size_t divisor = max_divisor; divisor >= 1; --divisor) {
        const double candidate = strongest.frequency_hz / static_cast<double>(divisor);
        if (candidate < low) {
            continue;
        }
        bool comb_present = true;
        SpectralPeak fundamental;
        for (std::size_t harmonic = 1; harmonic <= divisor; ++harmonic) {
            const SpectralPeak found =
                peak_near(*spectrum, baseline, candidate * static_cast<double>(harmonic),
                          kHarmonicToleranceBins);
            if (!found.found || found.margin_db < search.threshold_db) {
                comb_present = false;
                break;
            }
            if (harmonic == 1) {
                fundamental = found;
            }
        }
        if (!comb_present) {
            continue;
        }
        if (fundamental.margin_db < strongest.margin_db - kCombShortfallDb) {
            continue;
        }
        estimate.found = true;
        estimate.symbol_rate_hz = fundamental.frequency_hz;
        estimate.margin_db = fundamental.margin_db;
        estimate.confidence = margin_confidence(fundamental.margin_db, search.threshold_db);
        estimate.harmonic_divisor = divisor;
        break;
    }

    if (!estimate.found) {
        estimate.refusal = std::format(
            "{} detector: the strongest cycle frequency {:.1f} Hz stands {:.1f} dB up, but no "
            "divisor of it up to {} has a complete harmonic comb under it and it is itself "
            "below the search band's low edge of {:.1f} Hz. Lower "
            "CyclicSearch::min_symbol_rate_hz, or characterise a decimated extract so the "
            "symbol rate is not a thousandth of the way along the axis.",
            cyclic_detector_name(detector), strongest.frequency_hz, strongest.margin_db,
            max_divisor, low);
    }

    return estimate;
}

namespace {

// Peaks in a spectrum clearing the threshold, as local maxima separated by
// at least kLineSeparationBins. Counting every bin over the threshold would
// count one line's main lobe several times.
[[nodiscard]] std::size_t count_lines(const PowerSpectrum& spectrum,
                                      std::span<const double> baseline,
                                      double threshold_db,
                                      double strongest_margin_db)
{
    const double bar = std::max(threshold_db, strongest_margin_db - kCompanionRangeDb);
    std::size_t count = 0;
    std::size_t last = 0;
    bool have_last = false;
    for (std::size_t k = 1; k + 1 < spectrum.bins.size(); ++k) {
        if (spectrum.bins[k] <= spectrum.bins[k - 1] || spectrum.bins[k] < spectrum.bins[k + 1]) {
            continue;
        }
        const double margin =
            10.0 * std::log10(std::max(spectrum.bins[k], 1e-30) / std::max(baseline[k], 1e-30));
        if (margin < bar) {
            continue;
        }
        if (have_last && k - last < kLineSeparationBins) {
            continue;
        }
        ++count;
        last = k;
        have_last = true;
    }
    return count;
}

}  // namespace

Expected<ModulationOrder> estimate_modulation_order(std::span<const Complex64> samples,
                                                    SampleRate rate,
                                                    double threshold_db)
{
    if (rate <= 0) {
        return fail(std::format("estimate_modulation_order: sample rate must be positive, was {}",
                                rate));
    }
    if (samples.empty()) {
        return fail("estimate_modulation_order: the buffer is empty");
    }

    double mean_power = 0.0;
    for (const Complex64& sample : samples) {
        mean_power += std::norm(sample);
    }
    mean_power /= static_cast<double>(samples.size());
    if (!(mean_power > 0.0)) {
        return fail("estimate_modulation_order: the buffer carries no power at all, so there is "
                    "nothing to raise to a power. Check the extract reached this call.");
    }
    const double scale = 1.0 / std::sqrt(mean_power);

    std::vector<Complex64> normalised(samples.size());
    for (std::size_t n = 0; n < samples.size(); ++n) {
        normalised[n] = samples[n] * scale;
    }

    const std::size_t segment = analysis_segment(normalised.size());
    ModulationOrder order;
    std::vector<Complex64> raised(normalised.size());

    constexpr std::array<int, 3> kExponents{2, 4, 8};
    for (std::size_t slot = 0; slot < kExponents.size(); ++slot) {
        const int exponent = kExponents[slot];
        for (std::size_t n = 0; n < normalised.size(); ++n) {
            Complex64 value = normalised[n];
            Complex64 accumulated(1.0, 0.0);
            for (int power = 0; power < exponent; ++power) {
                accumulated *= value;
            }
            raised[n] = accumulated;
        }

        auto spectrum = welch_spectrum(raised, rate, segment);
        if (!spectrum.has_value()) {
            return std::unexpected(with_context(spectrum.error(), "estimate_modulation_order"));
        }
        const std::vector<double> baseline = local_baseline(spectrum->bins, kBaselineBlock);
        const SpectralPeak peak = strongest_peak(*spectrum, baseline,
                                                 -static_cast<double>(rate),
                                                 static_cast<double>(rate));

        PowerLawLine& line = order.lines[slot];
        line.exponent = exponent;
        if (!peak.found) {
            continue;
        }
        line.frequency_hz = peak.frequency_hz;
        line.carrier_offset_hz = peak.frequency_hz / static_cast<double>(exponent);
        line.margin_db = peak.margin_db;
        line.found = peak.margin_db >= threshold_db;
        line.line_count = count_lines(*spectrum, baseline, threshold_db, peak.margin_db);

    }

    // The exponent whose line stands highest, not the smallest one that has
    // a line at all.
    //
    // Smallest was the first rule here and it is wrong in both directions.
    // Squaring BPSK gives a pure tone and squaring that tone again gives
    // another one, so BPSK has a line at every exponent and smallest gets it
    // right by luck. QPSK measured 14.5 dB at exponent 2, which clears the
    // threshold on pedestal structure alone, so smallest calls QPSK BPSK.
    //
    // Highest is the physical statement. At the constellation's own order
    // the modulation cancels exactly and what is left is a tone; above it,
    // the tone is being squared along with its noise and degrades every
    // time. Measured 2026-09-21, exponents 2, 4 and 8: BPSK 51.2, 37.5,
    // 27.2 dB, monotonically down from its own order; QPSK 14.5, 26.4, up
    // to its order and then down. The winner has to beat every other
    // exponent by kOrderSeparationDb or the answer is refused as ambiguous
    // rather than settled by a decibel.
    std::size_t best = order.lines.size();
    double runner_up_db = 0.0;
    for (std::size_t slot = 0; slot < order.lines.size(); ++slot) {
        const PowerLawLine& line = order.lines[slot];
        if (!line.found) {
            continue;
        }
        if (best == order.lines.size() || line.margin_db > order.lines[best].margin_db) {
            if (best != order.lines.size()) {
                runner_up_db = std::max(runner_up_db, order.lines[best].margin_db);
            }
            best = slot;
        } else {
            runner_up_db = std::max(runner_up_db, line.margin_db);
        }
    }

    if (best != order.lines.size() &&
        order.lines[best].margin_db >= runner_up_db + kOrderSeparationDb) {
        const PowerLawLine& line = order.lines[best];
        order.found = true;
        order.order = line.exponent;
        order.carrier_offset_hz = line.carrier_offset_hz;
        order.margin_db = line.margin_db;
        order.confidence = margin_confidence(line.margin_db, threshold_db);
    }

    return order;
}

}  // namespace revenant::characterise
