// CombTracker from core/dsp/comb_cancel.h: the host side of the span-wide comb
// canceller. Pure, single-threaded bookkeeping on absolute sample indices, in
// the manner of ExcisionCursor in core/dsp/impulse_excision.h.
//
// THE MODEL STATE. c and d are each an exact fixed-point polynomial in the
// sample index (Axis in the header): phase, frequency and frequency rate, all
// mod 2^64 in units of a cycle. Line k's phase is phase_c + k * phase_d and its
// frequency freq_c + k * freq_d, computed in wrapping uint64 arithmetic, so the
// lines are exactly consistent with the model and advancing the model by any
// number of samples, forwards or back, accumulates no rounding. 2^-64 cycles
// per sample is 1.3e-13 Hz at 2.4 MS/s and the rate resolves 3e-7 Hz/s. Only
// the corrections the loops apply pass through a double, and they are small.
//
// A PLAN anchors each line at the block's middle sample m = start + count / 2:
// step is the model's frequency there rounded to 32 bits, and phase0 is chosen
// so that phase0 + (count / 2) * step is the model's phase at m rounded to 32
// bits. The kernel's tone is then exactly the planned linear phase, known to
// the host to the bit, and departs from the model only by the chirp across half
// a block: 0.5 * rate * (count / 2)^2, 0.003 cycles at 30 Hz/s with 65536
// samples at 2.4 MS/s.
//
// SUB-BLOCKS. The largest of 16, 8, 4, 2 that divides the block. The phase
// advance between adjacent sub-blocks of L samples is unambiguous for a
// frequency error under rate / (2 L): with 65536 samples at 2.4 MS/s, L = 4096
// and the pull-in is +/- 293 Hz, two to six detection bins of 50 to 150 Hz, and
// a 150 Hz error costs a sub-block only sinc(150 * 4096 / 2.4e6) = 0.9 of its
// coherent gain. A larger block narrows it in proportion: at 262144 samples the
// pull-in is 73 Hz, under one coarse bin, which is the figure to check before
// growing the graph's block.
//
// AN OBSERVATION is interpreted against the record of what was planned for its
// block (records_), never against the current model, because the projection
// is relative to the tones the kernel actually used. Per line, from the S
// sub-block projections p_s:
//
//   D = sum p_{s+1} conj(p_s)      phase step per sub-block, so the line's
//                                  frequency against the plan is arg D / 2 pi L
//   c = sum p_s conj(z)^s / S       z = D / |D|: the sub-block amplitude with
//                                  the frequency error taken out, whose power
//                                  is the line's and whose scatter about c z^s
//                                  is the noise near it
//   P = sum p_s                     the block's projection, phase at its middle
//
// The planned phase and frequency at m plus those residuals are the line's
// absolute phase and frequency at m. The current model is retrodicted to m by
// the same exact integer advance, and the difference is the innovation: the
// error of the model as it stands now, seen at m. That is what removes the
// frames-in-flight delay from the loop's stability: a correction made since
// the block was planned is already in the model the innovation is taken
// against, so nothing is corrected twice, and the error dynamics are those of
// the undelayed loop. The delay still costs prediction: whatever the comb does
// between m and the newest planned block is extrapolated from the model.
//
// THE FIT. Per-line innovations are fitted by weighted least squares to
// e_k = e_c + k * e_d, weights the line's SNR in a block's resolution (its
// averaged tone power times count over its noise power), which is the inverse
// variance of a phase or frequency estimate to within a constant. Lines under
// kFitSnr (9 dB) are left out of the fits: at four sub-blocks' worth of that the
// sub-block phases are already noisy enough that arg D wraps on noise. The
// weighted RMS residual of the phase fit is the lock statistic: a comb's lines
// move together and leave it near the noise, unrelated carriers on a grid do
// not.
//
// TWO STAGES OF ACQUISITION.
//
//   frequency  A second-order frequency-locked loop on the sub-block
//              discriminator: freq += 0.5 e, rate += e / 16 per block (poles
//              at 0.85 and 0.59, no overshoot to speak of). It leaves when the
//              fitted error at the lines' centroid and across their span are
//              both under 0.1 cycles per block for three blocks: 3.7 Hz at
//              65536 samples and 2.4 MS/s. Phase is not measured yet: with
//              tens of hertz of error the block projection has no coherent
//              phase to measure.
//
//   phase      A third-order phase-locked loop, alpha-beta-gamma with all three
//              poles at rho = exp(-T / 0.05 s) per block of T seconds, which
//              gives alpha = 1 - rho^3, beta = 1.5 (1 - rho)^2 (1 + rho),
//              gamma = (1 - rho)^3 (phase += alpha e, freq += beta e / T,
//              rate += gamma e / T^2). At 65536 samples and 2.4 MS/s rho is
//              0.58: a 50 ms time constant, about 3 Hz of loop bandwidth.
//              Because the innovation is taken against the retrodicted model,
//              the 2 to 4 frames in flight (55 to 110 ms) do not move the
//              poles; what they cost is extrapolation, and that sets the
//              bandwidth. A frequency rate of change gives a constant phase
//              error jerk * T^3 / gamma, so a faster loop follows a wandering
//              supply better, but its frequency noise is carried forward over
//              the latency and costs a static comb depth. Measured in the
//              closed-loop tests (lines 15 to 25 dB over the floor in a
//              block's resolution, four frames in flight): a static comb 28 dB
//              at 0.05 s against 33 dB at 0.1 s and 22 dB at 0.03 s; an offset
//              accelerating at 4 Hz/s^2 22 dB at 0.05 s against 15 dB at 0.1
//              s; an offset swinging 5 Hz at 3.7 s 15 dB against 7 dB. 0.05 s
//              is where those cross. The first block in this stage seeds each
//              line's phase reference, its complex amplitude, and corrects
//              nothing.
//
// AMPLITUDE. Each line's complex amplitude, against the model's phase, is
// averaged with weight 1/16 per block (0.44 s at 65536 samples) from the block
// projection with the fitted affine phase error taken out. Taking the affine
// part out leaves the common motion to the loop alone, so the amplitude holds
// only the line's own phase and magnitude and the phase innovation stays a
// clean error signal. A line whose tone power is under kStandOut (6 dB) times
// its noise in one block's resolution, a gap in the grid, is given zero
// amplitude: subtracting it would put its estimate's noise in for nothing.
//
// TWO MODELS. Most combs are exact harmonics and their lines lie on a straight
// line in k. Some combs' period grows away from the centre, and their lines
// lie on c + k d + k^2 e. The model always carries e, as an exact axis like c
// and d, and uses it only while the lines call for it: choose_model takes up
// the curved fit when it explains the frequency innovations decisively better
// over a wide enough span, and drops back, setting e to exactly zero, when the
// tracked curvature has stayed negligible. Under the straight model e is never
// corrected and stays zero. Recovery keeps whichever model was in use.
//
// THE FITS ARE ROBUST. Both are reweighted twice (Huber, past 2.5 times the
// median residual), so a carrier sharing its bin with a station neither steers
// the comb nor reads as the comb failing. Each carrier is then judged against
// the phase fit on its own: out by more than 0.8 rad for four blocks running
// and it is BENCHED, out of the fits and not subtracted, its sidebands with
// it; in step for eight and it comes back. A pass is judged by the carriers
// moving together, not by its worst ones: the residual below is over the
// carriers not benched, and more than half benched is no comb.
//
// LOCK.
//   Acquiring -> Locked   eight consecutive phase-stage blocks with the
//                         weighted RMS phase residual under 0.25 rad (12 dB of
//                         cancellation at worst from phase alone), the fitted
//                         phase innovation under 0.03 cycles across the lines
//                         (the loop has pulled in, not merely fits), at least
//                         kCombMinLines lines standing out and no more than
//                         half benched. About 37 blocks from a detection 90 Hz
//                         off, 1 s.
//   Locked -> Recovering  six consecutive blocks with that residual over
//                         0.6 rad, too few lines standing out, most benched,
//                         or the frequency innovation over 0.25 cycles per
//                         block. Cancellation stops but the model is kept:
//                         frequencies held, rates dropped, phase references
//                         retaken, and the loops pull back in from the
//                         frequency stage on long sub-blocks. A burst of noise
//                         or the comb going quiet for a second used to end the
//                         pass outright and wait for a fresh detection.
//   Recovering -> Locked  the acquisition test again.
//   Recovering -> Lost    not locked again within 8 s.
//   Acquiring -> Lost     no lock within 200 blocks, 5.5 s at 65536 samples.
// Lost and Idle plan nothing. A detection that disagrees with the model, or
// any detection while Lost or Idle, restarts acquisition. While acquiring, a
// detection that agrees is ignored as it is while locked: detections arrive
// about once a second, about as long as acquisition takes, and restarting on
// each would never finish.
//
// TRIMMING. A comb can have more lines than kCombMaxLines: a 4-bin comb across
// a full span is about 3500. Acquisition takes the kCombMaxLines central to
// the estimate's range, the lines the detection's grid fit is best at. Once
// locked, a line that has not stood out for 64 consecutive blocks (1.7 s) is
// swapped for the next untried k, alternately above and below the active set,
// so the slots drift toward the lines that are actually there. The candidate
// order is outward from the centre because that is where the model is best
// known; extrapolating k far outside the fitted range multiplies the spacing
// error by k.
//
// A SPUR (CombEstimate::spur) is the same machinery on one line: k = 0 only,
// d held at zero, no sidebands, and every count that asks a comb for
// kCombMinLines asks it for one. The fits are then that line's own error with
// nothing left over, so the phase residual is zero by construction and lock
// rests on the other three tests: the innovation pulled in, the line standing
// out, and the frequency error. It is lost when the carrier goes, because its
// projection reads the raw line.
//
// SUPPRESSION is measured from the projections themselves. The project pass
// reads the raw samples, so for a block planned with subtract on, P / count
// minus the planned amplitude is the line's residual after subtract: both are
// against the same tones. Power before and after are averaged per line, the
// noise's share (noise / count) taken out of each, and the figure is the mean
// over cancelled lines of before over after in dB, clamped to [0, 60]. The
// average cannot see a residual much under the noise in one block, so a line's
// figure stops at about its own SNR in a block plus 3 dB: the report is a lower
// bound, 2 to 8 dB under the truth in the tests.
//
// PROVENANCE, per docs/clean-room.md. Phase- and frequency-locked loops,
// alpha-beta-gamma tracking and weighted least squares are textbook; the
// arrangement and every constant here are this project's own. No document was
// opened and no other program's source was read.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <vector>

#include "core/dsp/comb_cancel.h"

namespace revenant::dsp {

namespace {

using Cd = std::complex<double>;

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kTwo64 = 18446744073709551616.0;
constexpr double kTwo32 = 4294967296.0;

constexpr double kFitSnr = 8.0;      // 9 dB in a block's resolution: used in the fits
constexpr double kStandOut = 4.0;    // 6 dB: given an amplitude
constexpr double kSnrCap = 1.0e4;    // 40 dB: no one line carries a fit alone
// A sideband's averaged phasor holding this share of its average power: its
// phase wanders under about 45 degrees against the tracked ripple.
constexpr double kSideCoherence = 0.5;
constexpr double kStatWeight = 1.0 / 8.0;
constexpr double kAmpWeight = 1.0 / 16.0;
constexpr double kLoopSeconds = 0.05;
constexpr double kFllGain = 0.5;
constexpr double kFllRateGain = 0.0625;
constexpr double kFllExitCyclesPerBlock = 0.1;
constexpr std::uint32_t kFllExitBlocks = 3;
constexpr double kPhaseFallbackCyclesPerBlock = 0.25;
constexpr double kLockRad = 0.25;
constexpr double kLockInnovationCycles = 0.03;
constexpr std::uint32_t kLockBlocks = 8;
constexpr double kLostRad = 0.6;
constexpr std::uint32_t kLostBlocks = 6;
constexpr std::uint32_t kAcquireLimit = 200;
// A carrier whose phase residual against the fit is over this, about three
// times the scatter of a line at the fit threshold, is out of step this block.
constexpr double kLineOutlierRad = 0.8;
constexpr std::uint32_t kBenchBlocks = 4;
constexpr std::uint32_t kUnbenchBlocks = 8;
// Fewer of the fitted carriers than this share moving together is not a comb.
constexpr double kHealthyShare = 0.5;
// How long a pass that was locked keeps its model while pulling back in. A
// burst of noise or the comb going quiet for a second or two is recovered
// from; a comb gone for longer is let go and its slots released.
constexpr double kRecoverSeconds = 8.0;
// A recovery still short of lock this long takes a detection's model.
constexpr double kRecentreSeconds = 1.0;
// Carriers a healthy pass is given per block when it grows.
constexpr std::uint32_t kGrowCarriers = 16;
// Huber: residuals past this many times the median residual are weighted down.
constexpr double kHuber = 2.5;
// Model choice, CombTracker::choose_model.
constexpr std::size_t kCurveMinLines = 24;
constexpr double kCurveMinSpan = 16.0;
constexpr double kCurveGain = 0.5;
constexpr double kCurveOnCycles = 0.05;
constexpr double kCurveOffCycles = 0.02;
constexpr std::uint32_t kCurveVotes = 4;
constexpr std::uint32_t kStraightVotes = 64;
constexpr std::uint32_t kSwapBlocks = 64;
constexpr double kAgreeHz = 150.0;
// Lines further than 0.45 of the rate from centre, 90% of the way to the band
// edge, sit in the anti-alias roll-off and are neither fitted nor cancelled.
constexpr double kBandEdge = 0.45;
constexpr double kGapSeconds = 0.5;
constexpr std::size_t kMaxRecords = 32;

// Wraps to [-0.5, 0.5) and scales to the fixed point, two's complement.
std::uint64_t to_fixed(double cycles)
{
    double x = cycles - std::round(cycles);
    x = std::clamp(x, -0.5, 0.4999999999);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(x * kTwo64)));
}

// Small quantities (a frequency rate) that never come near half a cycle.
std::uint64_t to_fixed_small(double value)
{
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(value * kTwo64)));
}

double signed_of(std::uint64_t value)
{
    return static_cast<double>(static_cast<std::int64_t>(value)) / kTwo64;
}

double wrap_cycles(double x)
{
    return x - std::round(x);
}

// n * (n - 1) / 2 exactly mod 2^64, for any signed n.
std::uint64_t triangle(std::int64_t n)
{
    std::int64_t half = 0;
    std::int64_t other = 0;
    if (n % 2 == 0) {
        half = n / 2;
        other = n - 1;
    } else {
        half = (n - 1) / 2;
        other = n;
    }
    return static_cast<std::uint64_t>(half) * static_cast<std::uint64_t>(other);
}

struct Axes {
    std::uint64_t phase = 0;
    std::uint64_t freq = 0;
    std::uint64_t rate = 0;
};

template <class A>
Axes advanced(const A& axis, std::int64_t n)
{
    const auto un = static_cast<std::uint64_t>(n);
    return Axes{axis.phase + axis.freq * un + axis.rate * triangle(n), axis.freq + axis.rate * un,
                axis.rate};
}

std::uint64_t times_k(std::uint64_t value, std::int32_t k)
{
    return value * static_cast<std::uint64_t>(static_cast<std::int64_t>(k));
}

std::uint64_t times_k2(std::uint64_t value, std::int32_t k)
{
    const auto kk = static_cast<std::int64_t>(k) * static_cast<std::int64_t>(k);
    return value * static_cast<std::uint64_t>(kk);
}

// y(k) = at_zero + slope k + curve k^2; curve is zero for the straight model.
struct Fit {
    double at_zero = 0.0;  // e_c
    double slope = 0.0;    // e_d
    double curve = 0.0;    // e_e
    double centroid_value = 0.0;
    double half_span_value = 0.0;  // the fit's largest departure across the lines from its centroid value
    double rms = 0.0;              // weighted RMS residual
    std::uint32_t used = 0;
};

double eval(const Fit& fit, double k)
{
    return fit.at_zero + fit.slope * k + fit.curve * k * k;
}

// The k of the grid line c + k d + k^2 e nearest f. Rounding against the
// straight grid first and correcting once for the curvature at that k is
// exact while the curvature moves a line by less than half a spacing between
// neighbours, which any comb the detector can see as a comb satisfies.
double nearest_k(double f, double c, double d, double e)
{
    const double k0 = std::round((f - c) / d);
    return std::round((f - c - e * k0 * k0) / d);
}

Fit fit_affine(const std::vector<double>& k, const std::vector<double>& y,
               const std::vector<double>& w);

// Weighted least squares for the curved model, in k about its weighted mean
// so the normal equations stay well conditioned at k in the hundreds. Too few
// lines, or a span the curvature cannot be told from, falls back to straight.
Fit fit_quadratic(const std::vector<double>& k, const std::vector<double>& y,
                  const std::vector<double>& w)
{
    if (k.size() < 4) {
        return fit_affine(k, y, w);
    }
    double sw = 0.0;
    double sk = 0.0;
    for (std::size_t i = 0; i < k.size(); ++i) {
        sw += w[i];
        sk += w[i] * k[i];
    }
    if (sw <= 0.0) {
        return Fit{};
    }
    const double kbar = sk / sw;
    double s2 = 0.0;
    double s3 = 0.0;
    double s4 = 0.0;
    double t0 = 0.0;
    double t1 = 0.0;
    double t2 = 0.0;
    double kmin = k[0];
    double kmax = k[0];
    for (std::size_t i = 0; i < k.size(); ++i) {
        const double u = k[i] - kbar;
        const double u2 = u * u;
        s2 += w[i] * u2;
        s3 += w[i] * u2 * u;
        s4 += w[i] * u2 * u2;
        t0 += w[i] * y[i];
        t1 += w[i] * u * y[i];
        t2 += w[i] * u2 * y[i];
        kmin = std::min(kmin, k[i]);
        kmax = std::max(kmax, k[i]);
    }
    // [sw 0 s2; 0 s2 s3; s2 s3 s4] [a b q] = [t0 t1 t2], by Cramer's rule.
    const double det = sw * (s2 * s4 - s3 * s3) - s2 * (s2 * s2);
    if (!(std::abs(det) > 1e-12 * sw * s2 * s4)) {
        return fit_affine(k, y, w);
    }
    const double a = (t0 * (s2 * s4 - s3 * s3) + s2 * (t1 * s3 - s2 * t2)) / det;
    const double b = (sw * (t1 * s4 - s3 * t2) + s2 * (t0 * s3 - s2 * t1)) / det;
    const double q = (sw * (s2 * t2 - s3 * t1) - t0 * s2 * s2) / det;
    Fit fit;
    fit.curve = q;
    fit.slope = b - 2.0 * q * kbar;
    fit.at_zero = a - b * kbar + q * kbar * kbar;
    fit.centroid_value = a;
    const double lo = kmin - kbar;
    const double hi = kmax - kbar;
    fit.half_span_value = std::max(std::abs(b * lo + q * lo * lo), std::abs(b * hi + q * hi * hi));
    double sr = 0.0;
    for (std::size_t i = 0; i < k.size(); ++i) {
        const double r = y[i] - eval(fit, k[i]);
        sr += w[i] * r * r;
    }
    fit.rms = std::sqrt(sr / sw);
    fit.used = static_cast<std::uint32_t>(k.size());
    return fit;
}

Fit fit_affine(const std::vector<double>& k, const std::vector<double>& y,
               const std::vector<double>& w)
{
    Fit fit;
    double sw = 0.0;
    double sk = 0.0;
    for (std::size_t i = 0; i < k.size(); ++i) {
        sw += w[i];
        sk += w[i] * k[i];
    }
    // One line, a spur's: the common error is that line's own, there is no
    // slope to fit and nothing left over. A comb never reaches here with one
    // line counted, since every use asks for kCombMinLines.
    if (k.size() == 1 && sw > 0.0) {
        fit.at_zero = y[0];
        fit.centroid_value = y[0];
        fit.used = 1;
        return fit;
    }
    if (k.size() < 2 || sw <= 0.0) {
        return fit;
    }
    const double kbar = sk / sw;
    double skk = 0.0;
    double sky = 0.0;
    double sy = 0.0;
    double kmin = k[0];
    double kmax = k[0];
    for (std::size_t i = 0; i < k.size(); ++i) {
        const double dk = k[i] - kbar;
        skk += w[i] * dk * dk;
        sky += w[i] * dk * y[i];
        sy += w[i] * y[i];
        kmin = std::min(kmin, k[i]);
        kmax = std::max(kmax, k[i]);
    }
    const double mean = sy / sw;
    const double slope = skk > 0.0 ? sky / skk : 0.0;
    double sr = 0.0;
    for (std::size_t i = 0; i < k.size(); ++i) {
        const double r = y[i] - mean - slope * (k[i] - kbar);
        sr += w[i] * r * r;
    }
    fit.slope = slope;
    fit.at_zero = mean - slope * kbar;
    fit.centroid_value = mean;
    fit.half_span_value = std::abs(slope) * 0.5 * (kmax - kmin);
    fit.rms = std::sqrt(sr / sw);
    fit.used = static_cast<std::uint32_t>(k.size());
    return fit;
}

// The affine fit with lines that disagree weighted down, twice reweighted
// (Huber). One line on a station, or a handful, then neither steers the comb
// nor reads as the comb failing to fit. `floor` keeps a clean fit, whose
// median residual is tiny, from treating ordinary scatter as outliers.
Fit fit_robust(const std::vector<double>& k, const std::vector<double>& y,
               const std::vector<double>& w, double floor, bool curved)
{
    auto fit_of = [curved](const std::vector<double>& kk, const std::vector<double>& yy,
                           const std::vector<double>& ww) {
        return curved ? fit_quadratic(kk, yy, ww) : fit_affine(kk, yy, ww);
    };
    Fit fit = fit_of(k, y, w);
    if (k.size() < 3) {
        return fit;
    }
    std::vector<double> r(k.size());
    std::vector<double> rw(w);
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < k.size(); ++i) {
            r[i] = std::abs(y[i] - eval(fit, k[i]));
        }
        std::vector<double> sorted(r);
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const double c = std::max(kHuber * sorted[sorted.size() / 2], floor);
        for (std::size_t i = 0; i < k.size(); ++i) {
            rw[i] = r[i] > c ? w[i] * c / r[i] : w[i];
        }
        fit = fit_of(k, y, rw);
    }
    return fit;
}

// The shortest sub-block the frequency discriminator is given, in seconds.
//
// A real comb's lines carry sidebands: the owner's HF comb (152 lines at
// 15789.4 Hz, 2026-10-12) has a pair 100.7 Hz either side of every line, a few
// dB under it, which is supply ripple modulating the switcher. A sub-block
// much shorter than the sideband spacing lets them through the discriminator
// as tens of hertz of block-to-block frequency noise, and the frequency loop
// never settled: at 16 sub-blocks of a 65536-sample block at 2.4 MS/s (1.7 ms
// each) it sat in acquisition for the whole run, and settled 25 Hz off the true
// offset. At 6.8 ms it locked in 24 blocks; at 3.4 ms it did not lock.
//
// Long sub-blocks narrow the pull-in, rate / (2 * sub-block), to 73 Hz at
// 2.4 MS/s, under the bin or two a detection can be off. So acquisition
// starts on the shortest sub-blocks, for the widest pull-in, and moves to
// these once the frequency loop has the comb within kNarrowCyclesPerBlock.
constexpr double kMinSubBlockSeconds = 0.005;

// The frequency error, in cycles per block, under which acquisition moves
// from short sub-blocks to long ones. Well inside the long sub-blocks'
// pull-in, which is kCombMaxSubBlocks / 2 / 4 cycles per block at four
// sub-blocks, and well above what the short ones read on a comb with
// sidebands.
constexpr double kNarrowCyclesPerBlock = 1.0;

// `wide` asks for the shortest sub-blocks the count allows.
std::uint32_t pick_sub_blocks(std::uint32_t count, SampleRate rate, bool wide)
{
    const auto shortest =
        wide ? 16U
             : std::max<std::uint32_t>(16U, static_cast<std::uint32_t>(
                                                 kMinSubBlockSeconds * static_cast<double>(rate)));
    for (std::uint32_t s = kCombMaxSubBlocks; s > 1; s /= 2) {
        if (count % s == 0 && count / s >= shortest) {
            return s;
        }
    }
    return 1;
}

}  // namespace

CombTracker::CombTracker(SampleRate rate, std::uint32_t max_lines)
    : rate_(rate), max_lines_(std::clamp<std::uint32_t>(max_lines, 1, kCombMaxLines))
{
}

void CombTracker::reset()
{
    const SampleRate rate = rate_;
    const std::uint64_t generation = generation_ + 1;
    const std::uint64_t id = id_;
    *this = CombTracker(rate, max_lines_);
    generation_ = generation;
    id_ = id;
}

double CombTracker::line_snr(const Line& line, std::uint32_t count) const
{
    if (line.fresh || line.noise <= 0.0 || count == 0) {
        return 0.0;
    }
    const double n = static_cast<double>(count);
    const double tone = std::max(line.coherent - line.noise / n, 0.0);
    return tone * n / line.noise;
}

// A carrier stands out on its own SNR. A sideband cannot be judged that way:
// its sub-block projections carry the carrier a hundred hertz off at nearly
// the sideband's own level, so its scatter reads as noise. It stands out when
// its carrier does and its own averaged phasor is coherent, which is what a
// tracked ripple gives and noise does not.
bool CombTracker::stands_out(std::size_t slot, std::uint32_t count) const
{
    const Line& line = lines_[slot];
    if (line.m == 0) {
        return line_snr(line, count) >= kStandOut;
    }
    const std::size_t carrier = slot - static_cast<std::size_t>(static_cast<std::ptrdiff_t>(line.m));
    if (carrier >= lines_.size() || lines_[carrier].m != 0 ||
        line_snr(lines_[carrier], count) < kStandOut) {
        return false;
    }
    return line.block_power > 0.0 && std::norm(line.amp) >= kSideCoherence * line.block_power;
}

double CombTracker::ripple_hz() const
{
    if (orders_ == 0) {
        return 0.0;
    }
    return anchored_ ? signed_of(r_.freq) * static_cast<double>(rate_) : initial_ripple_hz_;
}

CombEstimate CombTracker::estimate() const
{
    CombEstimate out;
    if (!active()) {
        return out;
    }
    const double rate = static_cast<double>(rate_);
    out.offset_hz = anchored_ ? signed_of(c_.freq) * rate : initial_offset_hz_;
    out.spacing_hz = anchored_ ? signed_of(d_.freq) * rate : initial_spacing_hz_;
    out.curvature_hz = anchored_ ? signed_of(e_.freq) * rate : initial_curvature_hz_;
    out.k_lo = range_lo_;
    out.k_hi = range_hi_;
    for (const Line& line : lines_) {
        out.k_lo = std::min(out.k_lo, line.k);
        out.k_hi = std::max(out.k_hi, line.k);
    }
    out.ripple_hz = ripple_hz();
    out.sideband_orders = orders_;
    out.spur = spur_;
    if (spur_) {
        out.spacing_hz = 0.0;
        out.curvature_hz = 0.0;
        out.k_lo = 0;
        out.k_hi = 0;
    }
    return out;
}

bool CombTracker::holds_frequency(double frequency_hz, double tolerance_hz) const
{
    if (!active()) {
        return false;
    }
    const double rate = static_cast<double>(rate_);
    const double c = anchored_ ? signed_of(c_.freq) * rate : initial_offset_hz_;
    const double d = spur_ ? 0.0 : (anchored_ ? signed_of(d_.freq) * rate : initial_spacing_hz_);
    const double e = spur_ ? 0.0 : (anchored_ ? signed_of(e_.freq) * rate : initial_curvature_hz_);
    const double r = ripple_hz();
    return std::ranges::any_of(lines_, [&](const Line& line) {
        const double k = static_cast<double>(line.k);
        const double f = c + k * d + k * k * e + static_cast<double>(line.m) * r;
        return std::abs(f - frequency_hz) < tolerance_hz;
    });
}

bool CombTracker::agrees(const CombEstimate& estimate) const
{
    if (spur_ || estimate.spur) {
        // A spur agrees only with a spur, and only on its own line.
        if (!(spur_ && estimate.spur)) {
            return false;
        }
        const double c = anchored_ ? signed_of(c_.freq) * static_cast<double>(rate_)
                                   : initial_offset_hz_;
        return std::abs(estimate.offset_hz - c) < kCombSameLineHz;
    }
    if (!anchored_) {
        // Nothing planned yet: compare against the estimate acquisition began from.
        const double d = initial_spacing_hz_;
        if (d <= 0.0) {
            return false;
        }
        const double j = std::round((estimate.offset_hz - initial_offset_hz_) / d);
        const double err_c = estimate.offset_hz - (initial_offset_hz_ + j * d);
        const double half_span = 0.5 * static_cast<double>(estimate.line_count());
        return std::abs(err_c) < kAgreeHz &&
               std::abs(estimate.spacing_hz - d) * half_span < kAgreeHz;
    }
    const double rate = static_cast<double>(rate_);
    const double c = signed_of(c_.freq) * rate;
    const double d = signed_of(d_.freq) * rate;
    if (d <= 0.0) {
        return false;
    }
    const double j = std::round((estimate.offset_hz - c) / d);
    const double err_c = estimate.offset_hz - (c + j * d);
    const double half_span = 0.5 * static_cast<double>(estimate.line_count());
    return std::abs(err_c) < kAgreeHz && std::abs(estimate.spacing_hz - d) * half_span < kAgreeHz;
}

double CombTracker::overlap(const CombEstimate& estimate, double tolerance_hz) const
{
    if (!active() || lines_.empty() || estimate.line_count() == 0) {
        return 0.0;
    }
    const double rate = static_cast<double>(rate_);
    const double c = anchored_ ? signed_of(c_.freq) * rate : initial_offset_hz_;
    const double d = anchored_ ? signed_of(d_.freq) * rate : initial_spacing_hz_;
    if (spur_) {
        std::uint32_t on = 0;
        for (std::int32_t k = estimate.k_lo; k <= estimate.k_hi; ++k) {
            if (std::abs(estimate.offset_hz + k * estimate.spacing_hz - c) < tolerance_hz) {
                ++on;
            }
        }
        return static_cast<double>(on) / static_cast<double>(estimate.line_count());
    }
    if (!(d > 0.0)) {
        return 0.0;
    }
    // The whole detected range, not only the lines holding slots: a pass on
    // a quarter of a comb still holds the comb, and a detection of the rest
    // must not start a second pass on the same grid. detect_combs judges held
    // combs over the same range (CombTracker::estimate).
    std::int32_t lo = range_lo_;
    std::int32_t hi = range_hi_;
    for (const Line& line : lines_) {
        lo = std::min(lo, line.k);
        hi = std::max(hi, line.k);
    }
    // Judged over the estimate's lines that fall inside this pass's range:
    // a detection of the whole comb is the same comb as a pass on part of it,
    // whatever share of the whole that part is. Interleaved grids and
    // sideband grids land inside the range off every line and still score
    // nothing. Too few inside to judge by, and it is the share of the whole.
    const double e = anchored_ ? signed_of(e_.freq) * rate : initial_curvature_hz_;
    std::uint32_t on = 0;
    std::uint32_t inside = 0;
    for (std::int32_t k = estimate.k_lo; k <= estimate.k_hi; ++k) {
        const double kk = static_cast<double>(k);
        const double f =
            estimate.offset_hz + kk * estimate.spacing_hz + kk * kk * estimate.curvature_hz;
        const double j = nearest_k(f, c, d, e);
        if (j >= lo && j <= hi) {
            ++inside;
            if (std::abs(f - (c + j * d + j * j * e)) < tolerance_hz) {
                ++on;
            }
        }
    }
    const std::uint32_t over = inside >= kCombMinLines ? inside : estimate.line_count();
    return static_cast<double>(on) / static_cast<double>(over);
}

void CombTracker::extend_range(const CombEstimate& estimate)
{
    if (spur_ || estimate.spur || !active() || estimate.line_count() == 0) {
        return;
    }
    const double rate = static_cast<double>(rate_);
    const double c = anchored_ ? signed_of(c_.freq) * rate : initial_offset_hz_;
    const double d = anchored_ ? signed_of(d_.freq) * rate : initial_spacing_hz_;
    if (!(d > 0.0)) {
        return;
    }
    const double e = anchored_ ? signed_of(e_.freq) * rate : initial_curvature_hz_;
    auto to_k = [&](std::int32_t k) {
        const double kk = static_cast<double>(k);
        const double f =
            estimate.offset_hz + kk * estimate.spacing_hz + kk * kk * estimate.curvature_hz;
        return static_cast<std::int32_t>(nearest_k(f, c, d, e));
    };
    const std::int32_t a = to_k(estimate.k_lo);
    const std::int32_t b = to_k(estimate.k_hi);
    range_lo_ = std::min({range_lo_, a, b});
    range_hi_ = std::max({range_hi_, a, b});
}

// Growth is a few carriers a block, so each batch of new lines is checked by
// the fit and the bench before the next arrives; a whole edge of unseeded
// lines at once would let a bad extrapolation into the fit wholesale.
std::uint32_t CombTracker::wants_lines() const
{
    if (spur_ || lock_ != CombLock::kLocked || !verdict_.good ||
        4 * verdict_.benched > verdict_.fitted) {
        return 0;
    }
    const auto per_line = static_cast<std::uint32_t>(2 * orders_ + 1);
    const auto left = static_cast<std::uint32_t>(std::max(0, range_hi_ - next_hi_ + 1) +
                                                 std::max(0, next_lo_ - range_lo_ + 1));
    return std::min(left, kGrowCarriers) * per_line;
}

std::uint32_t CombTracker::grow(std::uint32_t slots)
{
    const auto per_line = static_cast<std::uint32_t>(2 * orders_ + 1);
    std::uint32_t taken = 0;
    while (taken + per_line <= slots) {
        const bool hi_left = next_hi_ <= range_hi_;
        const bool lo_left = next_lo_ >= range_lo_;
        if (!hi_left && !lo_left) {
            break;
        }
        const bool take_hi = hi_left && (next_take_hi_ || !lo_left);
        const std::int32_t k = take_hi ? next_hi_++ : next_lo_--;
        next_take_hi_ = !take_hi;
        for (std::int32_t m = -orders_; m <= orders_; ++m) {
            Line line;
            line.k = k;
            line.m = m;
            lines_.push_back(line);
        }
        taken += per_line;
    }
    max_lines_ += taken;
    return taken;
}

std::uint32_t CombTracker::shrink_to(std::uint32_t slots)
{
    const auto per_line = static_cast<std::size_t>(2 * orders_ + 1);
    const std::size_t floor = std::max<std::size_t>(slots, kCombMinLines * per_line);
    if (spur_ || lines_.size() <= floor) {
        return 0;
    }
    const std::size_t before = lines_.size();
    std::vector<std::int32_t> ks;
    for (std::size_t base = 0; base < lines_.size(); base += per_line) {
        ks.push_back(lines_[base].k);
    }
    std::ranges::sort(ks);
    const std::size_t keep = floor / per_line;
    // The central `keep` carriers stay.
    const std::size_t first = (ks.size() - keep) / 2;
    const std::int32_t lo = ks[first];
    const std::int32_t hi = ks[first + keep - 1];
    std::erase_if(lines_, [&](const Line& line) { return line.k < lo || line.k > hi; });
    next_lo_ = lo - 1;
    next_hi_ = hi + 1;
    max_lines_ = static_cast<std::uint32_t>(lines_.size());
    // Slots moved: plans already handed out no longer name the right lines.
    ++generation_;
    records_.clear();
    return static_cast<std::uint32_t>(before - lines_.size());
}

// Recovery pulls in from the model it held, on the long sub-blocks, which is
// right after a burst and too narrow after the comb has moved while the pass
// could not see it. A detection names where the comb is now; mapped onto the
// pass's own k it gives c, d and e, and the frequency stage takes it from
// there on the wide pull-in. Once per recovery, after the first second, so a
// recovery the loops would finish alone is left to them: a detection is
// coarser than a locked model.
bool CombTracker::same_spacing(const CombEstimate& estimate) const
{
    if (spur_ || estimate.spur || !anchored_ || estimate.line_count() == 0) {
        return false;
    }
    const double d = signed_of(d_.freq) * static_cast<double>(rate_);
    const double half_span = 0.5 * static_cast<double>(estimate.line_count());
    // A quarter spacing at the ends of the span: a recovering model drifts in
    // spacing while it cannot see the comb, by more than kAgreeHz allows
    // across hundreds of lines, and a comb a quarter spacing off at its ends
    // is the same grid, not another one.
    return d > 0.0 && std::abs(estimate.spacing_hz - d) * half_span < 0.25 * d;
}

bool CombTracker::recentre(const CombEstimate& estimate, bool moved)
{
    if (spur_ || estimate.spur || lock_ != CombLock::kRecovering || !anchored_ ||
        estimate.line_count() == 0 || !(estimate.spacing_hz > 0.0)) {
        return false;
    }
    const double rate = static_cast<double>(rate_);
    if (!moved && (recentred_ || static_cast<double>(static_cast<std::int64_t>(
                                     anchor_ - recovering_since_)) < kRecentreSeconds * rate)) {
        return false;
    }
    const double c = signed_of(c_.freq) * rate;
    const double d = signed_of(d_.freq) * rate;
    const double e = signed_of(e_.freq) * rate;
    // The pass's k at the estimate's k = 0: estimate line k sits at pass k + j0.
    const double j0 = std::round(nearest_k(estimate.offset_hz, c, d, e));
    const double ce = estimate.curvature_hz;
    const double nc = estimate.offset_hz - j0 * estimate.spacing_hz + j0 * j0 * ce;
    const double nd = estimate.spacing_hz - 2.0 * j0 * ce;
    c_ = Axis{c_.phase, to_fixed(nc / rate), 0};
    d_ = Axis{d_.phase, to_fixed(nd / rate), 0};
    if (ce != 0.0) {
        e_ = Axis{e_.phase, to_fixed(ce / rate), 0};
        curved_ = true;
    }
    r_.rate = 0;
    recentred_ = true;
    stage_ = Stage::kFrequency;
    wide_ = true;
    settle_blocks_ = 0;
    bad_blocks_ = 0;
    // Plans already handed out were made against the old model.
    ++generation_;
    records_.clear();
    for (Line& line : lines_) {
        line.seeded = false;
    }
    return true;
}

void CombTracker::start_acquisition(const CombEstimate& estimate)
{
    const SampleRate rate = rate_;
    const std::uint64_t generation = generation_ + 1;
    const std::uint64_t id = id_;
    *this = CombTracker(rate, max_lines_);
    generation_ = generation;
    id_ = id;

    lock_ = CombLock::kAcquiring;
    stage_ = Stage::kFrequency;
    wide_ = true;
    initial_offset_hz_ = estimate.offset_hz;
    initial_spacing_hz_ = estimate.spacing_hz;
    initial_curvature_hz_ = estimate.spur ? 0.0 : estimate.curvature_hz;
    curved_ = initial_curvature_hz_ != 0.0;
    range_lo_ = estimate.k_lo;
    range_hi_ = estimate.k_hi;
    spur_ = estimate.spur;
    min_lines_ = spur_ ? 1U : kCombMinLines;
    if (spur_) {
        initial_spacing_hz_ = 0.0;
        range_lo_ = 0;
        range_hi_ = 0;
    }

    // Sidebands only with a ripple to put them at, and only as many orders as
    // leave room for kCombMinLines carriers.
    if (!spur_ && estimate.ripple_hz > 0.0 && estimate.sideband_orders > 0) {
        orders_ = std::min<std::int32_t>(
            estimate.sideband_orders,
            static_cast<std::int32_t>((max_lines_ / kCombMinLines - 1) / 2));
        initial_ripple_hz_ = estimate.ripple_hz;
    }
    const auto per_line = static_cast<std::int64_t>(2 * orders_ + 1);

    // The central max_lines_ of the range, each carrier followed by its
    // sidebands, -orders_ to +orders_.
    const std::int64_t total = spur_ ? 1 : static_cast<std::int64_t>(estimate.line_count());
    const std::int64_t take = std::min<std::int64_t>(total, max_lines_ / per_line);
    const std::int64_t first = range_lo_ + (total - take) / 2;
    lines_.reserve(static_cast<std::size_t>(take * per_line));
    for (std::int64_t i = 0; i < take; ++i) {
        for (std::int32_t m = -orders_; m <= orders_; ++m) {
            Line line;
            line.k = static_cast<std::int32_t>(first + i);
            line.m = m;
            lines_.push_back(line);
        }
    }
    next_lo_ = static_cast<std::int32_t>(first - 1);
    next_hi_ = static_cast<std::int32_t>(first + take);
}

void CombTracker::detected(const CombEstimate& estimate)
{
    if (rate_ <= 0) {
        return;
    }
    if (estimate.spur ? estimate.line_count() != 1
                      : (estimate.line_count() < kCombMinLines || !(estimate.spacing_hz > 0.0))) {
        return;
    }
    // A running tracker holds what it holds: a comb is not restarted as a
    // spur, nor a spur as a comb.
    if (active() && (spur_ != estimate.spur || agrees(estimate))) {
        return;
    }
    start_acquisition(estimate);
}

void CombTracker::advance_to(SampleIndex index)
{
    const auto n = static_cast<std::int64_t>(index - anchor_);
    const Axes c = advanced(c_, n);
    const Axes d = advanced(d_, n);
    const Axes r = advanced(r_, n);
    const Axes e = advanced(e_, n);
    c_ = Axis{c.phase, c.freq, c.rate};
    d_ = Axis{d.phase, d.freq, d.rate};
    r_ = Axis{r.phase, r.freq, r.rate};
    e_ = Axis{e.phase, e.freq, e.rate};
    anchor_ = index;
}

void CombTracker::correct_ripple(SampleIndex at, double dp, double df, double dr)
{
    const double dt = static_cast<double>(static_cast<std::int64_t>(anchor_ - at));
    r_.phase += to_fixed(dp + df * dt + 0.5 * dr * dt * dt);
    r_.freq += to_fixed(df + dr * dt);
    r_.rate += to_fixed_small(dr);
}

// Corrections are errors at index `at`, applied to the model as a change of
// its whole trajectory and carried forward to the anchor.
// The curvature is only moved while the curved model is in use, so under the
// straight model it stays exactly zero.
void CombTracker::correct(SampleIndex at, Correction dc, Correction dd, Correction de)
{
    const double dt = static_cast<double>(static_cast<std::int64_t>(anchor_ - at));
    auto apply = [dt](Axis& axis, const Correction& x) {
        axis.phase += to_fixed(x.phase + x.freq * dt + 0.5 * x.rate * dt * dt);
        axis.freq += to_fixed(x.freq + x.rate * dt);
        axis.rate += to_fixed_small(x.rate);
    };
    apply(c_, dc);
    apply(d_, dd);
    if (curved_) {
        apply(e_, de);
    }
}

// Which model the pass uses, from this block's frequency innovations over
// every carrier fitted, benched ones included: a curved comb under the
// straight model puts its outer lines off the line and on the bench, which is
// exactly where the evidence for the curvature is.
//
// To curved: four blocks running in which the curved fit leaves under half
// the straight fit's residual power and its curvature moves the outermost
// lines by more than 0.05 cycles per block (1.8 Hz at 65536 samples and
// 2.4 MS/s), over at least 24 lines spanning 16. To straight: the tracked
// curvature moving the outermost lines by under 0.02 cycles per block for 64
// blocks, 1.7 s, at which point it is set to exactly zero. Innovations are
// taken against the model, so once the curvature is tracked the evidence for
// it goes quiet; the way back is judged on the model's own curvature, not on
// the innovations, or the pass would flip back and forth.
void CombTracker::choose_model(const std::vector<double>& ks, const std::vector<double>& ys,
                               const std::vector<double>& ws, double block_samples)
{
    if (spur_ || ks.size() < kCurveMinLines) {
        curve_votes_ = 0;
        return;
    }
    const auto [lo, hi] = std::ranges::minmax(ks);
    if (hi - lo < kCurveMinSpan) {
        curve_votes_ = 0;
        return;
    }
    const double half = 0.5 * (hi - lo);
    if (!curved_) {
        const Fit straight = fit_robust(ks, ys, ws, 0.01 / block_samples, false);
        const Fit curved = fit_robust(ks, ys, ws, 0.01 / block_samples, true);
        const double bend = std::abs(curved.curve) * half * half * block_samples;
        const bool better = curved.rms * curved.rms < kCurveGain * straight.rms * straight.rms &&
                            bend > kCurveOnCycles;
        curve_votes_ = better ? curve_votes_ + 1 : 0;
        if (curve_votes_ >= kCurveVotes) {
            curved_ = true;
            curve_votes_ = 0;
            straight_votes_ = 0;
        }
        return;
    }
    // The curvature's bend across the lines, relative to the straight line
    // through the ends, as the model holds it now.
    const double e = signed_of(e_.freq);
    const double bend = std::abs(e) * half * half * block_samples;
    straight_votes_ = bend < kCurveOffCycles ? straight_votes_ + 1 : 0;
    if (straight_votes_ >= kStraightVotes) {
        // Fold what curvature is left into nothing: it is under the off
        // threshold, a fraction of a cycle per block at the outermost line.
        e_ = Axis{};
        curved_ = false;
        straight_votes_ = 0;
    }
}

CombBlockPlan CombTracker::plan(SampleIndex start, std::uint32_t count)
{
    CombBlockPlan out;
    out.start = start;
    out.count = count;
    out.sub_blocks = 1;
    if (rate_ <= 0 || count == 0 || lines_.empty() || !active()) {
        return out;
    }
    const std::uint32_t half = count / 2;
    const SampleIndex mid = start + half;
    const double rate = static_cast<double>(rate_);

    if (!anchored_) {
        anchored_ = true;
        anchor_ = mid;
        c_ = Axis{0, to_fixed(initial_offset_hz_ / rate), 0};
        d_ = Axis{0, to_fixed(initial_spacing_hz_ / rate), 0};
        r_ = Axis{0, to_fixed(initial_ripple_hz_ / rate), 0};
        e_ = Axis{0, to_fixed(initial_curvature_hz_ / rate), 0};
    } else {
        const auto gap = static_cast<std::int64_t>(mid - anchor_);
        if (gap > static_cast<std::int64_t>(kGapSeconds * rate) || gap < 0) {
            // A long gap or a jump back: the rate terms cannot be trusted across
            // it, so hold frequency and start the loops again.
            advance_to(mid);
            c_.rate = 0;
            d_.rate = 0;
            r_.rate = 0;
            e_.rate = 0;
            if (lock_ == CombLock::kLocked) {
                lock_ = CombLock::kRecovering;
                recovering_since_ = mid;
                recentred_ = false;
            } else if (lock_ != CombLock::kRecovering) {
                lock_ = CombLock::kAcquiring;
            }
            stage_ = Stage::kFrequency;
            wide_ = true;
            settle_blocks_ = 0;
            bad_blocks_ = 0;
            observed_ = 0;
            for (Line& line : lines_) {
                line.seeded = false;
            }
        } else {
            advance_to(mid);
        }
    }
    last_count_ = count;

    out.sub_blocks = pick_sub_blocks(count, rate_, wide_);
    out.subtract = lock_ == CombLock::kLocked;
    out.lines.reserve(lines_.size());

    PlanRecord record;
    record.start = start;
    record.mid = mid;
    record.count = count;
    record.sub_blocks = out.sub_blocks;
    record.generation = generation_;
    record.subtract = out.subtract;
    record.lines.reserve(lines_.size());

    for (std::uint32_t slot = 0; slot < lines_.size(); ++slot) {
        Line& line = lines_[slot];
        const std::uint64_t phase = c_.phase + times_k(d_.phase, line.k) +
                                    times_k2(e_.phase, line.k) + times_k(r_.phase, line.m);
        const std::uint64_t freq = c_.freq + times_k(d_.freq, line.k) +
                                   times_k2(e_.freq, line.k) + times_k(r_.freq, line.m);
        const auto step = static_cast<std::uint32_t>((freq + (1ULL << 31)) >> 32);
        const auto phase_mid = static_cast<std::uint32_t>((phase + (1ULL << 31)) >> 32);
        const std::uint32_t phase0 = phase_mid - step * half;

        line.in_band = std::abs(signed_of(freq)) < kBandEdge;

        Complex32 amp{};
        // A sideband follows its carrier onto the bench.
        const std::size_t carrier_slot =
            slot - static_cast<std::size_t>(static_cast<std::ptrdiff_t>(line.m));
        const bool benched = carrier_slot < lines_.size() && lines_[carrier_slot].benched;
        if (out.subtract && line.seeded && line.in_band && !benched && stands_out(slot, count)) {
            amp = Complex32{static_cast<float>(line.amp.real()),
                            static_cast<float>(line.amp.imag())};
        }
        CombLine planned;
        planned.phase0 = phase0;
        planned.step = step;
        planned.amplitude = amp;
        out.lines.push_back(planned);

        record.lines.push_back(PlannedLine{slot, line.k, line.m, phase0, step, amp, line.in_band});
    }

    records_.push_back(std::move(record));
    if (records_.size() > kMaxRecords) {
        records_.erase(records_.begin());
    }
    return out;
}

// A locked pass stopped fitting. Its model was right moments ago, so it is
// kept: frequencies held, rates dropped (they would carry the model away over
// a gap), cancellation off, and the loops pull back in from the frequency
// stage on the long sub-blocks. Every line's phase reference is retaken,
// since after a gap or a burst the old ones are stale.
void CombTracker::begin_recovery(SampleIndex at)
{
    lock_ = CombLock::kRecovering;
    recovering_since_ = at;
    recentred_ = false;
    stage_ = Stage::kFrequency;
    wide_ = false;
    seed_pending_ = false;
    settle_blocks_ = 0;
    bad_blocks_ = 0;
    c_.rate = 0;
    d_.rate = 0;
    r_.rate = 0;
    e_.rate = 0;
    for (Line& line : lines_) {
        line.seeded = false;
    }
}

void CombTracker::replace_weak_lines()
{
    // A carrier is judged on itself; its sidebands, in the slots after it,
    // move with it to the new k.
    const auto per_line = static_cast<std::size_t>(2 * orders_ + 1);
    for (std::size_t base = 0; base + per_line <= lines_.size(); base += per_line) {
        const Line& carrier = lines_[base + static_cast<std::size_t>(orders_)];
        if (carrier.weak_blocks < kSwapBlocks) {
            continue;
        }
        const bool hi_left = next_hi_ <= range_hi_;
        const bool lo_left = next_lo_ >= range_lo_;
        if (!hi_left && !lo_left) {
            return;
        }
        const bool take_hi = hi_left && (next_take_hi_ || !lo_left);
        const std::int32_t k = take_hi ? next_hi_++ : next_lo_--;
        next_take_hi_ = !take_hi;
        for (std::size_t i = 0; i < per_line; ++i) {
            Line fresh;
            fresh.k = k;
            fresh.m = lines_[base + i].m;
            lines_[base + i] = fresh;
        }
    }
}

void CombTracker::observe(const CombObservation& observation)
{
    if (!active()) {
        return;
    }
    auto it = std::find_if(records_.begin(), records_.end(), [&](const PlanRecord& r) {
        return r.start == observation.start;
    });
    if (it == records_.end()) {
        return;
    }
    const PlanRecord record = std::move(*it);
    records_.erase(records_.begin(), it + 1);

    const std::uint32_t S = record.sub_blocks;
    const std::uint32_t given = observation.sub_blocks;
    if (record.generation != generation_ || observation.count != record.count || S == 0 ||
        given < S || given % S != 0 ||
        observation.projections.size() != record.lines.size() * static_cast<std::size_t>(given) ||
        record.count == 0) {
        return;
    }
    // Finer sub-blocks than planned, from a dispatch shared with a tracker
    // that wanted them: adjacent ones add exactly into the planned length.
    std::vector<Complex32> folded;
    std::span<const Complex32> projections = observation.projections;
    if (given != S) {
        const std::uint32_t group = given / S;
        folded.assign(record.lines.size() * static_cast<std::size_t>(S), Complex32{});
        for (std::size_t i = 0; i < folded.size(); ++i) {
            for (std::uint32_t g = 0; g < group; ++g) {
                folded[i] += projections[i * group + g];
            }
        }
        projections = folded;
    }

    const double N = static_cast<double>(record.count);
    const double L = N / static_cast<double>(S);
    const double rate = static_cast<double>(rate_);
    const double T = N;  // samples per observation
    const std::uint32_t half = record.count / 2;

    // The current model, retrodicted to the block's middle.
    const auto back = static_cast<std::int64_t>(record.mid - anchor_);
    const Axes cb = advanced(c_, back);
    const Axes db = advanced(d_, back);
    const Axes rb = advanced(r_, back);
    const Axes eb = advanced(e_, back);

    struct Measured {
        std::uint32_t slot = 0;
        double k = 0.0;
        std::int32_t m = 0;   // sideband order, zero for a carrier
        bool side_fit = false;
        Cd a_meas{};
        double freq_innovation = 0.0;  // cycles per sample
        double snr = 0.0;
        bool fit = false;
    };
    std::vector<Measured> measured;
    measured.reserve(record.lines.size());

    // Each line's whole-block projection, and with sidebands, each with its
    // carrier group's other lines taken out. A first sideband 59.5 Hz from a
    // carrier 19 dB stronger sees it through a 27 ms block's response at
    // about -15 dB, which is more than the sideband itself; every tone in the
    // block is known exactly from its planned phase and step, so the leak of
    // line j into line i's projection is (P_j / N) times the closed-form sum
    // of their tones' product over the block, and is subtracted.
    std::vector<Cd> block_sum(record.lines.size());
    for (std::size_t i = 0; i < record.lines.size(); ++i) {
        for (std::uint32_t s = 0; s < S; ++s) {
            const Complex32 p = projections[i * S + s];
            block_sum[i] += Cd{p.real(), p.imag()};
        }
    }
    std::vector<Cd> cleaned_sum = block_sum;
    if (orders_ > 0) {
        const auto reach = static_cast<std::ptrdiff_t>(2 * orders_);
        const auto count = static_cast<std::ptrdiff_t>(record.lines.size());
        for (std::ptrdiff_t i = 0; i < count; ++i) {
            const PlannedLine& to = record.lines[static_cast<std::size_t>(i)];
            for (std::ptrdiff_t j = std::max<std::ptrdiff_t>(0, i - reach);
                 j <= std::min(count - 1, i + reach); ++j) {
                const PlannedLine& from = record.lines[static_cast<std::size_t>(j)];
                if (j == i || from.k != to.k) {
                    continue;
                }
                const double dphi =
                    static_cast<double>(static_cast<std::int32_t>(from.phase0 - to.phase0)) / kTwo32;
                const double dw =
                    static_cast<double>(static_cast<std::int32_t>(from.step - to.step)) / kTwo32;
                Cd gain;
                if (std::abs(dw) < 1e-12) {
                    gain = Cd{N, 0.0};
                } else {
                    gain = (1.0 - std::polar(1.0, kTwoPi * dw * N)) /
                           (1.0 - std::polar(1.0, kTwoPi * dw));
                }
                gain *= std::polar(1.0, kTwoPi * dphi);
                cleaned_sum[static_cast<std::size_t>(i)] -=
                    block_sum[static_cast<std::size_t>(j)] / N * gain;
            }
        }
    }

    std::uint32_t strong = 0;
    for (std::size_t i = 0; i < record.lines.size(); ++i) {
        const PlannedLine& planned = record.lines[i];
        if (planned.slot >= lines_.size() || lines_[planned.slot].k != planned.k ||
            lines_[planned.slot].m != planned.m) {
            continue;  // the slot was given to another line since
        }
        Line& line = lines_[planned.slot];
        const auto proj = projections.subspan(i * S, S);

        Cd D{};
        for (std::uint32_t s = 0; s + 1 < S; ++s) {
            const Cd p{proj[s].real(), proj[s].imag()};
            const Cd q{proj[s + 1].real(), proj[s + 1].imag()};
            D += q * std::conj(p);
        }
        const Cd P = cleaned_sum[i];
        const Cd z = std::abs(D) > 0.0 ? D / std::abs(D) : Cd{1.0, 0.0};
        Cd c{};
        Cd zs{1.0, 0.0};
        for (std::uint32_t s = 0; s < S; ++s) {
            c += Cd{proj[s].real(), proj[s].imag()} * std::conj(zs);
            zs *= z;
        }
        c /= static_cast<double>(S);
        double noise_sub = 0.0;
        if (S >= 3) {
            zs = Cd{1.0, 0.0};
            for (std::uint32_t s = 0; s < S; ++s) {
                noise_sub += std::norm(Cd{proj[s].real(), proj[s].imag()} - c * zs);
                zs *= z;
            }
            noise_sub /= static_cast<double>(S - 2);
        }
        const double coherent = std::norm(c) / (L * L);
        const double noise = noise_sub / L;
        if (line.fresh) {
            line.coherent = coherent;
            line.noise = noise;
            line.fresh = false;
        } else {
            line.coherent += kStatWeight * (coherent - line.coherent);
            line.noise += kStatWeight * (noise - line.noise);
        }

        const double plan_phase =
            static_cast<double>(static_cast<std::uint32_t>(planned.phase0 + planned.step * half)) /
            kTwo32;
        const double plan_freq =
            static_cast<double>(static_cast<std::int32_t>(planned.step)) / kTwo32;
        const std::uint64_t model_phase_fixed =
            cb.phase + times_k(db.phase, planned.k) + times_k2(eb.phase, planned.k) +
            times_k(rb.phase, planned.m);
        const double model_phase = static_cast<double>(model_phase_fixed) / kTwo64;
        const double model_freq = signed_of(cb.freq + times_k(db.freq, planned.k) +
                                            times_k2(eb.freq, planned.k) +
                                            times_k(rb.freq, planned.m));
        const double df_plan = S >= 2 ? std::arg(D) / (kTwoPi * L) : 0.0;

        Measured m;
        m.slot = planned.slot;
        m.k = static_cast<double>(planned.k);
        m.m = planned.m;
        const double rotation = wrap_cycles(plan_phase - model_phase);
        m.a_meas = P / N * std::polar(1.0, kTwoPi * rotation);
        m.freq_innovation = plan_freq + df_plan - model_freq;
        m.snr = std::min(line_snr(line, record.count), kSnrCap);
        // The carriers alone steer c and d and decide the lock; a sideband
        // carries the ripple's error on top of the carrier's, and steers r.
        m.fit = planned.in_band && m.snr >= kFitSnr && planned.m == 0;
        if (planned.m != 0) {
            // Weighted by its carrier: its own SNR is not measurable (see
            // stands_out), and a ripple sideband scales with its carrier.
            const std::size_t carrier =
                planned.slot - static_cast<std::size_t>(static_cast<std::ptrdiff_t>(planned.m));
            const double carrier_snr = carrier < lines_.size() && lines_[carrier].m == 0
                                           ? std::min(line_snr(lines_[carrier], record.count), kSnrCap)
                                           : 0.0;
            m.side_fit = planned.in_band && carrier_snr >= kFitSnr;
            m.snr = carrier_snr;
        }
        if (planned.m == 0) {
            if (planned.in_band && m.snr >= kStandOut) {
                ++strong;
                line.weak_blocks = 0;
            } else {
                ++line.weak_blocks;
            }
        }

        if (record.subtract && (planned.amp.real() != 0.0F || planned.amp.imag() != 0.0F)) {
            const Cd amp{planned.amp.real(), planned.amp.imag()};
            const double before = std::norm(P / N);
            const double after = std::norm(P / N - amp);
            line.before += kStatWeight * (before - line.before);
            line.after += kStatWeight * (after - line.after);
        }
        measured.push_back(m);
    }
    ++observed_;

    // Frequency fit, every stage.
    std::vector<double> ks;
    std::vector<double> ys;
    std::vector<double> ws;
    for (const Measured& m : measured) {
        if (m.fit) {
            ks.push_back(m.k);
            ys.push_back(m.freq_innovation);
            ws.push_back(m.snr);
        }
    }
    choose_model(ks, ys, ws, T);
    ks.clear();
    ys.clear();
    ws.clear();
    for (const Measured& m : measured) {
        if (m.fit && !lines_[m.slot].benched) {
            ks.push_back(m.k);
            ys.push_back(m.freq_innovation);
            ws.push_back(m.snr);
        }
    }
    const Fit ffit = fit_robust(ks, ys, ws, 0.01 / T, curved_);
    const bool enough = ffit.used >= min_lines_;
    const double freq_error_per_block =
        (std::abs(ffit.centroid_value) + ffit.half_span_value) * T;

    if (stage_ == Stage::kFrequency) {
        if (enough) {
            const double ec = ffit.at_zero;
            const double ed = ffit.slope;
            const double ee = ffit.curve;
            correct(record.mid, {0.0, kFllGain * ec, kFllRateGain * ec / T},
                    {0.0, kFllGain * ed, kFllRateGain * ed / T},
                    {0.0, kFllGain * ee, kFllRateGain * ee / T});
            if (wide_ && freq_error_per_block < kNarrowCyclesPerBlock) {
                wide_ = false;
            }
            if (freq_error_per_block < kFllExitCyclesPerBlock) {
                ++settle_blocks_;
            } else {
                settle_blocks_ = 0;
            }
            if (settle_blocks_ >= kFllExitBlocks) {
                stage_ = Stage::kPhase;
                seed_pending_ = true;
                settle_blocks_ = 0;
            }
        }
    } else if (seed_pending_) {
        for (const Measured& m : measured) {
            Line& line = lines_[m.slot];
            line.amp = m.a_meas;
            line.block_power = std::norm(m.a_meas);
            line.seeded = true;
        }
        seed_pending_ = false;
    } else {
        ks.clear();
        ys.clear();
        ws.clear();
        for (const Measured& m : measured) {
            const Line& line = lines_[m.slot];
            if (m.fit && !line.benched && line.seeded && std::abs(line.amp) > 0.0) {
                ks.push_back(m.k);
                ys.push_back(std::arg(m.a_meas * std::conj(line.amp)) / kTwoPi);
                ws.push_back(m.snr);
            }
        }
        const Fit pfit = fit_robust(ks, ys, ws, 0.02, curved_);

        // Each carrier judged against the comb's fit, benched ones included,
        // so a line that rejoins the comb comes off the bench. A carrier the
        // fit cannot judge this block (too weak, or the fit too thin) keeps
        // its standing.
        std::uint32_t candidates = 0;
        std::uint32_t benched = 0;
        if (pfit.used >= min_lines_) {
            for (const Measured& m : measured) {
                Line& line = lines_[m.slot];
                if (!m.fit || !line.seeded || std::abs(line.amp) <= 0.0) {
                    continue;
                }
                ++candidates;
                const double r = wrap_cycles(std::arg(m.a_meas * std::conj(line.amp)) / kTwoPi -
                                             eval(pfit, m.k));
                if (std::abs(r) * kTwoPi > kLineOutlierRad) {
                    ++line.outlier_blocks;
                    line.inlier_blocks = 0;
                    if (line.outlier_blocks >= kBenchBlocks) {
                        line.benched = true;
                    }
                } else {
                    ++line.inlier_blocks;
                    line.outlier_blocks = 0;
                    if (line.inlier_blocks >= kUnbenchBlocks) {
                        line.benched = false;
                    }
                }
                benched += line.benched ? 1U : 0U;
            }
        }
        const double rho = std::exp(-T / (kLoopSeconds * rate));
        const double alpha = 1.0 - rho * rho * rho;
        const double beta = 1.5 * (1.0 - rho) * (1.0 - rho) * (1.0 + rho);
        const double gamma = (1.0 - rho) * (1.0 - rho) * (1.0 - rho);
        if (pfit.used >= min_lines_) {
            auto loop = [&](double x) {
                return Correction{alpha * x, beta * x / T, gamma * x / (T * T)};
            };
            correct(record.mid, loop(pfit.at_zero), loop(pfit.slope), loop(pfit.curve));
        }
        // The ripple: each sideband's phase error past what the carriers'
        // fit gives its k is m times the ripple's, so the weighted least
        // squares slope through the origin in m. Only the block projection
        // is used, never the sub-blocks: a carrier a hundred hertz from its
        // sideband leaks into a short sub-block at nearly its own level, and
        // across a whole block about 22 dB down.
        double ripple_error = 0.0;
        if (orders_ > 0 && pfit.used >= min_lines_) {
            double swy = 0.0;
            double sww = 0.0;
            std::uint32_t used = 0;
            for (const Measured& m : measured) {
                const Line& line = lines_[m.slot];
                if (!m.side_fit || !line.seeded || std::abs(line.amp) <= 0.0) {
                    continue;
                }
                const double y = wrap_cycles(std::arg(m.a_meas * std::conj(line.amp)) / kTwoPi -
                                             eval(pfit, m.k));
                const double mm = static_cast<double>(m.m);
                swy += m.snr * y * mm;
                sww += m.snr * mm * mm;
                ++used;
            }
            if (used >= min_lines_ && sww > 0.0) {
                ripple_error = swy / sww;
                correct_ripple(record.mid, alpha * ripple_error, beta * ripple_error / T,
                               gamma * ripple_error / (T * T));
            }
        }
        // Amplitudes learn what the comb's fit does not explain.
        for (const Measured& m : measured) {
            Line& line = lines_[m.slot];
            const double fitted =
                (pfit.used >= min_lines_ ? eval(pfit, m.k) : 0.0) +
                static_cast<double>(m.m) * ripple_error;
            const Cd target = m.a_meas * std::polar(1.0, -kTwoPi * fitted);
            if (!line.seeded) {
                line.amp = target;
                line.block_power = std::norm(target);
                line.seeded = true;
            } else {
                line.amp += kAmpWeight * (target - line.amp);
                line.block_power += kAmpWeight * (std::norm(target) - line.block_power);
            }
        }

        const double residual_rad = pfit.rms * kTwoPi;
        const bool fits = pfit.used >= min_lines_;
        const double innovation_cycles = std::abs(pfit.centroid_value) + pfit.half_span_value;
        // The pass is judged by the carriers moving together, not by the
        // worst of them: benched lines are out of the residual, and the pass
        // only fails when too few remain or most have left the fit.
        const bool cohesive =
            candidates == 0 ||
            static_cast<double>(candidates - benched) >= kHealthyShare * candidates;
        const bool good = fits && cohesive && residual_rad < kLockRad && strong >= min_lines_ &&
                          innovation_cycles < kLockInnovationCycles &&
                          freq_error_per_block < kPhaseFallbackCyclesPerBlock;
        const bool bad = !fits || !cohesive || residual_rad > kLostRad || strong < min_lines_ ||
                         freq_error_per_block > kPhaseFallbackCyclesPerBlock;
        verdict_ = CombVerdict{pfit.used, strong,  benched, residual_rad, innovation_cycles,
                               freq_error_per_block, curved_, good,    bad};
        if (lock_ == CombLock::kAcquiring || lock_ == CombLock::kRecovering) {
            settle_blocks_ = good ? settle_blocks_ + 1 : 0;
            if (settle_blocks_ >= kLockBlocks) {
                lock_ = CombLock::kLocked;
                bad_blocks_ = 0;
            } else if (enough && freq_error_per_block > kPhaseFallbackCyclesPerBlock) {
                // Recovering starts from a model that was right moments ago,
                // so it keeps the long sub-blocks; acquisition needs the
                // wide pull-in.
                stage_ = Stage::kFrequency;
                wide_ = lock_ == CombLock::kAcquiring;
                settle_blocks_ = 0;
                for (Line& line : lines_) {
                    line.seeded = false;
                }
            }
        } else {
            bad_blocks_ = bad ? bad_blocks_ + 1 : 0;
            if (bad_blocks_ >= kLostBlocks) {
                begin_recovery(record.mid);
            }
        }
    }

    if (lock_ == CombLock::kAcquiring && observed_ > kAcquireLimit) {
        lock_ = CombLock::kLost;
        records_.clear();
        return;
    }
    if (lock_ == CombLock::kRecovering &&
        static_cast<double>(static_cast<std::int64_t>(record.mid - recovering_since_)) >
            kRecoverSeconds * rate) {
        lock_ = CombLock::kLost;
        records_.clear();
        return;
    }
    if (lock_ == CombLock::kLocked) {
        replace_weak_lines();
    }
}

CombStatus CombTracker::status() const
{
    CombStatus out;
    out.lock = lock_;
    if (lock_ == CombLock::kIdle) {
        return out;
    }
    const double rate = static_cast<double>(rate_);
    out.comb.offset_hz = anchored_ ? signed_of(c_.freq) * rate : initial_offset_hz_;
    out.comb.spacing_hz = anchored_ ? signed_of(d_.freq) * rate : initial_spacing_hz_;
    out.comb.curvature_hz = anchored_ ? signed_of(e_.freq) * rate : initial_curvature_hz_;
    if (!lines_.empty()) {
        std::int32_t lo = lines_.front().k;
        std::int32_t hi = lines_.front().k;
        for (const Line& line : lines_) {
            lo = std::min(lo, line.k);
            hi = std::max(hi, line.k);
        }
        out.comb.k_lo = lo;
        out.comb.k_hi = hi;
    }
    std::vector<double> heights;
    double suppression = 0.0;
    std::uint32_t cancelled = 0;
    std::uint32_t carriers = 0;
    const double n = static_cast<double>(std::max<std::uint32_t>(last_count_, 1));
    for (std::size_t slot = 0; slot < lines_.size(); ++slot) {
        const Line& line = lines_[slot];
        if (line.m != 0) {
            // Counted, but kept out of the heights and the suppression
            // figure, whose noise estimates a nearby carrier spoils.
            const std::size_t carrier =
                slot - static_cast<std::size_t>(static_cast<std::ptrdiff_t>(line.m));
            const bool benched = carrier < lines_.size() && lines_[carrier].benched;
            if (lock_ == CombLock::kLocked && line.seeded && line.in_band && !benched &&
                stands_out(slot, last_count_)) {
                ++cancelled;
            }
            continue;
        }
        const double snr = line_snr(line, last_count_);
        if (!line.in_band || snr < kStandOut) {
            continue;
        }
        heights.push_back(10.0 * std::log10(snr));
        if (lock_ == CombLock::kLocked && line.seeded && !line.benched) {
            ++cancelled;
            ++carriers;
            const double floor = line.noise / n;
            const double before = std::max(line.before - floor, 0.0);
            // The average of |residual|^2 over blocks resolves the residual
            // only down to about half the noise's share in it: with weight
            // 1/8 the average's own relative scatter is 0.25, so two of those
            // is where it stops meaning anything. Below that the figure is a
            // floor, so the report is a lower bound set by each line's SNR.
            const double after = std::max(line.after - floor, 0.5 * floor);
            if (before > 0.0) {
                suppression += std::clamp(10.0 * std::log10(before / after), 0.0, 60.0);
            }
        }
    }
    out.comb.lines_found = static_cast<std::uint32_t>(heights.size());
    if (!heights.empty()) {
        std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
        out.comb.median_height_db = static_cast<float>(heights[heights.size() / 2]);
    }
    out.lines_cancelled = cancelled;
    out.suppression_db = carriers > 0 ? static_cast<float>(suppression / carriers) : 0.0F;
    out.spacing_drift_hz_per_s =
        anchored_ ? static_cast<float>(signed_of(d_.rate) * rate * rate) : 0.0F;
    return out;
}

}  // namespace revenant::dsp
