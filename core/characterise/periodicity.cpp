#include "core/characterise/periodicity.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace revenant::characterise {
namespace {

constexpr std::size_t kMinSamplesForProfile = 1024;

constexpr std::size_t kDefaultMinSymbolSamples = 32;
constexpr std::size_t kDefaultMaxSymbolSamples = 8192;
constexpr std::size_t kDefaultMinPeriodSamples = 64;

// Lags either side of a candidate multiple that the period walk accepts as
// that multiple. Wider than the spectral comb's two bins because a frame
// period that is not a whole number of samples drifts by up to one sample
// per repeat, so the eighth repeat sits up to eight samples off.
constexpr std::size_t kPeriodToleranceLags = 8;

[[nodiscard]] double median_of(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle),
                     values.end());
    return values[middle];
}

// What one reading of the profile found over a band of lags.
struct ProfileScan {
    bool valid = false;

    // The tallest lag in the band, whatever its shape. Carried for the
    // refusal text: a caller told only that nothing was found cannot tell
    // "the channel is empty" from "something stood up and was rejected".
    std::size_t strongest_lag = 0;
    double strongest_ratio = 0.0;

    // The most PROMINENT local maximum, which is the one a reading acts
    // on. See the comment on isolation below for why that is not the same
    // as the tallest.
    bool has_peak = false;
    std::size_t lag = 0;
    double ratio = 0.0;

    // How high the lags in the proportional window beside the peak reach,
    // half a period to one and a half periods away, at the ninetieth
    // percentile.
    //
    // This is the number that separates a repeat from a pulse shape, and
    // it was added after a measurement rather than designed in. A root
    // raised cosine spanning eight symbols correlates with itself out to
    // eight symbol periods, so a 2400 baud signal at 48 kS/s has a decaying
    // ripple reaching 160 samples, and measured 2026-09-21 that ripple read
    // 0.129 at lag 32 on a signal with no framing in it at all: sixteen
    // times the profile's median, so every floor test passed and the
    // characteriser reported a 32-sample OFDM symbol.
    //
    // A cyclic prefix and a frame are both ISOLATED: the waveform matches
    // itself at one lag and not at the lags around it. A pulse shape's
    // shoulder is the opposite, a slope reaching back to zero lag where the
    // correlation is one. Comparing a candidate against its own
    // neighbourhood tells them apart and comparing it against the band's
    // median does not.
    //
    // The window is proportional rather than fixed because everything about
    // a correlation structure scales with its own lag, and a fixed window
    // either reaches back to lag 1 for a short symbol, where the ratio is
    // near one for any signal, or fails to clear the shoulder for a long
    // one.
    double isolation = 0.0;

    double floor_ratio = 0.0;
};

[[nodiscard]] double median_over(const Autocorrelation& profile, std::size_t low,
                                 std::size_t high) {
    std::vector<double> window;
    window.reserve(high - low + 1);
    for (std::size_t lag = low; lag <= high; ++lag) {
        window.push_back(profile.ratio[lag]);
    }
    return median_of(std::move(window));
}

// How high the lags beside a candidate reach: the 90th percentile over
// [lag/2, lag - guard] and [lag + guard, 3*lag/2].
//
// A percentile rather than the maximum, and that is the whole difference
// between this working and not. The maximum of a few hundred noisy lags is
// an extreme value: measured 2026-09-21 on a one-eighth-guard OFDM burst at
// 10 dB in 2500 Hz, the neighbourhood's median was 0.0022 and its maximum
// 0.010, so a peak at 0.0376 stood 3.8 times over the maximum and 17 over
// the median, and comparing against the maximum refused a signal the
// detector had located correctly.
//
// The ninetieth rather than the median, because the case this exists to
// reject is a pulse shape's shoulder, which is a broad elevated region
// rather than a spike: most of its window IS raised, so a high percentile
// still reads it while staying close to the floor over a window of noise.
[[nodiscard]] double isolation_of(const Autocorrelation& profile, std::size_t lag) {
    const std::size_t guard = std::max<std::size_t>(2, lag / 32);
    const std::size_t last = profile.ratio.size() - 1;
    std::vector<double> window;
    const std::size_t inner_low = std::max<std::size_t>(lag / 2, 1);
    if (lag > guard) {
        for (std::size_t k = inner_low; k + guard <= lag; ++k) {
            window.push_back(profile.ratio[k]);
        }
    }
    const std::size_t outer_high = std::min(lag + lag / 2, last);
    for (std::size_t k = lag + guard; k <= outer_high; ++k) {
        window.push_back(profile.ratio[k]);
    }
    if (window.empty()) {
        return 0.0;
    }
    const auto index = static_cast<std::size_t>(0.9 * static_cast<double>(window.size() - 1));
    std::nth_element(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(index),
                     window.end());
    return window[index];
}

[[nodiscard]] ProfileScan scan(const Autocorrelation& profile, std::size_t low, std::size_t high,
                               double min_ratio) {
    ProfileScan result;
    if (profile.ratio.size() < 4) {
        return result;
    }
    high = std::min(high, profile.ratio.size() - 1);
    if (low < 1 || low > high) {
        return result;
    }
    result.valid = true;
    result.floor_ratio = median_over(profile, low, high);

    double best_prominence = 0.0;
    for (std::size_t lag = low; lag <= high; ++lag) {
        const double ratio = profile.ratio[lag];
        if (ratio > result.strongest_ratio) {
            result.strongest_ratio = ratio;
            result.strongest_lag = lag;
        }
        if (ratio < min_ratio) {
            continue;
        }
        if (lag > 0 && profile.ratio[lag - 1] > ratio) {
            continue;
        }
        if (lag + 1 < profile.ratio.size() && profile.ratio[lag + 1] >= ratio) {
            continue;
        }
        const double isolation = isolation_of(profile, lag);
        const double prominence = ratio / std::max({isolation, result.floor_ratio, 1.0e-12});
        if (!result.has_peak || prominence > best_prominence) {
            result.has_peak = true;
            best_prominence = prominence;
            result.lag = lag;
            result.ratio = ratio;
            result.isolation = isolation;
        }
    }
    return result;
}

// Best ratio within kPeriodToleranceLags of a target lag.
[[nodiscard]] double ratio_near(const Autocorrelation& profile, std::size_t target) {
    if (target == 0 || target >= profile.ratio.size()) {
        return 0.0;
    }
    const std::size_t low = target > kPeriodToleranceLags ? target - kPeriodToleranceLags : 1;
    const std::size_t high = std::min(target + kPeriodToleranceLags, profile.ratio.size() - 1);
    double best = 0.0;
    for (std::size_t lag = low; lag <= high; ++lag) {
        best = std::max(best, profile.ratio[lag]);
    }
    return best;
}

// Same shape as margin_confidence, expressed on a ratio over a floor rather
// than on a margin in decibels, so that a Characterisation carrying both
// means the same kind of thing by each.
[[nodiscard]] double ratio_confidence(double ratio, double bar) {
    if (!(bar > 0.0) || ratio < bar) {
        return 0.0;
    }
    const double margin_db = 10.0 * std::log10(ratio / bar);
    return margin_confidence(margin_db, 0.0);
}

}  // namespace

Expected<Autocorrelation> autocorrelation(std::span<const Complex64> samples,
                                          SampleRate rate,
                                          std::size_t max_lag)
{
    if (rate <= 0) {
        return fail(std::format("autocorrelation: sample rate must be positive, was {}", rate));
    }
    if (samples.size() < kMinSamplesForProfile) {
        return fail(std::format(
            "autocorrelation: {} samples is under the {} this needs. A profile over a buffer "
            "that short has a floor around one over the square root of the overlap, which is "
            "high enough that a frame period would be found in noise.",
            samples.size(), kMinSamplesForProfile));
    }

    const std::size_t count = samples.size();
    if (max_lag == 0) {
        max_lag = std::min(count / 8, kMaxProfileLag);
    }
    max_lag = std::min({max_lag, count / 2, kMaxProfileLag});
    if (max_lag < 2) {
        return fail(std::format(
            "autocorrelation: the lag range resolved to {}, which is nothing to search. Supply "
            "a longer buffer than {} samples.",
            max_lag, count));
    }

    // Zero-padded to at least twice the buffer, so the circular
    // correlation the transform computes is the linear one: without the
    // padding, energy at lag tau wraps round and adds to lag count - tau,
    // which puts a mirror image of every real peak in the profile.
    std::size_t size = 1;
    while (size < 2 * count) {
        size *= 2;
    }
    if (size > kMaxTransform) {
        return fail(std::format(
            "autocorrelation: {} samples needs a {}-point transform, above the {} this analysis "
            "will run. Characterise a shorter extract, or a decimated one.",
            count, size, kMaxTransform));
    }

    std::vector<Complex64> work(size, Complex64(0.0, 0.0));
    std::copy(samples.begin(), samples.end(), work.begin());
    fft_in_place(work);
    for (Complex64& bin : work) {
        bin = Complex64(std::norm(bin), 0.0);
    }
    // Inverse transform out of the forward one, conj(fft(conj(X)))/N. The
    // spectrum here is real and non-negative, so the conjugation on the way
    // in is a no-op and only the one on the way out is written.
    fft_in_place(work);
    for (Complex64& bin : work) {
        bin = std::conj(bin);
    }

    Autocorrelation profile;
    profile.rate = rate;
    profile.sample_count = count;
    profile.max_lag = max_lag;
    profile.ratio.assign(max_lag + 1, 0.0);

    const double zero_lag = std::abs(work[0]);
    if (!(zero_lag > 0.0)) {
        return fail("autocorrelation: the buffer carries no power at all. Check the extract "
                    "reached this call.");
    }
    for (std::size_t lag = 0; lag <= max_lag; ++lag) {
        // Divided by the share of the buffer that overlaps at this lag, so
        // a peak at lag 8000 is comparable with one at lag 80. Without it
        // the profile slopes down across its own range and the median that
        // stands in for the floor is measuring that slope.
        const double overlap =
            static_cast<double>(count - lag) / static_cast<double>(count);
        profile.ratio[lag] = std::abs(work[lag]) / (zero_lag * overlap);
    }
    return profile;
}

OfdmStructure find_cyclic_prefix(const Autocorrelation& profile, const OfdmSearch& search)
{
    OfdmStructure structure;

    const std::size_t low = search.min_symbol_samples > 0 ? search.min_symbol_samples
                                                          : kDefaultMinSymbolSamples;
    const std::size_t high = search.max_symbol_samples > 0 ? search.max_symbol_samples
                                                           : kDefaultMaxSymbolSamples;

    const ProfileScan found = scan(profile, low, high, search.min_prefix_ratio);
    if (!found.valid) {
        structure.refusal = std::format(
            "the useful-symbol search band {} to {} samples does not fit inside the profile's "
            "{} lags. Lengthen the extract, or narrow OfdmSearch.",
            low, high, profile.max_lag);
        return structure;
    }

    structure.floor_ratio = found.floor_ratio;
    const double bar = std::max(search.min_prefix_ratio, search.floor_multiple * found.floor_ratio);

    if (!found.has_peak || found.ratio < bar) {
        structure.correlation = found.has_peak ? found.ratio : found.strongest_ratio;
        structure.refusal = std::format(
            "the most prominent isolated repeat between lags {} and {} is {:.4f} at lag {}, "
            "under the {:.4f} a guard interval has to reach, which is the larger of the "
            "{:.3f} floor and {:.0f} times the profile's own median of {:.4f}. The tallest lag "
            "in the band is {:.4f} at lag {}, and it was not taken because a candidate has to "
            "be a local maximum: a correlation that only rises toward the edge of the band is "
            "the slope of something wider, not a repeat. It is not OFDM, or the guard is small "
            "enough, or the extract short enough, that the correlation sits under the floor.",
            low, high, found.has_peak ? found.ratio : 0.0, found.has_peak ? found.lag : 0,
            bar, search.min_prefix_ratio, search.floor_multiple, found.floor_ratio,
            found.strongest_ratio, found.strongest_lag);
        return structure;
    }

    structure.correlation = found.ratio;

    if (found.ratio >= search.max_prefix_ratio) {
        structure.refusal = std::format(
            "the correlation at lag {} is {:.4f}, at or above the {:.2f} a cyclic prefix can "
            "reach. A guard interval is a FRACTION of its own symbol, so a repeat this strong "
            "means the whole waveform comes round again. That is framing rather than a cyclic "
            "prefix, and find_frame_period is what reads it.",
            found.lag, found.ratio, search.max_prefix_ratio);
        return structure;
    }

    if (found.ratio < search.isolation_multiple * found.isolation) {
        structure.refusal = std::format(
            "the correlation at lag {} is {:.4f} but the lags around it reach {:.4f}, so it "
            "stands only {:.1f} times over its own neighbourhood against the {:.0f} an "
            "isolated repeat has to clear. That is the shoulder of a pulse shape rather than a "
            "guard interval: a root raised cosine correlates with itself out to its whole span, "
            "which for a shaped signal is several symbol periods of decaying ripple. A cyclic "
            "prefix matches at ONE lag and at none of the lags beside it.",
            found.lag, found.ratio, found.isolation,
            found.ratio / std::max(found.isolation, 1.0e-12), search.isolation_multiple);
        return structure;
    }

    structure.found = true;
    structure.symbol_samples = found.lag;
    structure.symbol_seconds =
        static_cast<double>(found.lag) / static_cast<double>(profile.rate);
    structure.subcarrier_spacing_hz =
        static_cast<double>(profile.rate) / static_cast<double>(found.lag);
    structure.prefix_fraction = found.ratio;
    const double prefix =
        found.ratio * static_cast<double>(found.lag) / (1.0 - found.ratio);
    structure.prefix_samples = static_cast<std::size_t>(std::llround(prefix));
    structure.total_symbol_seconds =
        static_cast<double>(found.lag + structure.prefix_samples) /
        static_cast<double>(profile.rate);
    structure.confidence = ratio_confidence(found.ratio, bar);
    return structure;
}

FramePeriod find_frame_period(const Autocorrelation& profile, const FrameSearch& search)
{
    FramePeriod frame;

    const std::size_t low = search.min_period_samples > 0 ? search.min_period_samples
                                                          : kDefaultMinPeriodSamples;
    const std::size_t high =
        search.max_period_samples > 0 ? search.max_period_samples : profile.max_lag;

    const ProfileScan found = scan(profile, low, high, search.min_repeat_ratio);
    if (!found.valid) {
        frame.refusal = std::format(
            "the frame-period search band {} to {} samples does not fit inside the profile's {} "
            "lags. Lengthen the extract, or narrow FrameSearch.",
            low, high, profile.max_lag);
        return frame;
    }

    frame.floor_ratio = found.floor_ratio;
    const double bar = std::max(search.min_repeat_ratio, search.floor_multiple * found.floor_ratio);

    if (!found.has_peak || found.ratio < bar) {
        frame.refusal = std::format(
            "the strongest repeat between lags {} and {} is {:.4f} at lag {}, under the {:.4f} "
            "a frame has to reach, which is the larger of the {:.2f} floor and {:.0f} times the "
            "profile's own median of {:.4f}. Nothing in this extract comes round again. A "
            "waveform with no framing, a frame longer than the {} lags searched, or an extract "
            "covering less than two frames all read this way; the second and third are fixed by "
            "a longer extract.",
            low, high, found.strongest_ratio, found.strongest_lag, bar, search.min_repeat_ratio,
            search.floor_multiple, found.floor_ratio, high);
        return frame;
    }

    if (found.ratio < search.isolation_multiple * found.isolation) {
        frame.strongest_lag = found.lag;
        frame.repeat_fraction = found.ratio;
        frame.refusal = std::format(
            "the repeat at lag {} is {:.4f} but the lags around it reach {:.4f}, so it stands "
            "only {:.1f} times over its own neighbourhood against the {:.0f} an isolated repeat "
            "has to clear. That is a pulse shape's own correlation rather than a frame coming "
            "round again.",
            found.lag, found.ratio, found.isolation,
            found.ratio / std::max(found.isolation, 1.0e-12), search.isolation_multiple);
        return frame;
    }

    frame.strongest_lag = found.lag;
    frame.repeat_fraction = found.ratio;

    // Walk down to the first repeat. The strongest peak of a waveform that
    // repeats many times over the buffer can land on the second or third
    // repeat, and reporting that is reporting a multiple of the frame
    // period as the frame period. Largest divisor first, and a divisor is
    // taken only when every multiple under the strongest peak is there.
    frame.period_samples = found.lag;
    frame.harmonic_divisor = 1;
    for (std::size_t divisor = std::max<std::size_t>(search.max_period_divisor, 1); divisor >= 1;
         --divisor) {
        const std::size_t candidate = found.lag / divisor;
        if (candidate < low || candidate == 0) {
            continue;
        }
        bool present = true;
        for (std::size_t multiple = 1; multiple <= divisor; ++multiple) {
            if (ratio_near(profile, candidate * multiple) < bar) {
                present = false;
                break;
            }
        }
        if (!present) {
            continue;
        }
        frame.period_samples = candidate;
        frame.repeat_fraction = ratio_near(profile, candidate);
        frame.harmonic_divisor = divisor;
        break;
    }

    frame.found = true;
    frame.period_seconds =
        static_cast<double>(frame.period_samples) / static_cast<double>(profile.rate);
    frame.confidence = ratio_confidence(frame.repeat_fraction, bar);
    return frame;
}

}  // namespace revenant::characterise
