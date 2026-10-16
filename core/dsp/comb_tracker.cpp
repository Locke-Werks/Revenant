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
// LOCK.
//   Acquiring -> Locked   eight consecutive phase-stage blocks with the
//                         weighted RMS phase residual under 0.25 rad (12 dB of
//                         cancellation at worst from phase alone), the fitted
//                         phase innovation under 0.03 cycles across the lines
//                         (the loop has pulled in, not merely fits) and at
//                         least kCombMinLines lines standing out. About 37
//                         blocks from a detection 90 Hz off, 1 s.
//   Locked -> Lost        six consecutive blocks with that residual over
//                         0.6 rad, or too few lines standing out, or the
//                         frequency innovation over 0.25 cycles per block.
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

struct Fit {
    double at_zero = 0.0;  // e_c
    double slope = 0.0;    // e_d
    double centroid_value = 0.0;
    double half_span_value = 0.0;  // |slope| * half the weighted k spread
    double rms = 0.0;              // weighted RMS residual
    std::uint32_t used = 0;
};

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
    *this = CombTracker(rate, max_lines_);
    generation_ = generation;
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

bool CombTracker::agrees(const CombEstimate& estimate) const
{
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

double CombTracker::overlap(const CombEstimate& estimate) const
{
    if (!active() || lines_.empty() || estimate.line_count() == 0) {
        return 0.0;
    }
    const double rate = static_cast<double>(rate_);
    const double c = anchored_ ? signed_of(c_.freq) * rate : initial_offset_hz_;
    const double d = anchored_ ? signed_of(d_.freq) * rate : initial_spacing_hz_;
    if (!(d > 0.0)) {
        return 0.0;
    }
    std::int32_t lo = lines_.front().k;
    std::int32_t hi = lines_.front().k;
    for (const Line& line : lines_) {
        lo = std::min(lo, line.k);
        hi = std::max(hi, line.k);
    }
    std::uint32_t on = 0;
    for (std::int32_t k = estimate.k_lo; k <= estimate.k_hi; ++k) {
        const double f = estimate.offset_hz + k * estimate.spacing_hz;
        const double j = std::round((f - c) / d);
        if (j >= lo && j <= hi && std::abs(f - (c + j * d)) < kAgreeHz) {
            ++on;
        }
    }
    return static_cast<double>(on) / static_cast<double>(estimate.line_count());
}

void CombTracker::start_acquisition(const CombEstimate& estimate)
{
    const SampleRate rate = rate_;
    const std::uint64_t generation = generation_ + 1;
    *this = CombTracker(rate, max_lines_);
    generation_ = generation;

    lock_ = CombLock::kAcquiring;
    stage_ = Stage::kFrequency;
    wide_ = true;
    initial_offset_hz_ = estimate.offset_hz;
    initial_spacing_hz_ = estimate.spacing_hz;
    range_lo_ = estimate.k_lo;
    range_hi_ = estimate.k_hi;

    // The central max_lines_ of the range.
    const std::int64_t total = static_cast<std::int64_t>(estimate.line_count());
    const std::int64_t take = std::min<std::int64_t>(total, max_lines_);
    const std::int64_t first = estimate.k_lo + (total - take) / 2;
    lines_.reserve(static_cast<std::size_t>(take));
    for (std::int64_t i = 0; i < take; ++i) {
        Line line;
        line.k = static_cast<std::int32_t>(first + i);
        lines_.push_back(line);
    }
    next_lo_ = static_cast<std::int32_t>(first - 1);
    next_hi_ = static_cast<std::int32_t>(first + take);
}

void CombTracker::detected(const CombEstimate& estimate)
{
    if (rate_ <= 0 || estimate.line_count() < kCombMinLines || !(estimate.spacing_hz > 0.0)) {
        return;
    }
    if ((lock_ == CombLock::kLocked || lock_ == CombLock::kAcquiring) && agrees(estimate)) {
        return;
    }
    start_acquisition(estimate);
}

void CombTracker::advance_to(SampleIndex index)
{
    const auto n = static_cast<std::int64_t>(index - anchor_);
    const Axes c = advanced(c_, n);
    const Axes d = advanced(d_, n);
    c_ = Axis{c.phase, c.freq, c.rate};
    d_ = Axis{d.phase, d.freq, d.rate};
    anchor_ = index;
}

// Corrections are errors at index `at`, applied to the model as a change of
// its whole trajectory and carried forward to the anchor.
void CombTracker::correct(SampleIndex at, double dc_phase, double dc_freq, double dc_rate,
                          double dd_phase, double dd_freq, double dd_rate)
{
    const double dt = static_cast<double>(static_cast<std::int64_t>(anchor_ - at));
    auto apply = [dt](Axis& axis, double dp, double df, double dr) {
        axis.phase += to_fixed(dp + df * dt + 0.5 * dr * dt * dt);
        axis.freq += to_fixed(df + dr * dt);
        axis.rate += to_fixed_small(dr);
    };
    apply(c_, dc_phase, dc_freq, dc_rate);
    apply(d_, dd_phase, dd_freq, dd_rate);
}

CombBlockPlan CombTracker::plan(SampleIndex start, std::uint32_t count)
{
    CombBlockPlan out;
    out.start = start;
    out.count = count;
    out.sub_blocks = 1;
    if (rate_ <= 0 || count == 0 || lines_.empty() ||
        (lock_ != CombLock::kAcquiring && lock_ != CombLock::kLocked)) {
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
    } else {
        const auto gap = static_cast<std::int64_t>(mid - anchor_);
        if (gap > static_cast<std::int64_t>(kGapSeconds * rate) || gap < 0) {
            // A long gap or a jump back: the rate terms cannot be trusted across
            // it, so hold frequency and start the loops again.
            advance_to(mid);
            c_.rate = 0;
            d_.rate = 0;
            lock_ = CombLock::kAcquiring;
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
        const std::uint64_t phase = c_.phase + times_k(d_.phase, line.k);
        const std::uint64_t freq = c_.freq + times_k(d_.freq, line.k);
        const auto step = static_cast<std::uint32_t>((freq + (1ULL << 31)) >> 32);
        const auto phase_mid = static_cast<std::uint32_t>((phase + (1ULL << 31)) >> 32);
        const std::uint32_t phase0 = phase_mid - step * half;

        line.in_band = std::abs(signed_of(freq)) < kBandEdge;

        Complex32 amp{};
        if (out.subtract && line.seeded && line.in_band &&
            line_snr(line, count) >= kStandOut) {
            amp = Complex32{static_cast<float>(line.amp.real()),
                            static_cast<float>(line.amp.imag())};
        }
        CombLine planned;
        planned.phase0 = phase0;
        planned.step = step;
        planned.amplitude = amp;
        out.lines.push_back(planned);

        record.lines.push_back(PlannedLine{slot, line.k, phase0, step, amp, line.in_band});
    }

    records_.push_back(std::move(record));
    if (records_.size() > kMaxRecords) {
        records_.erase(records_.begin());
    }
    return out;
}

void CombTracker::replace_weak_lines()
{
    for (Line& line : lines_) {
        if (line.weak_blocks < kSwapBlocks) {
            continue;
        }
        const bool hi_left = next_hi_ <= range_hi_;
        const bool lo_left = next_lo_ >= range_lo_;
        if (!hi_left && !lo_left) {
            return;
        }
        const bool take_hi = hi_left && (next_take_hi_ || !lo_left);
        Line fresh;
        if (take_hi) {
            fresh.k = next_hi_++;
        } else {
            fresh.k = next_lo_--;
        }
        next_take_hi_ = !take_hi;
        line = fresh;
    }
}

void CombTracker::observe(const CombObservation& observation)
{
    if (lock_ != CombLock::kAcquiring && lock_ != CombLock::kLocked) {
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

    struct Measured {
        std::uint32_t slot = 0;
        double k = 0.0;
        Cd a_meas{};
        double freq_innovation = 0.0;  // cycles per sample
        double snr = 0.0;
        bool fit = false;
    };
    std::vector<Measured> measured;
    measured.reserve(record.lines.size());

    std::uint32_t strong = 0;
    for (std::size_t i = 0; i < record.lines.size(); ++i) {
        const PlannedLine& planned = record.lines[i];
        if (planned.slot >= lines_.size() || lines_[planned.slot].k != planned.k) {
            continue;  // the slot was given to another line since
        }
        Line& line = lines_[planned.slot];
        const auto proj = projections.subspan(i * S, S);

        Cd D{};
        Cd P{};
        for (std::uint32_t s = 0; s < S; ++s) {
            const Cd p{proj[s].real(), proj[s].imag()};
            P += p;
            if (s + 1 < S) {
                const Cd q{proj[s + 1].real(), proj[s + 1].imag()};
                D += q * std::conj(p);
            }
        }
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
        const std::uint64_t model_phase_fixed = cb.phase + times_k(db.phase, planned.k);
        const double model_phase = static_cast<double>(model_phase_fixed) / kTwo64;
        const double model_freq = signed_of(cb.freq + times_k(db.freq, planned.k));
        const double df_plan = S >= 2 ? std::arg(D) / (kTwoPi * L) : 0.0;

        Measured m;
        m.slot = planned.slot;
        m.k = static_cast<double>(planned.k);
        const double rotation = wrap_cycles(plan_phase - model_phase);
        m.a_meas = P / N * std::polar(1.0, kTwoPi * rotation);
        m.freq_innovation = plan_freq + df_plan - model_freq;
        m.snr = std::min(line_snr(line, record.count), kSnrCap);
        m.fit = planned.in_band && m.snr >= kFitSnr;
        if (planned.in_band && m.snr >= kStandOut) {
            ++strong;
            line.weak_blocks = 0;
        } else {
            ++line.weak_blocks;
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
    const Fit ffit = fit_affine(ks, ys, ws);
    const bool enough = ffit.used >= kCombMinLines;
    const double freq_error_per_block =
        (std::abs(ffit.centroid_value) + ffit.half_span_value) * T;

    if (stage_ == Stage::kFrequency) {
        if (enough) {
            const double ec = ffit.at_zero;
            const double ed = ffit.slope;
            correct(record.mid, 0.0, kFllGain * ec, kFllRateGain * ec / T, 0.0, kFllGain * ed,
                    kFllRateGain * ed / T);
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
            line.seeded = true;
        }
        seed_pending_ = false;
    } else {
        ks.clear();
        ys.clear();
        ws.clear();
        for (const Measured& m : measured) {
            const Line& line = lines_[m.slot];
            if (m.fit && line.seeded && std::abs(line.amp) > 0.0) {
                ks.push_back(m.k);
                ys.push_back(std::arg(m.a_meas * std::conj(line.amp)) / kTwoPi);
                ws.push_back(m.snr);
            }
        }
        const Fit pfit = fit_affine(ks, ys, ws);
        const double rho = std::exp(-T / (kLoopSeconds * rate));
        const double alpha = 1.0 - rho * rho * rho;
        const double beta = 1.5 * (1.0 - rho) * (1.0 - rho) * (1.0 + rho);
        const double gamma = (1.0 - rho) * (1.0 - rho) * (1.0 - rho);
        if (pfit.used >= kCombMinLines) {
            const double ec = pfit.at_zero;
            const double ed = pfit.slope;
            correct(record.mid, alpha * ec, beta * ec / T, gamma * ec / (T * T), alpha * ed,
                    beta * ed / T, gamma * ed / (T * T));
        }
        // Amplitudes learn what the affine fit does not explain.
        for (const Measured& m : measured) {
            Line& line = lines_[m.slot];
            const double fitted = pfit.used >= kCombMinLines ? pfit.at_zero + pfit.slope * m.k : 0.0;
            const Cd target = m.a_meas * std::polar(1.0, -kTwoPi * fitted);
            if (!line.seeded) {
                line.amp = target;
                line.seeded = true;
            } else {
                line.amp += kAmpWeight * (target - line.amp);
            }
        }

        const double residual_rad = pfit.rms * kTwoPi;
        const bool fits = pfit.used >= kCombMinLines;
        const double innovation_cycles = std::abs(pfit.centroid_value) + pfit.half_span_value;
        const bool good = fits && residual_rad < kLockRad && strong >= kCombMinLines &&
                          innovation_cycles < kLockInnovationCycles &&
                          freq_error_per_block < kPhaseFallbackCyclesPerBlock;
        const bool bad = !fits || residual_rad > kLostRad || strong < kCombMinLines ||
                         freq_error_per_block > kPhaseFallbackCyclesPerBlock;
        if (lock_ == CombLock::kAcquiring) {
            settle_blocks_ = good ? settle_blocks_ + 1 : 0;
            if (settle_blocks_ >= kLockBlocks) {
                lock_ = CombLock::kLocked;
                bad_blocks_ = 0;
            } else if (enough && freq_error_per_block > kPhaseFallbackCyclesPerBlock) {
                stage_ = Stage::kFrequency;
                wide_ = true;
                settle_blocks_ = 0;
                for (Line& line : lines_) {
                    line.seeded = false;
                }
            }
        } else {
            bad_blocks_ = bad ? bad_blocks_ + 1 : 0;
            if (bad_blocks_ >= kLostBlocks) {
                lock_ = CombLock::kLost;
                records_.clear();
                return;
            }
        }
    }

    if (lock_ == CombLock::kAcquiring && observed_ > kAcquireLimit) {
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
    const double n = static_cast<double>(std::max<std::uint32_t>(last_count_, 1));
    for (const Line& line : lines_) {
        const double snr = line_snr(line, last_count_);
        if (!line.in_band || snr < kStandOut) {
            continue;
        }
        heights.push_back(10.0 * std::log10(snr));
        if (lock_ == CombLock::kLocked && line.seeded) {
            ++cancelled;
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
    out.suppression_db = cancelled > 0 ? static_cast<float>(suppression / cancelled) : 0.0F;
    out.spacing_drift_hz_per_s =
        anchored_ ? static_cast<float>(signed_of(d_.rate) * rate * rate) : 0.0F;
    return out;
}

}  // namespace revenant::dsp
