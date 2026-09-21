#include "core/characterise/tones.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace revenant::characterise {
namespace {

// Fewest kept samples the histogram will be built from. Below this the bin
// counts are too small for the smoothing to mean anything and the mode
// prominence test is measuring shot noise.
constexpr std::size_t kMinKeptSamples = 4096;

// Percentiles the histogram's range is taken from, then widened by
// kRangeMargin either side. Not the extremes: one sample of phase noise
// across an envelope null that slipped past the gate would otherwise set
// the range and leave every tone in one bin.
constexpr double kRangeLowPercentile = 0.005;
constexpr double kRangeHighPercentile = 0.995;
constexpr double kRangeMargin = 0.25;

// Interquartile range over this is the standard deviation of a Gaussian.
constexpr double kIqrToSigma = 1.349;

// How many of the buffer's own frequency resolutions the instantaneous
// frequency has to span before there is anything to build a histogram over.
//
// An unmodulated carrier's instantaneous frequency is one value, and in
// float32 it is one value plus rounding: measured 2026-09-21 on a clean
// 1200 Hz carrier at 48 kS/s, the whole distribution spanned 8e-5 Hz and
// the histogram built over that range broke that rounding into five modes
// with empty valleys between them. Every structural rule below passed.
//
// A buffer of N samples resolves rate/N, and no tone set closer together
// than that is separable by any method, so sixteen of them is a floor with
// a wide margin under any real shift and far above the last bit of a float.
constexpr double kMinSpanResolutions = 16.0;

[[nodiscard]] double percentile_of_sorted(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1, sorted.size() - 1);
    const double blend = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - blend) + sorted[upper] * blend;
}

}  // namespace

EnvelopeStats envelope_stats(std::span<const Complex64> samples)
{
    EnvelopeStats stats;
    if (samples.empty()) {
        return stats;
    }

    double sum = 0.0;
    double sum_squares = 0.0;
    double peak = 0.0;
    for (const Complex64& sample : samples) {
        const double power = std::norm(sample);
        sum += power;
        sum_squares += power * power;
        peak = std::max(peak, power);
    }
    const auto count = static_cast<double>(samples.size());
    stats.mean_power = sum / count;
    if (!(stats.mean_power > 0.0)) {
        return stats;
    }

    const double variance = std::max(sum_squares / count - stats.mean_power * stats.mean_power,
                                     0.0);
    stats.normalised_power_variance = variance / (stats.mean_power * stats.mean_power);
    stats.peak_to_average_db = 10.0 * std::log10(peak / stats.mean_power);
    return stats;
}

Expected<ToneStructure> estimate_tone_structure(std::span<const Complex64> samples,
                                                SampleRate rate,
                                                const ToneSearch& search)
{
    if (rate <= 0) {
        return fail(std::format("estimate_tone_structure: sample rate must be positive, was {}",
                                rate));
    }
    if (search.bins < 16) {
        return fail(std::format(
            "estimate_tone_structure: {} histogram bins is too few to separate two modes. Set "
            "ToneSearch::bins to at least 16; the default is 256.",
            search.bins));
    }
    if (samples.size() < 3) {
        return fail(std::format(
            "estimate_tone_structure: {} samples cannot produce an instantaneous frequency, "
            "which needs at least three.",
            samples.size()));
    }

    ToneStructure structure;

    const EnvelopeStats envelope = envelope_stats(samples);
    if (!(envelope.mean_power > 0.0)) {
        return fail("estimate_tone_structure: the buffer carries no power at all. Check the "
                    "extract reached this call.");
    }
    const double gate = search.amplitude_gate * envelope.mean_power;

    // Instantaneous frequency in hertz, keeping only the pairs where both
    // samples stand above the gate.
    const double to_hertz = static_cast<double>(rate) / (2.0 * std::numbers::pi);
    std::vector<double> frequency;
    frequency.reserve(samples.size());
    for (std::size_t n = 1; n < samples.size(); ++n) {
        if (std::norm(samples[n]) < gate || std::norm(samples[n - 1]) < gate) {
            continue;
        }
        frequency.push_back(std::arg(samples[n] * std::conj(samples[n - 1])) * to_hertz);
    }
    structure.samples_kept = frequency.size();
    structure.samples_gated = samples.size() - 1 - frequency.size();

    if (frequency.size() < kMinKeptSamples) {
        structure.refusal = std::format(
            "only {} of {} sample pairs stood above the {:.0f} percent amplitude gate, under "
            "the {} needed for a histogram. The extract is mostly silence, or its level is "
            "dominated by a few loud samples. Lower ToneSearch::amplitude_gate, or take a "
            "longer extract over the signal's own transmission rather than across its gaps.",
            frequency.size(), samples.size() - 1, search.amplitude_gate * 100.0,
            kMinKeptSamples);
        return structure;
    }

    std::vector<double> sorted(frequency);
    std::sort(sorted.begin(), sorted.end());
    const double low_percentile = percentile_of_sorted(sorted, kRangeLowPercentile);
    const double high_percentile = percentile_of_sorted(sorted, kRangeHighPercentile);
    structure.frequency_spread_hz =
        (percentile_of_sorted(sorted, 0.75) - percentile_of_sorted(sorted, 0.25)) / kIqrToSigma;

    const double span = high_percentile - low_percentile;
    const double resolvable =
        kMinSpanResolutions * static_cast<double>(rate) / static_cast<double>(frequency.size());
    if (!(span > resolvable)) {
        structure.refusal = std::format(
            "the instantaneous frequency spans {:.4g} Hz across all {} kept samples, under the "
            "{:.4g} Hz this buffer can resolve. That is an unmodulated carrier, and the "
            "structure a histogram would find in a span that small is the last bit of a float. "
            "Characterise a longer extract if the shift is genuinely this narrow.",
            span, frequency.size(), resolvable);
        return structure;
    }

    structure.histogram_low_hz = low_percentile - kRangeMargin * span;
    structure.histogram_high_hz = high_percentile + kRangeMargin * span;
    const double width = structure.histogram_high_hz - structure.histogram_low_hz;
    const double bin_width = width / static_cast<double>(search.bins);

    std::vector<double> histogram(search.bins, 0.0);
    for (const double value : frequency) {
        const double position = (value - structure.histogram_low_hz) / bin_width;
        if (position < 0.0) {
            continue;
        }
        const auto bin = static_cast<std::size_t>(position);
        if (bin < histogram.size()) {
            histogram[bin] += 1.0;
        }
    }

    // Smoothed with a centred moving average. The unsmoothed counts are
    // Poisson, so a flat histogram of noise has maxima wherever its own
    // square root puts them, and every structural test below would then be
    // reading shot noise.
    std::vector<double> smoothed(histogram.size(), 0.0);
    const auto half = static_cast<std::ptrdiff_t>(std::max<std::size_t>(search.smoothing_bins, 1) /
                                                  2);
    for (std::ptrdiff_t k = 0; k < static_cast<std::ptrdiff_t>(histogram.size()); ++k) {
        double sum = 0.0;
        double weight = 0.0;
        for (std::ptrdiff_t offset = -half; offset <= half; ++offset) {
            const std::ptrdiff_t index = k + offset;
            if (index < 0 || index >= static_cast<std::ptrdiff_t>(histogram.size())) {
                continue;
            }
            sum += histogram[static_cast<std::size_t>(index)];
            weight += 1.0;
        }
        smoothed[static_cast<std::size_t>(k)] = weight > 0.0 ? sum / weight : 0.0;
    }

    double tallest = 0.0;
    double mean_height = 0.0;
    for (const double value : smoothed) {
        tallest = std::max(tallest, value);
        mean_height += value;
    }
    mean_height /= static_cast<double>(smoothed.size());

    const double height_bar =
        std::max(search.mode_height_fraction * tallest, search.mode_prominence * mean_height);

    std::vector<std::size_t> peaks;
    for (std::size_t k = 0; k < smoothed.size(); ++k) {
        if (smoothed[k] < height_bar) {
            continue;
        }
        const double left = k > 0 ? smoothed[k - 1] : 0.0;
        const double right = k + 1 < smoothed.size() ? smoothed[k + 1] : 0.0;
        if (smoothed[k] >= left && smoothed[k] > right) {
            peaks.push_back(k);
        }
    }

    // Merge every adjacent pair whose valley is not deep enough, dropping
    // the shorter of the two, and keep going until nothing merges. One
    // pass is not enough: dropping a peak makes its neighbours adjacent,
    // and the valley between THOSE is a different, usually shallower one.
    bool merged = true;
    while (merged && peaks.size() > 1) {
        merged = false;
        for (std::size_t index = 0; index + 1 < peaks.size(); ++index) {
            const std::size_t left = peaks[index];
            const std::size_t right = peaks[index + 1];
            double valley = smoothed[left];
            for (std::size_t k = left; k <= right; ++k) {
                valley = std::min(valley, smoothed[k]);
            }
            const double weaker = std::min(smoothed[left], smoothed[right]);
            if (weaker <= 0.0 || valley > search.valley_fraction * weaker) {
                peaks.erase(peaks.begin() +
                            static_cast<std::ptrdiff_t>(smoothed[left] < smoothed[right]
                                                            ? index
                                                            : index + 1));
                merged = true;
                break;
            }
        }
    }

    // The deepest valley that survived, which is the number the decision
    // rests on and the one a caller checks the call against.
    structure.valley_ratio = 0.0;
    for (std::size_t index = 0; index + 1 < peaks.size(); ++index) {
        const std::size_t left = peaks[index];
        const std::size_t right = peaks[index + 1];
        double valley = smoothed[left];
        for (std::size_t k = left; k <= right; ++k) {
            valley = std::min(valley, smoothed[k]);
        }
        const double weaker = std::min(smoothed[left], smoothed[right]);
        if (weaker > 0.0) {
            structure.valley_ratio = std::max(structure.valley_ratio, valley / weaker);
        }
    }

    // Each mode's basin runs to the midpoint between it and its
    // neighbours, and its position is the weighted centroid over that
    // basin. The centre of the tallest bin would quantise every tone to
    // bin_width, which at 256 bins over a 4.8 kHz shift is 37 Hz of
    // avoidable error on a number a catalogue match is checked against.
    const double kept = static_cast<double>(frequency.size());
    for (std::size_t index = 0; index < peaks.size(); ++index) {
        const std::size_t begin =
            index == 0 ? 0 : (peaks[index - 1] + peaks[index] + 1) / 2;
        const std::size_t end = index + 1 == peaks.size()
                                    ? histogram.size()
                                    : (peaks[index] + peaks[index + 1] + 1) / 2;

        double weight = 0.0;
        double first_moment = 0.0;
        double second_moment = 0.0;
        for (std::size_t k = begin; k < end; ++k) {
            const double centre =
                structure.histogram_low_hz + (static_cast<double>(k) + 0.5) * bin_width;
            weight += histogram[k];
            first_moment += histogram[k] * centre;
            second_moment += histogram[k] * centre * centre;
        }
        ToneMode mode;
        if (weight > 0.0) {
            mode.offset_hz = first_moment / weight;
            mode.width_hz = std::sqrt(
                std::max(second_moment / weight - mode.offset_hz * mode.offset_hz, 0.0));
            mode.weight = weight / kept;
        }
        structure.tones.push_back(mode);
    }
    structure.tone_count = structure.tones.size();

    if (structure.tone_count > search.max_tones) {
        structure.refusal = std::format(
            "the instantaneous-frequency histogram broke into {} modes, above the {} this will "
            "report. That is a histogram falling apart rather than a comb: check the extract is "
            "on one signal, and raise ToneSearch::smoothing_bins from {} if it genuinely is.",
            structure.tone_count, search.max_tones, search.smoothing_bins);
        return structure;
    }

    if (structure.tone_count == 0) {
        structure.refusal = std::format(
            "no maximum in the instantaneous-frequency histogram reached {:.1f} times its mean "
            "height, over a distribution {:.1f} Hz wide. The histogram is flat, which is what "
            "the instantaneous frequency of noise looks like: uniform across the whole band, "
            "with nothing standing out of it. Check the extract is on a signal.",
            search.mode_prominence, structure.frequency_spread_hz);
        return structure;
    }

    if (structure.tone_count < 2) {
        structure.refusal = std::format(
            "the instantaneous frequency has one mode, at {:.1f} Hz with a spread of {:.1f} Hz "
            "over a distribution {:.1f} Hz wide. Nothing is stepping between tones. A linear "
            "modulation, an analogue FM carrier and an unmodulated carrier all look like this, "
            "and telling them apart is the envelope's job rather than the histogram's: see "
            "EnvelopeStats and spectral_concentration.",
            structure.tones.front().offset_hz, structure.tones.front().width_hz,
            structure.frequency_spread_hz);
        return structure;
    }

    double spacing_sum = 0.0;
    double centre_sum = 0.0;
    for (std::size_t index = 0; index < structure.tones.size(); ++index) {
        centre_sum += structure.tones[index].offset_hz;
        if (index > 0) {
            spacing_sum += structure.tones[index].offset_hz - structure.tones[index - 1].offset_hz;
        }
    }
    structure.centre_offset_hz = centre_sum / static_cast<double>(structure.tones.size());
    structure.spacing_hz = spacing_sum / static_cast<double>(structure.tones.size() - 1);
    for (std::size_t index = 1; index < structure.tones.size(); ++index) {
        const double gap =
            structure.tones[index].offset_hz - structure.tones[index - 1].offset_hz;
        structure.spacing_spread_hz =
            std::max(structure.spacing_spread_hz, std::abs(gap - structure.spacing_hz));
    }

    if (structure.spacing_hz > 0.0) {
        for (const ToneMode& mode : structure.tones) {
            structure.widest_tone_fraction =
                std::max(structure.widest_tone_fraction, mode.width_hz / structure.spacing_hz);
        }
    }

    if (structure.widest_tone_fraction > search.tone_width_fraction) {
        structure.refusal = std::format(
            "the histogram has {} maxima {:.0f} Hz apart, but the widest of them has a standard "
            "deviation {:.3f} of that spacing, above the {:.2f} a tone set has to clear. A tone "
            "is a value the signal IS at, so its mass is a spike; a lump that wide means the "
            "signal is sweeping through the values around it. An analogue FM carrier reads "
            "exactly like this and its valleys can be deep enough to pass the valley rule, "
            "which is why the width is checked as well.",
            structure.tone_count, structure.spacing_hz, structure.widest_tone_fraction,
            search.tone_width_fraction);
        return structure;
    }

    if (structure.valley_ratio > search.valley_fraction) {
        structure.refusal = std::format(
            "the histogram has {} maxima but the deepest valley between them is {:.3f} of the "
            "weaker one, above the {:.2f} a tone set has to clear. A signal that is AT a tone "
            "leaves the space between tones nearly empty; one that sweeps through it does not. "
            "This is what an analogue FM carrier looks like, and calling it {}-FSK at {:.0f} Hz "
            "would be wrong rather than imprecise.",
            structure.tone_count, structure.valley_ratio, search.valley_fraction,
            structure.tone_count, structure.spacing_hz);
        return structure;
    }

    structure.found = true;
    // Half at the threshold, one at a perfectly empty valley, matching the
    // shape of margin_confidence so that two confidences in one
    // Characterisation mean the same kind of thing.
    structure.confidence =
        0.5 + 0.5 * std::clamp(1.0 - structure.valley_ratio / search.valley_fraction, 0.0, 1.0);
    return structure;
}

}  // namespace revenant::characterise
