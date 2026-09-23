// Removing a receiver's centre spike and its mirror images.
//
// TWO DEFECTS OF A ZERO-IF FRONT END, and one stage for both because both are
// read off the same statistics of the same samples.
//
//   DC offset. A constant added to I and Q: LO leakage into the mixer, an
//   ADC's own offset, or rounding in a digital downconverter. At complex
//   baseband a constant is a carrier at exactly 0 Hz, so it draws a spike in
//   the middle of the span that looks like a station on the tuned frequency.
//
//   I/Q imbalance. The two arms of the quadrature mixer differ slightly in
//   gain and are not quite 90 degrees apart. A tone at +f then also appears at
//   -f, attenuated by the image rejection ratio, which for a gain ratio of
//   1.05 (0.42 dB) and a phase error of 3 degrees is 28.9 dB. A strong station puts a
//   ghost of itself on the other side of the centre that the detector reports
//   as a real track.
//
// THE MODEL AND THE ESTIMATOR. The ideal signal x = I + jQ is proper: I and Q
// have equal power and are uncorrelated. That holds for noise, for any single
// complex tone and for any spectrum that is not its own mirror image, which is
// the ordinary state of a wideband capture. The imbalanced receiver delivers
//
//     I_r = I
//     Q_r = g * (Q cos(phi) + I sin(phi))
//
// so, with P the power of each arm,
//
//     E[I_r^2] = P      E[Q_r^2] = g^2 P      E[I_r Q_r] = g P sin(phi)
//
// and the three second-order moments give the two unknowns directly:
//
//     g        = sqrt(E[Q_r^2] / E[I_r^2])
//     sin(phi) = E[I_r Q_r] / sqrt(E[I_r^2] E[Q_r^2])
//
// Inverting the model is one row of a 2x2 matrix, which is a Gram-Schmidt
// orthonormalisation of Q against I:
//
//     I_c = I_r
//     Q_c = (Q_r / g - I_r sin(phi)) / cos(phi)  =  cross * I_r + scale * Q_r
//     cross = -tan(phi),  scale = 1 / (g cos(phi))
//
// This is the blind, feed-forward, second-order-statistics estimator of the
// literature, written from the published mathematics per docs/clean-room.md:
// N. A. Moseley and C. H. Slump, "A low-complexity feed-forward I/Q imbalance
// compensation algorithm", 17th Annual Workshop on Circuits, Systems and
// Signal Processing (ProRISC), 2006, which estimates the same two parameters
// from sign statistics, and the Gram-Schmidt orthogonalisation procedure of
// I. Fatadin, S. J. Savory and D. Ives, "Compensation of quadrature imbalance
// in an optical QPSK coherent receiver", IEEE Photonics Technology Letters
// 20(20), 2008, which is the correction above. The moments here are the
// second-order ones rather than Moseley and Slump's sign statistics because
// the GPU sums squares as cheaply as signs and the second-order estimate is
// exact for the model rather than approximately so.
//
// WHERE EACH HALF RUNS. The per-sample work is on the device, as two kernels
// with CPU twins: core/shaders/iq_moments.comp sums the five moments over
// fixed chunks of a block, and core/shaders/iq_correct.comp subtracts the DC
// estimate and applies the correction in place on the ring. Both are one
// correctly rounded add or multiply at a time under `precise`, so the twins
// below reproduce the bits.
//
// The estimate itself is a handful of numbers per block and runs on the host,
// in double, in FrontEndCorrector: a square root, a division and an
// exponential average, none of which a GPU performs identically to a CPU. It
// reads a frame's chunk sums when the graph reuses that frame's slot, so the
// correction applied to a block comes from the blocks before the frames still
// in flight. At the default 13.6 ms block and three frames that is about 40
// ms, against time constants of 100 ms and a second.

#pragma once

#include <cmath>
#include <cstdint>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::dsp {

// Samples summed by one invocation of the moments kernel, in order. A
// constant in both the kernel and the twin rather than a push constant, so a
// host cannot make the two disagree about the summation order, which is the
// whole of what bit-exactness in a float sum rests on.
inline constexpr std::uint32_t kIqMomentsChunk = 64;

// Floats per chunk in the moments output: sum I, sum Q, sum I^2, sum Q^2,
// sum IQ, in that order.
inline constexpr std::uint32_t kIqMomentsPerChunk = 5;

[[nodiscard]] constexpr std::uint32_t iq_moments_chunks(std::uint32_t count) {
    return (count + kIqMomentsChunk - 1) / kIqMomentsChunk;
}

// The moments kernel's push constants, exactly as core/shaders/iq_moments.comp
// declares them.
struct IqMomentsParams {
    // Ring capacity minus one, a power of two less one.
    std::uint32_t ring_mask = 0;

    // Ring offset of the block's first sample.
    std::uint32_t src_offset = 0;

    // Samples in the block. One invocation per kIqMomentsChunk of them.
    std::uint32_t count = 0;
};

static_assert(sizeof(IqMomentsParams) == 3 * sizeof(std::uint32_t),
              "IqMomentsParams must alias core/shaders/iq_moments.comp's push constants");

// The correction kernel's push constants, exactly as
// core/shaders/iq_correct.comp declares them.
//
// The identity is dc 0, cross 0, scale 1, and it is exact in value: x - 0 is
// x, 0 * i is a zero, and a zero plus 1 * q is q. Only a negative-zero Q can
// come back with its sign changed.
struct IqCorrectParams {
    std::uint32_t ring_mask = 0;
    std::uint32_t offset = 0;
    std::uint32_t count = 0;

    // Subtracted from I and from Q.
    float dc_i = 0.0F;
    float dc_q = 0.0F;

    // Q_c = cross * I + scale * Q, after the DC subtraction.
    float cross = 0.0F;
    float scale = 1.0F;
};

static_assert(sizeof(IqCorrectParams) == 3 * sizeof(std::uint32_t) + 4 * sizeof(float),
              "IqCorrectParams must alias core/shaders/iq_correct.comp's push constants");

// Twin of core/shaders/iq_moments.comp.
//
// ring is the whole ring, ring_mask + 1 samples. sums receives
// iq_moments_chunks(count) * kIqMomentsPerChunk floats; anything past that is
// left alone. Each chunk is summed in sample order from zero, one rounding per
// operation, exactly as the kernel does.
[[nodiscard]] Status reference_iq_moments(ConstComplexSpan ring, const IqMomentsParams& params,
                                          RealSpan sums);

// Twin of core/shaders/iq_correct.comp.
//
// Reads the block's slots from `in` and writes them to `out`, both whole
// rings; the engine binds one buffer as both. Slots outside the block are not
// written.
[[nodiscard]] Status reference_iq_correct(ConstComplexSpan in, const IqCorrectParams& params,
                                          ComplexSpan out);

// A block's five moments, summed in double from the kernel's chunk sums.
struct IqMoments {
    std::uint64_t samples = 0;
    double sum_i = 0.0;
    double sum_q = 0.0;
    double sum_ii = 0.0;
    double sum_qq = 0.0;
    double sum_iq = 0.0;
};

// Adds the chunk sums the kernel wrote for a block of `count` samples, in
// chunk order.
[[nodiscard]] IqMoments total_moments(ConstRealSpan chunk_sums, std::uint32_t count);

struct FrontEndCorrectionConfig {
    // The DC estimate's time constant. A tenth of a second follows the jump
    // in LO leakage a retune or a gain change brings within about half a
    // second, and it is a notch 1.6 Hz wide: nothing a person transmits lives
    // that close to the centre. Longer would be a narrower notch and a slower
    // recovery; the notch is already far narrower than a spectrum bin.
    double dc_time_constant_s = 0.1;

    // The imbalance estimate's. A second, because imbalance is a property of
    // the mixer and moves with temperature and with the tuner's band, not
    // from block to block, and because a longer average is what keeps a strong
    // signal that happens to be its own mirror, such as a tone exactly
    // centred, from dragging the estimate while it lasts.
    double iq_time_constant_s = 1.0;

    // An estimate outside these is left unapplied and reported as
    // implausible. Six decibels and thirty degrees are an order of magnitude
    // past what a working mixer shows; a stream reading like that is not
    // proper, which is what a real-valued signal, a direct-sampling input with
    // one arm dead, or a single tone exactly at DC looks like, and correcting
    // it would make it worse.
    double max_gain_error_db = 6.0;
    double max_phase_error_deg = 30.0;
};

// What the estimator believes, for the engine to apply and for an operator
// to read.
struct FrontEndEstimate {
    // False until a block has been read. Nothing is applied before it.
    bool measured = false;
    std::uint64_t samples_seen = 0;

    // The DC estimate, full scale 1.0.
    double dc_i = 0.0;
    double dc_q = 0.0;

    // The imbalance, and whether it passed the plausibility bounds.
    double gain = 1.0;
    double sin_phase = 0.0;
    bool iq_plausible = false;

    // 10 log10(dc_i^2 + dc_q^2), which is the spike's level on a spectrum
    // whose full scale is a complex sinusoid of amplitude one. -300 for none.
    [[nodiscard]] double dc_dbfs() const;
    [[nodiscard]] double gain_error_db() const { return 20.0 * std::log10(gain); }
    [[nodiscard]] double phase_error_deg() const;

    // What the uncorrected stream's image rejection is, from the estimate:
    // (1 + 2 g cos(phi) + g^2) / (1 - 2 g cos(phi) + g^2), in decibels.
    [[nodiscard]] double image_rejection_db() const;
};

// The estimator. Host code, one instance per open source, driven by the
// graph's recording thread and by nothing else, so it holds no lock.
class FrontEndCorrector {
public:
    explicit FrontEndCorrector(FrontEndCorrectionConfig config = {});

    void reset();

    // One block's moments. The first block seeds the averages outright rather
    // than being averaged against zero, so the correction is right from the
    // first block it is applied to rather than a time constant later.
    //
    // Each block is weighted by its length: alpha = 1 - exp(-n / (tau rate)),
    // so the time constants are seconds whatever the block size.
    void update(const IqMoments& moments, SampleRate rate);

    [[nodiscard]] const FrontEndEstimate& estimate() const { return estimate_; }

    // The push constants for the correction kernel, with the ring fields left
    // for the caller. The identity for a half that is switched off, not yet
    // measured, or, for I/Q, not plausible.
    [[nodiscard]] IqCorrectParams correction(bool dc, bool iq) const;

private:
    FrontEndCorrectionConfig config_;
    FrontEndEstimate estimate_{};

    // Central second moments, averaged.
    double m_ii_ = 0.0;
    double m_qq_ = 0.0;
    double m_iq_ = 0.0;
};

}  // namespace revenant::dsp
