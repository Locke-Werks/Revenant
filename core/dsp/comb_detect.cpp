// detect_comb from core/dsp/comb_cancel.h: a comb of evenly spaced narrow lines
// found in a time-averaged power spectrum with no prior.
//
// FOUR STEPS.
//
//   floor    The spectrum in dB, and a local floor from a running low
//            percentile. A percentile and not a median because a comb at four
//            bins' spacing with window leakage covers more than half the bins,
//            so a median sits on the lines. The 20th percentile still sees
//            floor when three bins in four are line, and is then lifted by
//            0.84 sigma so a height is measured from where the noise's median
//            would be. The window is 1/32 of the span, clamped to [65, 513]
//            bins: 513 bins at 16384 is 75 kHz, twice a broadcast station's
//            width, so the floor goes under a wideband signal instead of over
//            the top of it.
//
//   peaks    A local maximum standing kMinHeightDb or 4.5 sigma above the
//            floor, whichever is larger, and kMinProminenceDb above the lowest
//            bin two to six bins either side. The height test is against the
//            noise: an averaged second at 16384 bins has a dB standard
//            deviation near 0.4 dB, so 4 dB is ten sigma and noise alone
//            yields nothing, while an unaveraged frame's 5.6 dB spread lifts
//            the threshold with it. 4 dB is the bottom of what the lines seen
//            so far stand above the floor (4 to 15 dB). The prominence test is
//            what keeps the top of a wideband signal, which stands well above
//            the floor but has no valley beside each bump, from contributing
//            a peak per bump. Centres are a parabola through the dB values of
//            the peak and its two neighbours, good to about a tenth of a bin
//            for a Hann window.
//
//   spacing  The peak centres x_j scored by phase coherence,
//            S(f) = |sum_j exp(2 pi i f x_j)|, over f in cycles per bin from
//            7 / extent (eight lines cannot be spaced wider) to 1 / 4 (four
//            bins is the closest spacing at which the lines are separate
//            peaks). A comb of spacing d makes S(1/d) equal to its line
//            count, and makes S(n / d) the same for every n, while every
//            other grid sees phases that cancel. Each strong local maximum of
//            S is then fitted as a grid and judged by how many peaks it puts
//            on lines.
//
//   fit      For a candidate, each peak gets the nearest k, peaks within
//            tolerance of their line are kept, c and d come from a
//            least-squares line through (k, x), and that repeats with the
//            tolerance tightening. The ambiguity S cannot see: d / n puts
//            every line of d on its grid too, plus any strays between. So
//            the answer is the LARGEST d whose fit keeps at least 90% of the
//            best fit's lines, which rejects d / 2 (it keeps the same lines
//            and a few strays, not more than 11% more) and rejects 2 d (it
//            keeps half). A grid whose positions are mostly empty, under 40%
//            found, is unrelated carriers that happen to line up and is not
//            reported.
//
// k = 0 is the line nearest DC, bin bins / 2, so offset_hz is the comb line
// closest to the tuning and k is negative below it.
//
// PROVENANCE, per docs/clean-room.md: this project's own. Phase coherence of
// event positions is the textbook periodogram of a point process; no document
// was opened and no other program's source was read.

#include "core/dsp/comb_cancel.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <optional>
#include <vector>

namespace revenant::dsp {

namespace {

constexpr std::size_t kMinBins = 64;
constexpr double kMinSpacingBins = 4.0;

constexpr double kMinHeightDb = 4.0;
constexpr double kHeightSigmas = 4.5;
constexpr double kMinProminenceDb = 3.0;
constexpr std::size_t kValleyNear = 2;
constexpr std::size_t kValleyFar = 6;

constexpr double kFloorPercentile = 0.20;
constexpr double kFloorPercentileSigmas = 0.8416;  // standard normal at 0.20, negated

// More peaks than this and only the tallest are scored, because S costs bins
// times peaks. Sized for a comb four bins apart filling a span of 16384 bins,
// about 4000 lines: keeping only the tallest thousand of those leaves a grid
// three quarters empty, which the occupancy test then rightly refuses. At
// 65536 bins and the cap, S is 2.7e8 complex multiply-adds, a fraction of a
// second at acquisition.
constexpr std::size_t kMaxPeaks = 16 * kCombMaxLines;

constexpr std::size_t kMaxCandidates = 64;
constexpr double kCandidateFraction = 0.5;  // of the largest S
constexpr double kKeepFraction = 0.9;       // of the best fit's lines
constexpr double kMinOccupancy = 0.4;

struct Peak {
    double x = 0.0;       // centre, bins
    double height = 0.0;  // dB above floor
};

struct Fit {
    double c = 0.0;  // bins, line k = 0
    double d = 0.0;  // bins
    std::int64_t k_lo = 0;
    std::int64_t k_hi = -1;
    std::uint32_t found = 0;
    std::vector<double> heights;
};

[[nodiscard]] double percentile(std::vector<double>& values, double p)
{
    const auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                     values.end());
    return values[index];
}

// The running percentile, computed every `step` bins and joined by straight
// lines: the floor moves over hundreds of bins, so evaluating it at each bin
// buys nothing but time.
[[nodiscard]] std::vector<double> running_floor(const std::vector<double>& db)
{
    const std::size_t n = db.size();
    std::size_t window = std::clamp<std::size_t>(n / 32, 65, 513) | 1U;
    window = std::min(window, n | 1U);
    const std::size_t half = window / 2;
    const std::size_t step = std::max<std::size_t>(1, window / 16);

    std::vector<std::size_t> at;
    for (std::size_t i = 0; i < n; i += step) {
        at.push_back(i);
    }
    if (at.back() != n - 1) {
        at.push_back(n - 1);
    }

    std::vector<double> scratch;
    std::vector<double> value(at.size());
    for (std::size_t a = 0; a < at.size(); ++a) {
        const std::size_t lo = at[a] > half ? at[a] - half : 0;
        const std::size_t hi = std::min(n, at[a] + half + 1);
        scratch.assign(db.begin() + static_cast<std::ptrdiff_t>(lo),
                       db.begin() + static_cast<std::ptrdiff_t>(hi));
        value[a] = percentile(scratch, kFloorPercentile);
    }

    std::vector<double> floor(n);
    for (std::size_t a = 0; a + 1 < at.size(); ++a) {
        const double span = static_cast<double>(at[a + 1] - at[a]);
        for (std::size_t i = at[a]; i <= at[a + 1]; ++i) {
            const double t = static_cast<double>(i - at[a]) / span;
            floor[i] = value[a] + t * (value[a + 1] - value[a]);
        }
    }
    if (at.size() == 1) {
        floor.assign(n, value[0]);
    }
    return floor;
}

[[nodiscard]] std::vector<Peak> find_peaks(const std::vector<double>& db)
{
    const std::size_t n = db.size();
    std::vector<double> floor = running_floor(db);

    // The noise's spread from below the floor, where no line reaches: with
    // the floor at the 20th percentile, the 5th percentile of the residual
    // sits (1.645 - 0.842) sigma under it.
    std::vector<double> residual(n);
    for (std::size_t i = 0; i < n; ++i) {
        residual[i] = db[i] - floor[i];
    }
    const double p5 = percentile(residual, 0.05);
    const double sigma = std::max(-p5 / (1.6449 - kFloorPercentileSigmas), 0.0);
    for (double& f : floor) {
        f += kFloorPercentileSigmas * sigma;
    }
    const double threshold = std::max(kMinHeightDb, kHeightSigmas * sigma);

    std::vector<Peak> peaks;
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const double v = db[i];
        if (v - floor[i] < threshold || v < db[i - 1] || v <= db[i + 1]) {
            continue;
        }
        double valley = v;
        for (std::size_t m = kValleyNear; m <= kValleyFar; ++m) {
            if (i >= m) {
                valley = std::min(valley, db[i - m]);
            }
            if (i + m < n) {
                valley = std::min(valley, db[i + m]);
            }
        }
        if (v - valley < kMinProminenceDb) {
            continue;
        }
        const double a = db[i - 1];
        const double c = db[i + 1];
        const double curvature = a - 2.0 * v + c;
        const double shift = curvature < 0.0 ? std::clamp(0.5 * (a - c) / curvature, -0.5, 0.5)
                                             : 0.0;
        peaks.push_back({static_cast<double>(i) + shift, v - floor[i]});
    }

    if (peaks.size() > kMaxPeaks) {
        std::ranges::nth_element(peaks, peaks.begin() + kMaxPeaks,
                                 [](const Peak& l, const Peak& r) { return l.height > r.height; });
        peaks.resize(kMaxPeaks);
        std::ranges::sort(peaks, {}, &Peak::x);
    }
    return peaks;
}

// Peaks on the grid (c, d) within `tolerance` bins, one per k, the nearest.
void match(const std::vector<Peak>& peaks, double c, double d, double tolerance,
           std::vector<std::int64_t>& ks, std::vector<std::size_t>& which)
{
    ks.clear();
    which.clear();
    for (std::size_t j = 0; j < peaks.size(); ++j) {
        const double k = std::round((peaks[j].x - c) / d);
        const double r = std::abs(peaks[j].x - c - k * d);
        if (r > tolerance) {
            continue;
        }
        const auto ki = static_cast<std::int64_t>(k);
        if (!ks.empty() && ks.back() == ki) {
            const double prev = std::abs(peaks[which.back()].x - c - k * d);
            if (r < prev) {
                which.back() = j;
            }
            continue;
        }
        ks.push_back(ki);
        which.push_back(j);
    }
}

[[nodiscard]] std::optional<Fit> fit_grid(const std::vector<Peak>& peaks, double d0, double c0)
{
    double c = c0;
    double d = d0;
    std::vector<std::int64_t> ks;
    std::vector<std::size_t> which;

    // Loose first, because the candidate's d is only as good as S's sampling
    // and a small error in d walks the far lines off the grid; each least-
    // squares pass shrinks that error and the tolerance follows it down.
    const double final_tolerance = std::min(0.6, 0.2 * d0);
    for (const double tolerance : {0.3 * d0, 0.2 * d0, final_tolerance, final_tolerance}) {
        match(peaks, c, d, std::max(tolerance, final_tolerance), ks, which);
        if (ks.size() < 2) {
            return std::nullopt;
        }
        double sk = 0.0;
        double sx = 0.0;
        double skk = 0.0;
        double skx = 0.0;
        const auto m = static_cast<double>(ks.size());
        for (std::size_t i = 0; i < ks.size(); ++i) {
            const auto k = static_cast<double>(ks[i]);
            const double x = peaks[which[i]].x;
            sk += k;
            sx += x;
            skk += k * k;
            skx += k * x;
        }
        const double det = m * skk - sk * sk;
        if (det <= 0.0) {
            return std::nullopt;
        }
        d = (m * skx - sk * sx) / det;
        c = (sx - d * sk) / m;
        if (!(d >= kMinSpacingBins * 0.9)) {
            return std::nullopt;
        }
    }

    match(peaks, c, d, final_tolerance, ks, which);
    if (ks.empty()) {
        return std::nullopt;
    }
    Fit fit;
    fit.c = c;
    fit.d = d;
    fit.k_lo = ks.front();
    fit.k_hi = ks.back();
    fit.found = static_cast<std::uint32_t>(ks.size());
    for (const std::size_t j : which) {
        fit.heights.push_back(peaks[j].height);
    }
    return fit;
}

}  // namespace

Expected<std::optional<CombEstimate>> detect_comb(const CombSpectrum& spectrum)
{
    const std::size_t n = spectrum.power.size();
    if (n == 0) {
        return fail("comb detection: the spectrum is empty");
    }
    if (spectrum.rate <= 0) {
        return fail(std::format("comb detection: a rate of {} is not positive", spectrum.rate));
    }
    if (n < kMinBins) {
        return fail(std::format("comb detection: {} bins is fewer than the {} it needs", n,
                                kMinBins));
    }

    // A zero bin is legitimate (a filter's stopband, a quiet simulation) and
    // becomes a very low dB value rather than minus infinity.
    std::vector<double> db(n);
    float largest = 0.0F;
    for (std::size_t i = 0; i < n; ++i) {
        const float p = spectrum.power[i];
        if (!std::isfinite(p) || p < 0.0F) {
            return fail(std::format("comb detection: bin {} holds {}, not a power", i, p));
        }
        largest = std::max(largest, p);
    }
    if (largest <= 0.0F) {
        return std::optional<CombEstimate>{};
    }
    const double tiny = static_cast<double>(largest) * 1e-30;
    for (std::size_t i = 0; i < n; ++i) {
        db[i] = 10.0 * std::log10(std::max(static_cast<double>(spectrum.power[i]), tiny));
    }

    const std::vector<Peak> peaks = find_peaks(db);
    if (peaks.size() < kCombMinLines) {
        return std::optional<CombEstimate>{};
    }

    // S(f) on a grid a quarter of its main lobe wide, each peak's phasor
    // advanced by multiplication rather than recomputed: in double the drift
    // over a span's worth of steps is below 1e-10 of a cycle.
    const double extent = peaks.back().x - peaks.front().x;
    const double f_lo = static_cast<double>(kCombMinLines - 1) / extent;
    const double f_hi = 1.0 / kMinSpacingBins;
    if (!(f_lo < f_hi)) {
        return std::optional<CombEstimate>{};
    }
    const double df = 0.25 / extent;
    const auto steps = static_cast<std::size_t>((f_hi - f_lo) / df) + 1;
    std::vector<std::complex<double>> sum(steps);
    for (const Peak& peak : peaks) {
        const double x = peak.x - peaks.front().x;
        std::complex<double> z = std::polar(1.0, 2.0 * std::numbers::pi * f_lo * x);
        const std::complex<double> r = std::polar(1.0, 2.0 * std::numbers::pi * df * x);
        for (std::size_t s = 0; s < steps; ++s) {
            sum[s] += z;
            z *= r;
        }
    }
    std::vector<double> mag(steps);
    double largest_s = 0.0;
    for (std::size_t s = 0; s < steps; ++s) {
        mag[s] = std::abs(sum[s]);
        largest_s = std::max(largest_s, mag[s]);
    }

    struct Candidate {
        double f;
        double s;
        std::size_t at;
    };
    std::vector<Candidate> candidates;
    for (std::size_t s = 0; s < steps; ++s) {
        const double left = s > 0 ? mag[s - 1] : 0.0;
        const double right = s + 1 < steps ? mag[s + 1] : 0.0;
        if (mag[s] < kCandidateFraction * largest_s || mag[s] < left || mag[s] <= right) {
            continue;
        }
        // Parabolic refinement of the lobe's top, worth a factor of several in
        // d before the fit starts.
        double shift = 0.0;
        const double curvature = left - 2.0 * mag[s] + right;
        if (s > 0 && s + 1 < steps && curvature < 0.0) {
            shift = std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5);
        }
        candidates.push_back({f_lo + (static_cast<double>(s) + shift) * df, mag[s], s});
    }
    std::ranges::sort(candidates, [](const Candidate& l, const Candidate& r) { return l.s > r.s; });
    if (candidates.size() > kMaxCandidates) {
        candidates.resize(kMaxCandidates);
    }

    std::vector<Fit> fits;
    for (const Candidate& candidate : candidates) {
        const double d = 1.0 / candidate.f;
        // The grid's phase from the refined f: S's argument at the refined
        // frequency, computed directly rather than read off the sampled sum.
        std::complex<double> z{};
        for (const Peak& peak : peaks) {
            z += std::polar(1.0, 2.0 * std::numbers::pi * candidate.f * (peak.x - peaks.front().x));
        }
        const double c = peaks.front().x + std::arg(z) / (2.0 * std::numbers::pi) * d;
        std::optional<Fit> fit = fit_grid(peaks, d, c);
        if (!fit || fit->found < kCombMinLines) {
            continue;
        }
        const double occupancy =
            static_cast<double>(fit->found) / static_cast<double>(fit->k_hi - fit->k_lo + 1);
        if (occupancy < kMinOccupancy) {
            continue;
        }
        fits.push_back(std::move(*fit));
    }
    if (fits.empty()) {
        return std::optional<CombEstimate>{};
    }

    std::uint32_t best_found = 0;
    for (const Fit& fit : fits) {
        best_found = std::max(best_found, fit.found);
    }
    const Fit* chosen = nullptr;
    for (const Fit& fit : fits) {
        if (static_cast<double>(fit.found) < kKeepFraction * static_cast<double>(best_found)) {
            continue;
        }
        if (chosen == nullptr || fit.d > chosen->d * 1.01 ||
            (fit.d > chosen->d / 1.01 && fit.found > chosen->found)) {
            chosen = &fit;
        }
    }

    // Renumber so k = 0 is the line nearest DC.
    const double dc = static_cast<double>(n / 2);
    const double k0 = std::round((dc - chosen->c) / chosen->d);
    const double c = chosen->c + k0 * chosen->d;
    const auto shift = static_cast<std::int64_t>(k0);

    const double hz_per_bin = static_cast<double>(spectrum.rate) / static_cast<double>(n);
    CombEstimate estimate;
    estimate.offset_hz = (c - dc) * hz_per_bin;
    estimate.spacing_hz = chosen->d * hz_per_bin;
    estimate.k_lo = static_cast<std::int32_t>(chosen->k_lo - shift);
    estimate.k_hi = static_cast<std::int32_t>(chosen->k_hi - shift);
    estimate.lines_found = chosen->found;
    std::vector<double> heights = chosen->heights;
    std::ranges::sort(heights);
    const std::size_t h = heights.size();
    estimate.median_height_db = static_cast<float>(
        h % 2 == 1 ? heights[h / 2] : 0.5 * (heights[h / 2 - 1] + heights[h / 2]));
    return std::optional<CombEstimate>{estimate};
}

}  // namespace revenant::dsp
