// The polyphase channelizer's shared vocabulary.
//
// The channelizer is the core primitive of the whole application: a prototype
// lowpass filter polyphase-decomposed into M branches, followed by an M-point
// FFT, producing M channels in one pass. Near-free channels are what make two
// hundred simultaneous receivers cost roughly what two cost today, and every
// feature above Layer 1 is downstream of that.
//
// This header declares only the host-side vocabulary: the grid, the filter and
// twiddle tables the kernels read, and the exact-rational channel centre. The
// kernels are in core/shaders/, their CPU twins in core/dsp/, and the
// orchestration that binds them in core/dsp/channelizer.h.

#pragma once

#include <cstdint>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::dsp {

// Parameters of the coarse grid. These change only when the sample rate
// changes or the user asks for a channel count beyond the current grid, which
// is why they are specialization constants in the kernels rather than push
// constants: it lets the tap loop unroll and lets the critically-sampled case
// fold its phase correction away entirely at pipeline creation.
//
// Adding a receiver must never change any of these. A receiver attaches to the
// nearest grid channel and does its own fine mixing; it touches no field here.
struct GridParams {
    // M. A power of two. Channel spacing is rate/M.
    std::uint32_t channels = 64;

    // L + 1, including the one zero tap that makes the group delay an integer
    // number of channel samples. See prototype_group_delay below.
    std::uint32_t taps_per_branch = 17;

    // D, which must divide M. D == M is critically sampled; D == M/2 is the
    // 2x-oversampled design, which is what this project uses, because a
    // critically-sampled bank splits a signal sitting on a channel edge across
    // two channels and neither one is usable.
    std::uint32_t decimation = 32;

    [[nodiscard]] constexpr std::uint32_t oversample_numerator() const { return channels; }
    [[nodiscard]] constexpr std::uint32_t oversample_denominator() const { return decimation; }

    // Total prototype length actually uploaded, channels * taps_per_branch.
    [[nodiscard]] constexpr std::uint32_t prototype_length() const {
        return channels * taps_per_branch;
    }
};

// Rejects a grid the kernels cannot run: a non-power-of-two channel count, a
// decimation that does not divide it, zero taps, or a prototype larger than the
// device can hold.
[[nodiscard]] Status validate(const GridParams& grid);

// A channel's centre frequency, kept exact.
//
// k * rate / M is frequently not an integer number of hertz: 2,500,000 over 64
// channels puts channel 1 at 39062.5 Hz. Rounding that to the integer-hertz
// type at the point the channel is described would introduce precisely the
// unsourceable tuning offset the conventions warn about, accumulated once per
// channel and then again through every mixing stage below it. So a centre is
// carried as the exact rational it is, and a receiver computes its residual
// offset in integer arithmetic before the single conversion to a phase
// increment.
struct ChannelCentre {
    // numerator / denominator hertz, exactly. Signed because channels above
    // M/2 are negative frequencies: channel M-1 is -rate/M.
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    [[nodiscard]] constexpr double hertz() const {
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }

    // True when the centre lands on a whole hertz, which lets a caller take the
    // integer path with no rounding at all.
    [[nodiscard]] constexpr bool is_integral() const {
        return denominator != 0 && numerator % denominator == 0;
    }
};

// Centre of channel k, exact. Channels below M/2 are positive frequencies,
// channels at or above M/2 are negative.
[[nodiscard]] ChannelCentre channel_centre(const GridParams& grid, SampleRate rate,
                                           std::uint32_t channel);

// The prototype filter and the twiddle table, both built on the host in double
// and rounded to float once.
//
// Neither is ever computed in a shader. GLSL's sin and cos are permitted
// several units in the last place and each vendor spends them differently, so
// a shader-computed twiddle would end bit-exactness on its own, before any
// question of accumulation order arose.
struct PrototypeFilter {
    // channels * taps_per_branch real taps, normalised so a tone at a channel
    // centre reads unit magnitude.
    std::vector<float> taps;

    // Measured stopband attenuation in decibels, negative. Recorded so a test
    // can assert the design met its target rather than trusting the estimate.
    double stopband_db = 0.0;

    // Delay through the filter, in input samples, exact. The extra zero tap in
    // taps_per_branch exists to make this an integer, so that a channel sample
    // maps to an input sample index with no fractional bookkeeping and the
    // sample-accurate-timestamp guarantee survives channelization.
    SampleIndex group_delay_samples = 0;
};

// Kaiser-windowed sinc, cutoff at rate/(2M) so adjacent channels cross at the
// half-power point. attenuation_db is the stopband target, positive decibels.
[[nodiscard]] Expected<PrototypeFilter> design_prototype(const GridParams& grid,
                                                          double attenuation_db = 120.0);

// The M-entry circle W_M^j = exp(-j*2*pi*j/M), built from the first octant and
// reflected, so that the four quadrature entries are exactly (1,0), (0,-1),
// (-1,0) and (0,1). Computing W^(M/2) from cos(pi) and sin(pi) leaves a
// residue in the imaginary part, which turns a free sign flip into a real
// complex multiply and costs bit-exactness for nothing.
[[nodiscard]] Expected<std::vector<Complex32>> build_twiddles(std::uint32_t channels);

// Input sample index that channel sample m of the given grid corresponds to,
// exactly. Integer by construction; see PrototypeFilter::group_delay_samples.
[[nodiscard]] SampleIndex channel_sample_to_input(const GridParams& grid,
                                                  const PrototypeFilter& prototype,
                                                  SampleIndex base, SampleIndex m);

}  // namespace revenant::dsp
