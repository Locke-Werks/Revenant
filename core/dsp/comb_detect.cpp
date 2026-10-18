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

// A half is split off when its own grid puts its end lines this far from the
// joint grid: a quarter of a bin, which is well over the centres' error
// averaged along a half's least-squares line, and at the finer resolutions no
// less than kSplitHz. On the owner's HF dumps at 36.6 Hz a bin, the 100.7 Hz
// ripple sidebands beside every line pull peak centres about, and one
// switcher's halves came out 11 Hz apart at the ends of the span; two sources
// a hundred hertz off half a spacing, or a few hertz apart in spacing across
// thirty lines, are far beyond it.
constexpr double kSplitBins = 0.25;
constexpr double kSplitHz = 25.0;

// A parity holding under this share of its slots is empty: the grid is the
// other parity's at twice the spacing. Well under kMinOccupancy, so a comb's
// weak edges do not read as a missing half.
constexpr double kEmptyParity = 0.15;

// A peak this close to a taken grid's line is that line: the grid fit's own
// final tolerance.
constexpr double kTakenBins = 0.6;

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
    std::vector<std::int64_t> ks;     // per found line
    std::vector<std::size_t> which;   // the peak each found line is
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
    fit.ks = ks;
    fit.which = which;
    return fit;
}

// The best grid among the peaks, by the phase-coherence search and the
// largest-d rule in the header comment, or nothing.
[[nodiscard]] std::optional<Fit> best_grid(const std::vector<Peak>& peaks)
{
    if (peaks.size() < kCombMinLines) {
        return std::nullopt;
    }

    // S(f) on a grid a quarter of its main lobe wide, each peak's phasor
    // advanced by multiplication rather than recomputed: in double the drift
    // over a span's worth of steps is below 1e-10 of a cycle.
    const double extent = peaks.back().x - peaks.front().x;
    const double f_lo = static_cast<double>(kCombMinLines - 1) / extent;
    const double f_hi = 1.0 / kMinSpacingBins;
    if (!(f_lo < f_hi)) {
        return std::nullopt;
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
        return std::nullopt;
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
    return *chosen;
}

// The lines of `fit` whose k has parity `odd`, refitted by least squares as a
// grid of their own at about twice the spacing. Each sub-comb gets its own c
// and d because two interleaved sources do not share a spacing exactly.
[[nodiscard]] std::optional<Fit> parity_fit(const Fit& fit, const std::vector<Peak>& peaks,
                                            bool odd)
{
    Fit out;
    double sj = 0.0;
    double sx = 0.0;
    double sjj = 0.0;
    double sjx = 0.0;
    for (std::size_t i = 0; i < fit.ks.size(); ++i) {
        const std::int64_t k = fit.ks[i];
        if (((k % 2) != 0) != odd) {
            continue;
        }
        // Floor division, so negative k keep their place on the half grid.
        const std::int64_t j = (k - (odd ? 1 : 0) - (k < 0 ? 1 : 0)) / 2;
        const double x = peaks[fit.which[i]].x;
        out.ks.push_back(j);
        out.which.push_back(fit.which[i]);
        out.heights.push_back(fit.heights[i]);
        const auto jd = static_cast<double>(j);
        sj += jd;
        sx += x;
        sjj += jd * jd;
        sjx += jd * x;
    }
    out.found = static_cast<std::uint32_t>(out.ks.size());
    if (out.found < kCombMinLines) {
        return std::nullopt;
    }
    const auto m = static_cast<double>(out.found);
    const double det = m * sjj - sj * sj;
    if (det <= 0.0) {
        return std::nullopt;
    }
    out.d = (m * sjx - sj * sx) / det;
    out.c = (sx - out.d * sj) / m;
    if (!(out.d > 0.0)) {
        return std::nullopt;
    }
    out.k_lo = std::ranges::min(out.ks);
    out.k_hi = std::ranges::max(out.ks);
    return out;
}

// How far, in bins, a half's own grid puts its end lines from where the joint
// grid puts them. Half line j is joint line 2 j + parity; both grids are
// straight lines in k, so the ends are the worst of it.
[[nodiscard]] double misplaced(const Fit& joint, const Fit& half, std::int64_t parity)
{
    double worst = 0.0;
    for (const std::int64_t j : {half.k_lo, half.k_hi}) {
        const auto jd = static_cast<double>(j);
        const auto k = static_cast<double>(2 * j + parity);
        worst = std::max(worst, std::abs(half.c + jd * half.d - (joint.c + k * joint.d)));
    }
    return worst;
}

// A fit as an estimate, renumbered so k = 0 is the line nearest DC.
[[nodiscard]] CombEstimate to_estimate(const Fit& fit, std::size_t n, SampleRate rate)
{
    const double dc = static_cast<double>(n / 2);
    const double k0 = std::round((dc - fit.c) / fit.d);
    const double c = fit.c + k0 * fit.d;
    const auto shift = static_cast<std::int64_t>(k0);

    const double hz_per_bin = static_cast<double>(rate) / static_cast<double>(n);
    CombEstimate estimate;
    estimate.offset_hz = (c - dc) * hz_per_bin;
    estimate.spacing_hz = fit.d * hz_per_bin;
    estimate.k_lo = static_cast<std::int32_t>(fit.k_lo - shift);
    estimate.k_hi = static_cast<std::int32_t>(fit.k_hi - shift);
    estimate.lines_found = fit.found;
    std::vector<double> heights = fit.heights;
    std::ranges::sort(heights);
    const std::size_t h = heights.size();
    estimate.median_height_db = static_cast<float>(
        h % 2 == 1 ? heights[h / 2] : 0.5 * (heights[h / 2 - 1] + heights[h / 2]));
    return estimate;
}

// A comb's modulation sidebands. A switcher whose supply ripples puts a ladder
// of sidebands at multiples of the ripple frequency either side of every line
// (on the owner's HF comb, 2026-10-16: about 118 Hz apart, the first pair 11.5
// dB under the carrier, the second 17, then a phase-noise pedestal near 25).
// Each order is a comb at the same spacing, offset by a fixed amount, which
// the tracker cancels as it does any comb; the carrier alone is three
// quarters of a line's power, and the waterfall's bins hold the first pair
// too, so cancelling only the carrier barely changes what is seen.
//
// Measured from the comb's own lines stacked on their peaks and averaged, so
// a sideband a single line could not show above the floor stands out of the
// average. Up to kSidebandOrders each side, nearest first, each standing
// kSidebandOverPedestalDb over the stack's median.
constexpr int kSidebandOrders = 6;
constexpr double kSidebandOverPedestalDb = 6.0;

// Orders the tracker cancels, of the kSidebandOrders taken out of the search.
// On the owner's 80 m comb the rungs at 59.5 Hz steps stand 19, 12, 23 and 17
// dB under the carrier, then 25 and below into a pedestal near 30: four
// orders take the ladder down to the pedestal. Each order costs two slots per
// line.
constexpr std::int32_t kTrackedOrders = 4;
constexpr double kSidebandReachHz = 1'500.0;

[[nodiscard]] std::vector<Fit> sideband_grids(const Fit& grid, const std::vector<Peak>& peaks,
                                              const std::vector<double>& db, SampleRate rate,
                                              double& ripple_bins)
{
    std::vector<Fit> out;
    ripple_bins = 0.0;
    const auto n = static_cast<std::int64_t>(db.size());
    const double hz_per_bin = static_cast<double>(rate) / static_cast<double>(n);
    const auto reach = static_cast<std::int64_t>(
        std::min(kSidebandReachHz / hz_per_bin, 0.45 * grid.d));
    if (reach < 4 || grid.which.size() < kCombMinLines) {
        return out;
    }
    std::vector<double> stack(static_cast<std::size_t>(2 * reach + 1), 0.0);
    std::size_t lines = 0;
    for (const std::size_t j : grid.which) {
        const auto centre = static_cast<std::int64_t>(std::lround(peaks[j].x));
        if (centre - reach < 0 || centre + reach >= n) {
            continue;
        }
        const double top = db[static_cast<std::size_t>(centre)];
        for (std::int64_t o = -reach; o <= reach; ++o) {
            stack[static_cast<std::size_t>(o + reach)] +=
                std::pow(10.0, (db[static_cast<std::size_t>(centre + o)] - top) / 10.0);
        }
        ++lines;
    }
    if (lines < kCombMinLines) {
        return out;
    }
    std::vector<double> profile(stack.size());
    for (std::size_t i = 0; i < stack.size(); ++i) {
        profile[i] = 10.0 * std::log10(std::max(stack[i] / static_cast<double>(lines), 1e-30));
    }
    // The lower quartile, not the median: with a short reach at a coarse
    // resolution the ladder itself can fill half the window.
    std::vector<double> sorted = profile;
    const double pedestal = percentile(sorted, 0.25);

    // The first sideband each side, nearest the carrier and standing clear
    // of the pedestal; their mean distance is the ripple frequency, and the
    // higher orders, which sink into the pedestal in the average while still
    // standing out beside the strongest lines, are taken at its multiples.
    double first[2] = {0.0, 0.0};
    double level[2] = {0.0, 0.0};
    for (const int side : {1, -1}) {
        // From two bins out, past the carrier's own window response.
        for (std::int64_t o = 2; o < reach; ++o) {
            const auto i = static_cast<std::size_t>(reach + side * o);
            const double v = profile[i];
            if (v < profile[i - 1] || v < profile[i + 1] ||
                v - pedestal < kSidebandOverPedestalDb) {
                continue;
            }
            const double a = profile[i - 1];
            const double c = profile[i + 1];
            const double curvature = a - 2.0 * v + c;
            const double shift =
                curvature < 0.0 ? std::clamp(0.5 * (a - c) / curvature, -0.5, 0.5) : 0.0;
            first[side > 0 ? 0 : 1] = static_cast<double>(o) + side * shift;
            level[side > 0 ? 0 : 1] = v;
            break;
        }
    }
    // Ripple modulation puts its sidebands in pairs, the same distance either
    // side. One side alone, or a lopsided pair, is a neighbouring line or
    // another family's sideband, not this comb's.
    if (!(first[0] > 0.0 && first[1] > 0.0) || std::abs(first[0] - first[1]) > 1.0) {
        return out;
    }
    const double ripple = 0.5 * (first[0] + first[1]);
    ripple_bins = ripple;
    for (int m = 1; m <= kSidebandOrders; ++m) {
        for (const int side : {1, -1}) {
            // The first order where it was measured, the rest at multiples.
            const double measured = first[side > 0 ? 0 : 1];
            const double distance = m == 1 && measured > 0.0 ? measured : m * ripple;
            if (distance >= static_cast<double>(reach)) {
                continue;
            }
            Fit sideband = grid;
            sideband.c += side * distance;
            const double rel = m == 1 && measured > 0.0 ? level[side > 0 ? 0 : 1]
                                                         : pedestal;
            for (double& h : sideband.heights) {
                h += rel;
            }
            sideband.ks.clear();
            sideband.which.clear();
            out.push_back(std::move(sideband));
        }
    }
    return out;
}

// Every peak on the grid's lines leaves the search, matched or not.
void take_grid(const std::vector<Peak>& peaks, const Fit& grid, std::vector<bool>& taken)
{
    for (std::size_t j = 0; j < peaks.size(); ++j) {
        const double k = grid.d > 0.0 ? std::round((peaks[j].x - grid.c) / grid.d) : 0.0;
        if (k >= static_cast<double>(grid.k_lo) && k <= static_cast<double>(grid.k_hi) &&
            std::abs(peaks[j].x - grid.c - k * grid.d) < kTakenBins) {
            taken[j] = true;
        }
    }
}

// A held comb, as the bank reports it, in bins: its own grid and, with a
// ripple, one grid per sideband order either side, as many as the search
// would have taken out with it had it been found here.
[[nodiscard]] std::vector<Fit> held_grids(const CombEstimate& held, std::size_t n, SampleRate rate)
{
    std::vector<Fit> out;
    if (held.line_count() == 0) {
        return out;
    }
    const double hz_per_bin = static_cast<double>(rate) / static_cast<double>(n);
    Fit grid;
    grid.c = static_cast<double>(n / 2) + held.offset_hz / hz_per_bin;
    grid.d = std::max(held.spacing_hz, 0.0) / hz_per_bin;
    grid.k_lo = held.k_lo;
    grid.k_hi = grid.d > 0.0 ? held.k_hi : held.k_lo;
    out.push_back(grid);
    if (held.ripple_hz > 0.0 && grid.d > 0.0) {
        const int orders = std::max(kSidebandOrders, static_cast<int>(held.sideband_orders));
        for (int m = 1; m <= orders; ++m) {
            for (const int side : {1, -1}) {
                Fit sideband = grid;
                sideband.c += side * m * held.ripple_hz / hz_per_bin;
                out.push_back(sideband);
            }
        }
    }
    return out;
}

// The share of a found grid's positions that sit on a held comb's lines,
// within kCombSameLineHz. CombTracker::overlap's test, in bins.
[[nodiscard]] double held_overlap(const Fit& grid, const Fit& held, double tolerance_bins)
{
    const std::int64_t count = grid.k_hi - grid.k_lo + 1;
    if (count <= 0) {
        return 0.0;
    }
    std::int64_t on = 0;
    for (std::int64_t k = grid.k_lo; k <= grid.k_hi; ++k) {
        const double x = grid.c + static_cast<double>(k) * grid.d;
        const double j = held.d > 0.0 ? std::round((x - held.c) / held.d) : 0.0;
        if (j >= static_cast<double>(held.k_lo) && j <= static_cast<double>(held.k_hi) &&
            std::abs(x - held.c - j * held.d) < tolerance_bins) {
            ++on;
        }
    }
    return static_cast<double>(on) / static_cast<double>(count);
}

[[nodiscard]] double line_power(const Fit& fit)
{
    double sum = 0.0;
    for (const double h : fit.heights) {
        sum += std::pow(10.0, h / 10.0);
    }
    return sum;
}

// The spectrum in dB, or the reason it cannot be read. Empty when the
// spectrum is all zero.
[[nodiscard]] Expected<std::vector<double>> spectrum_db(const CombSpectrum& spectrum)
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
    float largest = 0.0F;
    for (std::size_t i = 0; i < n; ++i) {
        const float p = spectrum.power[i];
        if (!std::isfinite(p) || p < 0.0F) {
            return fail(std::format("comb detection: bin {} holds {}, not a power", i, p));
        }
        largest = std::max(largest, p);
    }
    if (largest <= 0.0F) {
        return std::vector<double>{};
    }
    std::vector<double> db(n);
    const double tiny = static_cast<double>(largest) * 1e-30;
    for (std::size_t i = 0; i < n; ++i) {
        db[i] = 10.0 * std::log10(std::max(static_cast<double>(spectrum.power[i]), tiny));
    }
    return db;
}

}  // namespace

Expected<std::optional<CombEstimate>> detect_comb(const CombSpectrum& spectrum)
{
    auto db = spectrum_db(spectrum);
    if (!db) {
        return std::unexpected(db.error());
    }
    if (db->empty()) {
        return std::optional<CombEstimate>{};
    }
    const std::optional<Fit> fit = best_grid(find_peaks(*db));
    if (!fit) {
        return std::optional<CombEstimate>{};
    }
    return std::optional<CombEstimate>{to_estimate(*fit, db->size(), spectrum.rate)};
}

Expected<std::vector<CombEstimate>> detect_combs(const CombSpectrum& spectrum,
                                                 std::span<const CombEstimate> active)
{
    auto db = spectrum_db(spectrum);
    if (!db) {
        return std::unexpected(db.error());
    }
    std::vector<CombEstimate> out;
    if (db->empty()) {
        return out;
    }
    const std::size_t n = db->size();

    struct Found {
        CombEstimate estimate;
        double power = 0.0;
    };
    std::vector<Found> found;
    const double split_bins = std::max(
        kSplitBins, kSplitHz * static_cast<double>(n) / static_cast<double>(spectrum.rate));
    const double hz_per_bin = static_cast<double>(spectrum.rate) / static_cast<double>(n);
    std::vector<Peak> peaks = find_peaks(*db);

    // What the bank holds leaves the search before the first pass: its
    // carriers and its sidebands, as if each had been found here and taken.
    std::vector<Fit> held;
    std::vector<Fit> held_carriers;
    for (const CombEstimate& estimate : active) {
        std::vector<Fit> grids = held_grids(estimate, n, spectrum.rate);
        if (grids.empty()) {
            continue;
        }
        held_carriers.push_back(grids.front());
        held.insert(held.end(), grids.begin(), grids.end());
    }
    auto drop_taken = [&peaks](const std::vector<bool>& taken) {
        std::vector<Peak> rest;
        for (std::size_t j = 0; j < peaks.size(); ++j) {
            if (!taken[j]) {
                rest.push_back(peaks[j]);
            }
        }
        peaks = std::move(rest);
    };
    if (!held.empty()) {
        std::vector<bool> taken(peaks.size(), false);
        for (const Fit& grid : held) {
            take_grid(peaks, grid, taken);
        }
        drop_taken(taken);
    }
    const double same_line_bins = std::max(kTakenBins, kCombSameLineHz / hz_per_bin);
    auto is_held = [&](const Fit& grid) {
        for (const Fit& carrier : held_carriers) {
            if (held_overlap(grid, carrier, same_line_bins) > kCombOverlapFraction) {
                return true;
            }
        }
        return false;
    };

    // A pass that comes out on a held comb anyway (a sub-grid of it whose
    // lines the fit placed off the held grid by more than kTakenBins) takes
    // its peaks and finds nothing, so passes are capped as well as finds.
    std::uint32_t passes = 0;
    while (found.size() < kCombMaxCombs && passes < 2 * kCombMaxCombs) {
        ++passes;
        const std::optional<Fit> fit = best_grid(peaks);
        if (!fit) {
            break;
        }
        std::vector<bool> taken(peaks.size(), false);
        for (const std::size_t j : fit->which) {
            taken[j] = true;
        }
        if (is_held(*fit)) {
            take_grid(peaks, *fit, taken);
            drop_taken(taken);
            continue;
        }

        // A grid is split into its even and odd lines when the halves, each
        // fitted on its own, do not sit where the grid puts them, and the
        // halves are looked at the same way in turn. Each half is refitted
        // against every peak from its own grid: two sources whose spacings
        // differ by a few hertz walk apart by that much per line, so the joint
        // grid holds only the middle of the span and each half's own grid is
        // what reaches the rest. A true single comb's halves agree with it and
        // it stays whole.
        // Each pending grid carries whether it is already a half of another.
        std::vector<Fit> pending{*fit};
        std::vector<bool> pending_half{false};
        std::vector<Fit> done;
        while (!pending.empty()) {
            Fit grid = std::move(pending.back());
            const bool is_half = pending_half.back();
            pending.pop_back();
            pending_half.pop_back();
            std::optional<Fit> even = parity_fit(grid, peaks, false);
            std::optional<Fit> odd = parity_fit(grid, peaks, true);
            // The refit reaches lines the joint grid missed, and from a poor
            // start can also wander onto other peaks and keep fewer: it is
            // taken only when it holds more lines than the half did.
            for (std::optional<Fit>* half : {&even, &odd}) {
                if (*half) {
                    std::optional<Fit> refit = fit_grid(peaks, (*half)->d, (*half)->c);
                    if (refit && refit->found > (*half)->found) {
                        *half = std::move(refit);
                    }
                }
            }

            // One parity mostly empty: the grid is the other parity's, at
            // twice the spacing. On the 80 m capture of 2026-10-16 a strong
            // 31578.86 Hz family stood on the even lines of a 15789 Hz grid
            // and a faint family 73 Hz off its odd ones, two bins at 36.6 Hz,
            // so no odd line matched; reported at 15789 Hz the comb had the
            // tracker subtract a tone 73 Hz from every faint line, which put
            // a new line beside each instead of removing one.
            const auto parity_slots = static_cast<double>(grid.k_hi - grid.k_lo + 1) / 2.0;
            auto count = [](const std::optional<Fit>& half) {
                return half ? static_cast<double>(half->found) : 0.0;
            };
            auto occupied = [&](const std::optional<Fit>& half) {
                return half && half->found >= kCombMinLines &&
                       count(half) >= kMinOccupancy * parity_slots;
            };
            const bool even_empty = count(even) < kEmptyParity * parity_slots;
            const bool odd_empty = count(odd) < kEmptyParity * parity_slots;
            if (even_empty != odd_empty && occupied(even_empty ? odd : even)) {
                pending.push_back(std::move(even_empty ? *odd : *even));
                pending_half.push_back(is_half);
                continue;
            }
            // At the top only, one half there and the other thin: the thin
            // one is a second family near the half grid, which the search
            // finds next on its own grid. Marked a half, so it is not halved
            // again for weak edges.
            if (!is_half && occupied(even) != occupied(odd)) {
                pending.push_back(std::move(occupied(even) ? *even : *odd));
                pending_half.push_back(true);
                continue;
            }

            // A comb is always held as its two halves when both are there.
            // Two sources at one spacing can sit almost on each other's half
            // grid for a while and then walk apart: on the 80 m capture the
            // offset between the families went from a few hertz to 73 Hz in
            // seconds, and a tracker holding both as one grid is then wrong
            // for half its lines. A half is an exact model of every other
            // line of a true single comb, so nothing is lost by splitting
            // one. Below the first level a half is split again only when its
            // own halves disagree with it.
            const bool room = found.size() + done.size() + pending.size() + 2 <= kCombMaxCombs;
            const bool disagree =
                even && odd && std::max(misplaced(grid, *even, 0), misplaced(grid, *odd, 1)) > split_bins;
            if (room && occupied(even) && occupied(odd) && (!is_half || disagree)) {
                pending.push_back(std::move(*even));
                pending_half.push_back(true);
                pending.push_back(std::move(*odd));
                pending_half.push_back(true);
            } else {
                done.push_back(std::move(grid));
            }
        }
        // Every peak on a taken grid leaves the search, matched or not: a line
        // the fit missed would otherwise come back next time round as the
        // same comb at a sub-grid of it, and be cancelled twice.
        //
        // Each comb with its modulation sidebands. The sidebands go to the
        // tracker as part of the comb, ripple_hz and sideband_orders, and
        // leave the search so they do not come back as combs of their own:
        // on the 80 m capture a strong family's first sidebands stood higher
        // than a second, weaker family and were found ahead of it, and a
        // tracker started on a sideband comb is pulled onto the carriers a
        // hundred hertz away. A half that lies on a held comb is taken the
        // same way and not reported.
        for (const Fit& grid : done) {
            double ripple_bins = 0.0;
            const std::vector<Fit> sides = sideband_grids(grid, peaks, *db, spectrum.rate, ripple_bins);
            if (!is_held(grid)) {
                CombEstimate estimate = to_estimate(grid, n, spectrum.rate);
                if (ripple_bins > 0.0) {
                    estimate.ripple_hz = ripple_bins * hz_per_bin;
                    estimate.sideband_orders = kTrackedOrders;
                }
                found.push_back({estimate, line_power(grid)});
            }
            for (const std::size_t j : grid.which) {
                taken[j] = true;
            }
            take_grid(peaks, grid, taken);
            for (const Fit& side : sides) {
                take_grid(peaks, side, taken);
            }
        }
        drop_taken(taken);
    }

    std::ranges::stable_sort(found, [](const Found& l, const Found& r) { return l.power > r.power; });
    for (const Found& f : found) {
        out.push_back(f.estimate);
    }
    return out;
}

}  // namespace revenant::dsp
