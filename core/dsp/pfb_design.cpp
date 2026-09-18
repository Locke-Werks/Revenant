// Host-side design of the two tables the channelizer kernels read: the
// polyphase prototype filter and the FFT twiddle circle.
//
// Neither table is ever computed in a shader. GLSL's sin and cos are permitted
// several units in the last place and each vendor spends them differently, so a
// shader-computed twiddle would end bit-exactness on its own, before any
// question of accumulation order arose. Both are built here in double and
// rounded to float once.
//
// Nothing in this file is a CPU twin. It produces the artefact that the kernel
// and the twin both read, so there is no operation order to match: the
// requirement is only that the same (M, L, A) produces the same float array on
// every machine that builds it. reference_fp.h is included first all the same,
// because that array is what the prototype-hash test pins, and a contracted
// multiply-add in the design loop is one of the ways it could move.
//
// Sources, per docs/clean-room.md. Every result here is textbook and every
// citation is to a document:
//
//   Oppenheim and Schafer, Discrete-Time Signal Processing, 3rd ed.,
//     section 7.5.3: the Kaiser window, the shape parameter formula
//     (eq. 7.62) and the filter order estimate (eq. 7.63).
//   Abramowitz and Stegun, Handbook of Mathematical Functions, eq. 9.6.12:
//     the ascending power series for the modified Bessel function I0.
//   Crochiere and Rabiner, Multirate Digital Signal Processing, chapter 3:
//     the polyphase decomposition h[qM + r] of a prototype lowpass.
//   harris, Multirate Signal Processing for Communication Systems, chapter 6:
//     the channelizer form and the oversampled variant this project uses.
//
// No GPL-licensed implementation was opened, fetched or consulted while writing
// it. In particular neither GNU Radio's filterbank sources nor FFTW.

// This include must come first. It carries the pragma that disables
// floating-point contraction for this translation unit, and a pragma that
// appears after a function definition does not apply to it.
#include "core/dsp/reference_fp.h"

#include "core/dsp/pfb.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <numeric>
#include <vector>

namespace revenant::dsp {
namespace {

static_assert(kReferenceFpDisciplineApplied,
              "core/dsp/reference_fp.h must be included before anything else here: the "
              "prototype coefficients are pinned by a hash test, and contraction is one of "
              "the things that can move them between host compilers");

constexpr double kPi = std::numbers::pi;

// The FFT kernel holds one transform in shared memory as M vec2, so it needs
// 8*M bytes. Vulkan 1.3 guarantees maxComputeSharedMemorySize of at least
// 16384 bytes (Required Limits table), which is 2048 channels. A specific
// device may allow more, but validate() is not handed a device, so it applies
// the guaranteed floor and the device-aware check belongs at pipeline creation
// where the real limit is known.
constexpr std::uint32_t kMaxChannels = 2048;

// A device-independent sanity bound on the uploaded prototype, not a hardware
// limit. One million taps is 4 MiB of float and four hundred times the length
// any sensible grid asks for; a request past it is an arithmetic mistake in the
// caller rather than a filter somebody wants.
constexpr std::uint32_t kMaxPrototypeTaps = 1u << 20;

// Far enough past any target this project has a use for, and short of where
// the Kaiser window's edge taps would underflow float and the design would
// quietly stop being the filter that was asked for. At 300 dB the shape
// parameter is 32.1 and the smallest window value is 1.6e-13, twenty-five
// orders of magnitude clear of the smallest normal float. Because the design
// attenuation below is never more than this, it bounds the shape parameter for
// every grid.
constexpr double kMaxAttenuationDb = 300.0;

// Modified Bessel function of the first kind, order zero, from the ascending
// power series I0(x) = sum_k (x^2/4)^k / (k!)^2 (Abramowitz and Stegun 9.6.12).
//
// Written out because MSVC does not ship the C++17 special mathematical
// functions: std::cyl_bessel_i does not exist on this toolchain, and the design
// needs I0 in exactly two places.
//
// Convergence criterion: successive terms are in the ratio (x^2/4)/k^2, so once
// k passes x/2 the terms fall superexponentially and the tail is bounded by a
// geometric series with that ratio. Stopping when a term is below 1e-18 of the
// running sum puts the whole remaining tail below the 2.2e-16 relative spacing
// of a double, so every term left rounds away and the sum is already final. The
// iteration cap is a guard against a pathological argument rather than a real
// limit: at the largest shape parameter this file can produce, 32.1, the
// criterion is met after about eighty terms.
[[nodiscard]] double bessel_i0(double x) {
    const double quarter_square = 0.25 * x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 256; ++k) {
        term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-18 * sum) {
            break;
        }
    }
    return sum;
}

// Kaiser's empirical shape parameter for a given stopband attenuation, in
// positive decibels. Oppenheim and Schafer eq. 7.62.
[[nodiscard]] double kaiser_beta(double attenuation_db) {
    if (attenuation_db > 50.0) {
        return 0.1102 * (attenuation_db - 8.7);
    }
    if (attenuation_db >= 21.0) {
        const double excess = attenuation_db - 21.0;
        return 0.5842 * std::pow(excess, 0.4) + 0.07886 * excess;
    }
    // Below 21 dB the Kaiser window degenerates to the rectangular window,
    // whose own sidelobes are already at -21 dB.
    return 0.0;
}

// The attenuation a prototype of this many taps per branch can actually reach,
// which is Kaiser's order estimate read backwards.
//
// Forwards, Oppenheim and Schafer eq. 7.63 gives the filter order for a target
// A and a transition width dw in radians per sample as N = (A - 8)/(2.285*dw).
// This channelizer fixes the transition at half a channel spacing: passband
// edge 0.25/M, stopband edge 0.75/M, so dw = 2*pi*(0.75 - 0.25)/M = pi/M and
// N = (A - 8)*M/(2.285*pi). Dividing by M, the taps per branch come out
// independent of M entirely: L = ceil((A - 8)/(2.285*pi)), which is 16 at the
// project's 120 dB target and is where GridParams::taps_per_branch = 17 comes
// from once the delay pad is added.
//
// Read backwards with N = M*L it gives A = 2.285*pi*L + 8, which is what this
// returns.
//
// The estimate is empirical and it is optimistic. Measured against the filters
// this file produces, across L = 8 to 15 where the design attenuation is the
// estimate's own value, the achieved peak stopband came out between 0.4 and
// 2.2 dB short of it. Kaiser's shape parameter formula carries about another
// 1 dB of the same: at L = 16 designed to 120 dB the measured peak is
// -119.0 dB. Close enough to choose a length, nowhere near close enough for a
// test to assert against, which is what the measured
// PrototypeFilter::stopband_db is for.
[[nodiscard]] double attenuation_reachable_at(std::uint32_t taps_per_branch) {
    const auto branch_order = static_cast<double>(taps_per_branch - 1u);
    return 2.285 * kPi * branch_order + 8.0;
}

// An ordinary iterative radix-2 Cooley-Tukey DFT in double, decimation in time,
// in place.
//
// It is used only to measure the filter that was just designed. It is not the
// channelizer's FFT, it is not a twin of anything, and nothing compares its
// output bit for bit, so it is free to be the shortest correct thing rather
// than a transcription of a kernel. Direct evaluation of the response would be
// O(N^2), which at a 2048-channel grid is tens of seconds for a number that is
// wanted once per grid change.
void dft_in_place(std::vector<std::complex<double>>& data) {
    const std::size_t count = data.size();
    if (count < 2) {
        return;
    }

    // Bit-reversal permutation, counting in reversed order rather than
    // reversing each index.
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        std::size_t bit = count >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    // exp(-j*2*pi*k/count) for the whole circle, so the inner loop costs no
    // transcendentals and no recurrence accumulates error across stages.
    std::vector<std::complex<double>> twiddles(count / 2);
    for (std::size_t k = 0; k < twiddles.size(); ++k) {
        const double angle = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(count);
        twiddles[k] = std::complex<double>(std::cos(angle), std::sin(angle));
    }

    for (std::size_t span = 2; span <= count; span <<= 1) {
        const std::size_t half = span / 2;
        const std::size_t stride = count / span;
        for (std::size_t base = 0; base < count; base += span) {
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<double> upper = data[base + k + half] * twiddles[k * stride];
                const std::complex<double> lower = data[base + k];
                data[base + k] = lower + upper;
                data[base + k + half] = lower - upper;
            }
        }
    }
}

// Peak response anywhere in the stopband, in decibels relative to DC, negative.
//
// Measured on the float taps that are actually uploaded rather than on the
// double working array, because the float array is the filter that runs.
//
// The response is sampled by zero-padding to sixteen times the next power of
// two above the tap count and transforming. Sixteen points per sidelobe is the
// worst case and puts the worst sample 1/32 of a lobe from a peak, which
// understates that peak by 0.04 dB. Four points per lobe would understate it
// by 0.7 dB, the same order as the thing being measured. Checked against a
// direct evaluation of the response on a 32-point-per-lobe grid at M = 8, 16,
// 64 and 256: the two agree to 0.008 dB.
//
// The band starts at the declared stopband edge, 0.75/M, and that choice is
// worth a note because it moves the answer. At M = 64, L = 16 the peak over
// [0.75/M, 0.5] is -119.0 dB and sits at 0.752/M, on the shoulder where the
// transition is still settling. The first true sidelobe, at 0.85/M, is
// -121.1 dB. Measuring from the edge the filter was specified against is the
// conservative reading and is the one reported.
[[nodiscard]] double measure_stopband_db(const std::vector<float>& taps, std::uint32_t channels) {
    if (taps.empty() || channels == 0) {
        return 0.0;
    }

    // Sixteen times oversampled, capped so that an absurdly long prototype
    // costs bounded memory. The cap only bites above about 130,000 taps, where
    // the oversampling falls to eight and the understatement to 0.17 dB.
    constexpr std::size_t kMaxPoints = std::size_t{1} << 21;
    std::size_t points = std::bit_ceil(taps.size());
    for (int doubling = 0; doubling < 4 && points < kMaxPoints; ++doubling) {
        points <<= 1;
    }

    std::vector<std::complex<double>> spectrum(points, std::complex<double>(0.0, 0.0));
    for (std::size_t n = 0; n < taps.size(); ++n) {
        spectrum[n] = std::complex<double>(static_cast<double>(taps[n]), 0.0);
    }
    dft_in_place(spectrum);

    const double dc = std::abs(spectrum[0]);
    if (!(dc > 0.0)) {
        return 0.0;
    }

    // Stopband edge at 0.75/M cycles per sample, up to Nyquist. Bin i sits at
    // i/points, so the first bin at or above the edge is the ceiling.
    const double stop_edge = 0.75 / static_cast<double>(channels);
    const auto first = static_cast<std::size_t>(std::ceil(stop_edge * static_cast<double>(points)));
    const std::size_t last = points / 2;
    if (first > last) {
        return 0.0;
    }

    double peak = 0.0;
    for (std::size_t i = first; i <= last; ++i) {
        peak = std::max(peak, std::abs(spectrum[i]));
    }

    // Unreachable for any filter this file produces, since a windowed sinc has
    // sidelobes. The floor keeps a degenerate input from returning -inf into a
    // struct a test will format.
    constexpr double kFloorDb = -600.0;
    if (!(peak > 0.0)) {
        return kFloorDb;
    }
    return std::max(kFloorDb, 20.0 * std::log10(peak / dc));
}

// Rounds to float and normalises a negative zero to positive. A twiddle table
// whose W^0 imaginary part is -0.0 compares equal to +0.0 in arithmetic but
// not in bits, and this table is uploaded to a device and diffed bit for bit.
[[nodiscard]] float to_float_unsigned_zero(double value) {
    const auto rounded = static_cast<float>(value);
    return rounded == 0.0f ? 0.0f : rounded;
}

}  // namespace

Status validate(const GridParams& grid) {
    // M = 1 would leave the FFT with no stages and the channelizer with nothing
    // to do. It is excluded here rather than left to fail somewhere further in.
    if (grid.channels < 2) {
        return fail(
            std::format("channel count {} is below the minimum of 2; one channel "
                        "leaves the FFT with no stages and nothing to channelize",
                        grid.channels));
    }
    if (!std::has_single_bit(grid.channels)) {
        return fail(
            std::format("channel count {} is not a power of two, which the "
                        "radix-2 FFT and the branch-reversal mask both require",
                        grid.channels));
    }
    if (grid.channels > kMaxChannels) {
        return fail(
            std::format("channel count {} exceeds {}, the most that fits in the "
                        "16384 bytes of shared memory Vulkan 1.3 guarantees at "
                        "8 bytes per channel",
                        grid.channels, kMaxChannels));
    }
    if (grid.taps_per_branch == 0) {
        return fail("grid has zero taps per branch, so there is no prototype to design");
    }
    if (grid.decimation == 0) {
        return fail("grid has zero decimation, which would produce no output samples");
    }
    if (grid.channels % grid.decimation != 0) {
        return fail(
            std::format("decimation {} does not divide {} channels; the output "
                        "phase correction exp(-j*2*pi*k*m*D/M) is only periodic "
                        "in m when it does",
                        grid.decimation, grid.channels));
    }

    const auto length = static_cast<std::uint64_t>(grid.channels) *
                        static_cast<std::uint64_t>(grid.taps_per_branch);
    if (length > kMaxPrototypeTaps) {
        return fail(
            std::format("prototype of {} taps ({} channels x {} per branch) "
                        "exceeds the {} tap limit",
                        length, grid.channels, grid.taps_per_branch, kMaxPrototypeTaps));
    }

    return {};
}

ChannelCentre channel_centre(const GridParams& grid, SampleRate rate, std::uint32_t channel) {
    ChannelCentre centre;
    if (grid.channels == 0) {
        return centre;
    }

    const std::uint32_t channels = grid.channels;
    const std::uint32_t index = channel % channels;

    // Channel k is centred at k*rate/M taken modulo rate, so k below M/2 are
    // positive frequencies and k at or above M/2 are negative: channel M-1 is
    // -rate/M and channel M/2 is -rate/2. Doing the fold here in integers is
    // what keeps the descriptor exact.
    const auto signed_index = (index < channels / 2) ? static_cast<std::int64_t>(index)
                                                     : static_cast<std::int64_t>(index) -
                                                           static_cast<std::int64_t>(channels);

    centre.numerator = signed_index * static_cast<std::int64_t>(rate);
    centre.denominator = static_cast<std::int64_t>(channels);

    // Reduced, so that two descriptors for the same frequency compare equal and
    // is_integral() answers the question it claims to. gcd(0, M) is M, so a DC
    // channel comes out as 0/1 rather than 0/M.
    const std::int64_t divisor = std::gcd(centre.numerator, centre.denominator);
    if (divisor > 1) {
        centre.numerator /= divisor;
        centre.denominator /= divisor;
    }

    return centre;
}

Expected<PrototypeFilter> design_prototype(const GridParams& grid, double attenuation_db) {
    if (const Status grid_ok = validate(grid); !grid_ok) {
        return std::unexpected(with_context(grid_ok.error(), "design_prototype"));
    }
    if (!std::isfinite(attenuation_db) || attenuation_db < 0.0 ||
        attenuation_db > kMaxAttenuationDb) {
        return fail(
            std::format("design_prototype: stopband target {} dB is outside the "
                        "supported range [0, {}]",
                        attenuation_db, kMaxAttenuationDb));
    }

    const std::uint32_t channels = grid.channels;
    const std::uint32_t branch_order = grid.taps_per_branch - 1u;  // L

    // The design length is N = M*L + 1, not M*L. M*L is even, so a linear-phase
    // filter of that length has group delay (M*L - 1)/2, a half-integer, and at
    // D = M/2 the delay in channel samples is (M*L - 1)/M, which is neither
    // integer nor half-integer. That is where a sample-accurate timestamp
    // quietly stops being accurate. The odd length puts the group delay at
    // exactly M*L/2 input samples, which divides by any D that divides M.
    //
    // The array is then zero-padded to M*(L + 1) so every branch runs L + 1
    // taps uniformly and the kernel needs no ragged last iteration. Measured
    // cost of the extra tap at M = 64, L = 16: 0.075 dB of stopband
    // (-119.013 dB padded against -119.088 dB at length M*L). Only branch 0's
    // last tap is nonzero, because every padded entry is past the end of the
    // filter.
    const auto design_length = static_cast<std::size_t>(channels) * branch_order;  // M*L
    const std::size_t taps_used = design_length + 1;
    const std::size_t taps_total = static_cast<std::size_t>(grid.prototype_length());

    // A target the length cannot reach is met with the best filter that length
    // can hold, rather than with a window shaped for an attenuation it will
    // never see. Shaping for more than the length can carry widens the
    // transition past its allotment, so the response has not reached its floor
    // by the stopband edge and the stopband gets worse, not better. Measured at
    // M = 64, L = 12 asking for 120 dB: -55.6 dB unclamped against -93.3 dB
    // clamped to the 94.1 dB that length reaches. The clamp is the difference
    // between a filter and a mistake.
    //
    // It does not touch a grid whose taps_per_branch came from the order
    // estimate in the first place. At L = 16 the reachable figure is 122.9 dB,
    // above the project's 120 dB target, so the canonical design is shaped for
    // the full 120 dB it asked for.
    const double design_db =
        std::min(attenuation_db, attenuation_reachable_at(grid.taps_per_branch));
    const double beta = kaiser_beta(design_db);
    const double i0_beta = bessel_i0(beta);
    if (!(i0_beta > 0.0) || !std::isfinite(i0_beta)) {
        return fail(
            std::format("design_prototype: Kaiser shape parameter {} produced a "
                        "non-finite normalisation",
                        beta));
    }

    // Everything below is computed in double and rounded to float exactly once,
    // at the normalisation step.
    const double centre = 0.5 * static_cast<double>(design_length);
    const double inverse_channels = 1.0 / static_cast<double>(channels);

    std::vector<double> work(taps_used, 0.0);
    double sum = 0.0;
    for (std::size_t n = 0; n < taps_used; ++n) {
        // Offset from the centre tap, exact: both operands are integers and the
        // centre is an integer because M*L is even.
        const double offset = static_cast<double>(n) - centre;

        // Kaiser window, w[n] = I0(beta*sqrt(1 - x^2)) / I0(beta) with x the
        // offset normalised to the half-length. Writing the position as
        // offset/centre rather than 2n/(N-1) - 1 keeps the expression exactly
        // odd in offset, so w[c+i] and w[c-i] are the same double and the
        // filter is symmetric bit for bit. That symmetry is what makes the
        // group delay exactly M*L/2 rather than approximately.
        const double position = (centre > 0.0) ? offset / centre : 0.0;
        const double radicand = std::max(0.0, 1.0 - position * position);
        const double window = bessel_i0(beta * std::sqrt(radicand)) / i0_beta;

        // Ideal lowpass with cutoff at rate/(2M), which in cycles per sample is
        // 0.5/M: h[n] = 2*fc*sinc(2*fc*offset) = (1/M)*sinc(offset/M). The
        // cutoff sits at half a channel spacing, which is what puts the
        // crossover between adjacent channels at the half-power point:
        // measured -6.0206 dB there, against 20*log10(0.5) = -6.0206.
        const double angle = kPi * offset * inverse_channels;
        const double sinc = (offset == 0.0) ? 1.0 : std::sin(angle) / angle;

        work[n] = inverse_channels * sinc * window;
        sum += work[n];
    }

    if (!(sum > 0.0) || !std::isfinite(sum)) {
        return fail(
            std::format("design_prototype: prototype taps sum to {}, which cannot "
                        "be normalised",
                        sum));
    }

    PrototypeFilter prototype;
    prototype.taps.assign(taps_total, 0.0f);
    for (std::size_t n = 0; n < taps_used; ++n) {
        // Divided rather than multiplied by a precomputed reciprocal: one
        // rounding instead of two, and the file runs once per grid change.
        prototype.taps[n] = static_cast<float>(work[n] / sum);
    }

    // sum(h) = 1 by the normalisation above, so a tone at a channel centre
    // comes out of the bank at unit magnitude. Skipping this is how a level
    // ends up 18 dB off with nothing in the chain looking wrong. Measured
    // after the rounding to float at M = 64, L = 16: the taps sum to
    // 1.0000000013, and a tone at a channel centre taken through the branch
    // filter and the transform reads 0.999999993.

    prototype.group_delay_samples = static_cast<SampleIndex>(design_length / 2);
    prototype.stopband_db = measure_stopband_db(prototype.taps, channels);

    return prototype;
}

Expected<std::vector<Complex32>> build_twiddles(std::uint32_t channels) {
    if (channels == 0) {
        return fail("build_twiddles: channel count must not be zero");
    }
    if (!std::has_single_bit(channels)) {
        return fail(
            std::format("build_twiddles: channel count {} is not a power of two", channels));
    }
    if (channels > kMaxChannels) {
        return fail(
            std::format("build_twiddles: channel count {} exceeds the {} the "
                        "channelizer supports",
                        channels, kMaxChannels));
    }

    const std::uint32_t half = channels / 2;
    const std::uint32_t quarter = channels / 4;
    const std::uint32_t octant = channels / 8;

    // libm is consulted for the first octant only, angles in [0, pi/4]. At
    // M = 64 that is nine cosines and nine sines; everything else in the table
    // is one of those values with a sign flipped or the pair swapped, both of
    // which are exact in floating point.
    //
    // The point of doing it this way is the four quadrature entries. Computing
    // W^(M/2) as (cos(pi), sin(pi)) leaves about 1e-16 in the imaginary part,
    // which turns a butterfly that should be a free sign flip into a real
    // complex multiply, and costs bit-exactness for nothing. Reflected from the
    // octant, W^0, W^(M/4), W^(M/2) and W^(3M/4) come out as exactly (1,0),
    // (0,-1), (-1,0) and (0,1).
    std::vector<double> octant_cos(static_cast<std::size_t>(octant) + 1, 0.0);
    std::vector<double> octant_sin(static_cast<std::size_t>(octant) + 1, 0.0);
    for (std::uint32_t i = 0; i <= octant; ++i) {
        const double angle = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(channels);
        octant_cos[i] = std::cos(angle);
        octant_sin[i] = std::sin(angle);
    }

    std::vector<double> real(channels, 0.0);
    std::vector<double> imaginary(channels, 0.0);

    // First quadrant, W^j = (cos(2*pi*j/M), -sin(2*pi*j/M)) for j in [0, M/4].
    // Past the octant boundary the angle's complement is in the octant, and
    // cos and sin swap: cos(a) = sin(pi/2 - a).
    for (std::uint32_t j = 0; j <= quarter && j < channels; ++j) {
        if (j <= octant) {
            real[j] = octant_cos[j];
            imaginary[j] = -octant_sin[j];
        } else {
            const std::uint32_t complement = quarter - j;
            real[j] = octant_sin[complement];
            imaginary[j] = -octant_cos[complement];
        }
    }

    // Second quadrant. W^(M/2 - i) = exp(-j*pi)*exp(+j*2*pi*i/M) = -conj(W^i),
    // so the real part flips sign and the imaginary part is carried across.
    for (std::uint32_t j = quarter + 1; j <= half; ++j) {
        const std::uint32_t mirror = half - j;
        real[j] = -real[mirror];
        imaginary[j] = imaginary[mirror];
    }

    // Lower half by conjugate symmetry, W^(M-i) = conj(W^i).
    for (std::uint32_t j = half + 1; j < channels; ++j) {
        const std::uint32_t mirror = channels - j;
        real[j] = real[mirror];
        imaginary[j] = -imaginary[mirror];
    }

    std::vector<Complex32> table(channels, Complex32{});
    for (std::uint32_t j = 0; j < channels; ++j) {
        table[j] = Complex32{to_float_unsigned_zero(real[j]), to_float_unsigned_zero(imaginary[j])};
    }

    return table;
}

SampleIndex channel_sample_to_input(const GridParams& grid, const PrototypeFilter& prototype,
                                    SampleIndex base, SampleIndex m) {
    // Output block m is taken at input index base + m*D, and the linear-phase
    // prototype delays the signal by exactly M*L/2 input samples, so the input
    // sample that channel sample m is centred on is base + m*D - M*L/2. Every
    // term is an integer by construction; see the design length comment in
    // design_prototype for why the group delay is not a half-integer.
    //
    // The subtraction is left to wrap rather than saturating. A channel sample
    // from before the filter had filled has no input sample behind it, and an
    // index near 2^64 is visibly impossible where a clamp to zero would look
    // like an ordinary answer. Callers skip the ceil(N/D) warm-up blocks after
    // a grid change, which is where such an m would come from.
    const auto decimation = static_cast<SampleIndex>(grid.decimation);
    return base + m * decimation - prototype.group_delay_samples;
}

}  // namespace revenant::dsp
