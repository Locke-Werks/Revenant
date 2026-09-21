// CPU twin of core/shaders/spectrum.comp: the full-span spectrum stage.
//
// This is a referee, not a library. It exists so CI can prove that the kernel
// on the display path computes what it claims to, on every vendor's driver,
// and it is written to match the kernel's operation ORDER rather than merely
// its mathematical result. Read the two files side by side; every statement
// here has a counterpart there, in the same order, the way
// core/dsp/pfb_fft_reference.cpp reads against core/shaders/pfb_fft.comp.
//
// WHAT THE STAGE IS
//
// The channelizer has already produced M coarse channels on a 2x-oversampled
// grid. This stage transforms N consecutive samples of each channel's own time
// series and keeps the central half of each transform's bins, which tiles the
// whole span exactly once: the oversampling makes adjacent channels overlap by
// half, so taking only the N/4 bins either side of each channel's centre gives
// every frequency exactly one owner. docs/detection.md, "Where the spectrum
// comes from", is the decision and the arithmetic behind it.
//
// Those N/4 bins either side reach exactly as far as the prototype's cutoff,
// so each channel arrives about 6 dB down at both ends of its own band and the
// span shows that droop once per channel. build_spectrum_window below carries
// the per-bin gain that divides it out, and docs/detection.md, "The channel
// shape, and why it is a measurement error", is why it is not only a display
// problem.
//
// The transform itself is reference_fft_radix2 from
// core/dsp/pfb_fft_reference.h, unchanged. That is the point: the kernel's
// butterfly graph is core/shaders/pfb_fft.comp's, already proved bit-exact on
// both devices in the conformance matrix, so the twin reuses the twin rather
// than growing a second transform that agrees by luck.
//
// Written from published mathematics: Oppenheim and Schafer, Discrete-Time
// Signal Processing, chapter 9, for the transform; harris, Multirate Signal
// Processing for Communication Systems, chapter 6, for the oversampled channel
// bank; Harris, "On the use of windows for harmonic analysis with the discrete
// Fourier transform", Proceedings of the IEEE 66(1), for the window. No
// GPL-licensed implementation was read; see docs/clean-room.md.

#pragma once

#include <cstdint>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/pfb_fft_reference.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::dsp {

// The kernel's full parameter set: its three specialization constants and its
// three push constants, in one struct so the twin and the dispatch that
// configures the pipeline cannot drift apart.
struct SpectrumParams {
    // Specialization constant 1. M, the coarse channel count, a power of two.
    std::uint32_t channels = 64;

    // Specialization constant 2. N, points in one channel's transform, a power
    // of two of at least four. Four is the smallest that has a central half
    // with a bin either side of the channel centre.
    std::uint32_t transform = 2048;

    // Specialization constant 3. log2(N). Carried rather than derived because
    // the kernel carries it, and a twin that derived it would not notice a
    // host that specialized it wrongly.
    std::uint32_t stages = 11;

    // Push constant. Per-channel capacity of the channel ring in blocks, which
    // is also the stride from one channel's time series to the next, and its
    // mask.
    std::uint32_t chan_blocks = 0;
    std::uint32_t chan_mask = 0;

    // Push constant. Ring slot of the oldest channel sample in the window this
    // frame transforms. The host owns the absolute 64-bit index; GLSL has no
    // 64-bit integer here, so it hands down the reduced offset.
    std::uint32_t in_offset = 0;
};

// Largest transform build_twiddles() will produce a circle for.
//
// Not a device limit and not this stage's own: core/dsp/pfb_design.cpp caps
// its channel count at 2048 and build_twiddles shares that cap, because the
// table was written for a channelizer grid. A larger spectrum transform needs
// that ceiling raised rather than this one, which is a change to the
// channelizer's design file and wants its own justification.
inline constexpr std::uint32_t kMaxSpectrumTransform = 2048;

// The transform size the stage uses when nothing asks for another.
//
// 2048 points is 16 KiB of shared memory, which fits both devices in the
// conformance matrix: the AMD integrated part reports 32768 bytes and the
// RTX 4090 reports 49152, and the usable figure is lower again once the driver
// has taken its own share. docs/detection.md's table is where this number
// comes from. The device's real ceiling is still read at run time through
// max_spectrum_transform_size rather than assumed from this.
inline constexpr std::uint32_t kDefaultSpectrumTransform = 2048;

// Below this a bin is silence for every purpose here. Taking the logarithm of
// zero gives -inf, which propagates into a colour map and into a percentile as
// a NaN nobody can source. 1e-20 in power is exactly -200 dB, the same floor
// core/engine/graph.cpp uses for the signal meter.
inline constexpr float kSpectrumPowerFloor = 1.0e-20F;
inline constexpr float kSpectrumFloorDb = -200.0F;

// Ceiling on the channel-shape correction below, in decibels of power.
//
// A guard against a table entry that is not a number, not a working part.
// Every prototype design_prototype builds puts its cutoff at half a channel
// spacing, which is exactly where the kept band ends, so what the correction
// has to undo is the cutoff and never the stopband.
//
// Measured at a 120 dB target and a 2048-point transform, the deepest point
// in the kept band is its outer edge in every case, and its depth settles on
// the half-power crossover as the prototype lengthens. -6.0206 dB, which is
// 20*log10(0.5), at taps_per_branch of 16 and upward: 16, 17, 24, 33, 48, 64
// and 129 all agree to four decimals. Below that the transition has not
// finished by the band edge and the figure drifts either way: -6.0208 dB at
// 12, -6.0279 at 8, -5.9909 at 5, -6.0627 at 4 and -8.0045 at 3. So 12 dB
// clears the deepest of them by four decibels and the clamp never fires; it
// exists because a response of zero would put an infinity in an array that
// gets uploaded to a device, and an infinity there poisons a whole frame
// rather than one bin.
//
// WHAT THIS PARAGRAPH USED TO SAY: "the deepest point in the kept band is
// -6.0206 dB at taps_per_branch of 4, 5, 8, 12, 16, 17, 24, 33, 48, 64 and
// 129, and the one outlier is a three-tap branch at -8.0045 dB." Four of
// those eleven do not measure -6.0206 and one of them, five taps per branch,
// is not even below it. The list read as eleven independent confirmations of
// one number and was one number plus four that had been rounded into it.
inline constexpr float kMaxSpectrumCorrectionDb = 12.0F;

// Bins one channel contributes: the central half of its transform.
[[nodiscard]] constexpr std::uint32_t spectrum_bins_per_channel(std::uint32_t transform) {
    return transform / 2;
}

// Floats in the buffer build_spectrum_window produces, which is the analysis
// window followed by the channel-shape correction. See that function.
[[nodiscard]] constexpr std::uint32_t spectrum_window_length(std::uint32_t transform) {
    return transform + spectrum_bins_per_channel(transform);
}

// Bins in one whole frame, across the whole span.
[[nodiscard]] constexpr std::uint32_t spectrum_bin_count(std::uint32_t channels,
                                                          std::uint32_t transform) {
    return channels * spectrum_bins_per_channel(transform);
}

// Where coarse channel k sits in a frame ordered by ascending frequency.
//
// Channels at and above M/2 are negative frequencies, so channel M/2 is the
// most negative and comes first, and channel 0, baseband DC, lands in the
// middle of the frame.
[[nodiscard]] constexpr std::uint32_t spectrum_channel_slot(std::uint32_t channels,
                                                             std::uint32_t channel) {
    return (channel + (channels / 2)) & (channels - 1);
}

// Which transform bin ascending bin j of a channel comes from. Walks the
// negative quarter of the transform and then the positive quarter, which is
// the central half in frequency order.
[[nodiscard]] constexpr std::uint32_t spectrum_source_bin(std::uint32_t transform,
                                                           std::uint32_t bin) {
    return (bin + transform - (transform / 4)) & (transform - 1);
}

// Largest transform this device holds in one workgroup's shared memory, capped
// at what build_twiddles will serve.
//
// Call this with VkPhysicalDeviceLimits::maxComputeSharedMemorySize rather
// than assuming a number. The two devices in the conformance matrix do not
// agree on the budget, and a kernel that baked one in could not be launched on
// the device with the smaller cap, which is the one device that would have
// caught the bug.
[[nodiscard]] constexpr std::uint32_t max_spectrum_transform_size(
    std::uint32_t shared_memory_bytes) {
    const std::uint32_t fits = max_fft_transform_size(shared_memory_bytes);
    return fits < kMaxSpectrumTransform ? fits : kMaxSpectrumTransform;
}

// Rejects a parameter set the kernel cannot run.
[[nodiscard]] Status validate(const SpectrumParams& params);

// The spectrum stage's two host-built tables, in one array, because the kernel
// reads them from one binding. spectrum_window_length(N) floats:
//
//   [0, N)              the analysis window, one tap per transform input.
//   [N, N + N/2)        the channel-shape correction, one power gain per kept
//                       output bin, in the same ascending order the frame is
//                       written in.
//
// THE WINDOW
//
// N real taps, normalised so that its coherent gain is one: a full-scale tone
// at a bin centre then reads 0 dB.
//
// Four-term Blackman-Harris, periodic rather than symmetric because this is a
// transform of a continuing stream and not of an isolated record. Its
// sidelobes are 92 dB down where a rectangular window's are 13 dB down, and
// 13 dB is not enough to keep one broadcast carrier from smearing across the
// whole display, which reads as a noisy receiver rather than as an artefact of
// not windowing.
//
// THE CORRECTION, AND WHY IT IS NOT PART OF THE WINDOW
//
// The channelizer's prototype has its cutoff at rate/(2M), so adjacent
// channels cross at half amplitude there. The kept band is the central half of
// a transform of a channel running at 2*rate/M, which is +/- rate/(2M) around
// the channel centre. Those are the same frequency, so each channel's kept
// band ends exactly on the prototype's cutoff and every channel's contribution
// droops toward both of its own edges. Measured on white noise through the
// channelizer twins at M = 64, N = 2048, 49152 averages per bin: 0 dB across
// the middle, -1.82 dB at 0.4375 of a channel spacing from the centre,
// -3.44 dB at 0.4688, and -5.99 dB in the outermost kept bin. The whole curve
// matched the prototype's own magnitude response to within 0.076 dB, so the
// droop is the filter and nothing else.
//
// Dividing it out is one gain per kept bin, and that CANNOT be folded into the
// window. The window multiplies the transform's input, indexed over N time
// samples; the correction scales the transform's output, indexed over the N/2
// kept bins. A per-sample multiply is a convolution in frequency, not a
// per-bin gain, and the two index spaces are not even the same length. So the
// correction is a separate table applied after the transform, and it rides in
// this buffer only because the kernel has four bindings and adding a fifth is
// a change to the graph that dispatches it.
//
// GRID DEPENDENCE, MEASURED
//
// The correction inverts the prototype, so it is a property of the grid. Which
// part of the grid matters is a measurement rather than a guess. Expressed
// against offset in channel spacings, which is how the kept band is indexed,
// the response does not depend on the channel count at all: at 17 taps per
// branch, designs at M = 8, 16, 32, 128, 256 and 1024 against M = 64 differ by
// 0.0000 dB across all 1024 kept bins of a 2048-point transform, and the same
// holds at 9, 33 and 65 taps. It does depend on the prototype length and the
// stopband target, because those set how far the transition has settled by the
// band edge: against the canonical 17 taps at 120 dB, 9 taps differ by
// 0.93 dB, 33 taps by 1.75 dB and an 80 dB target by 0.58 dB.
//
// So the parameters here are the two that were measured to matter, and their
// defaults are the canonical grid's: 17 is what GridParams and EngineConfig
// both hold, and 120 dB is design_prototype's own default, which is what
// core/engine/engine.cpp asks for. A caller running a different prototype
// length has to pass it, and core/engine/graph.cpp does: the full-span stage
// calls build_spectrum_window(points, impl.grid.taps_per_branch), so
// `revenant-engine --taps` away from 17 is corrected for the prototype the
// grid actually built.
//
// WHAT THIS PARAGRAPH USED TO SAY: "core/engine/graph.cpp does not yet, so
// `revenant-engine --taps` away from 17 leaves up to about 1.8 dB of the
// droop uncorrected near the seams; closing that is one argument at its call
// site." The argument was added at that call site and this sentence was not
// taken out with it. Left standing it tells anybody chasing a seam-edge level
// that up to 1.8 dB of known droop is still in the data and that the cause is
// elsewhere, which is the most expensive kind of comment to leave behind.
//
// One caller still defaults the tap count on purpose. passband_window in
// core/engine/graph.cpp overwrites the correction half of the buffer with
// unity, because a passband frame has no seam to hide, so the parameters that
// built that half are irrelevant there. Its own comment says so.
//
// Both halves are built on the host in double and rounded to float once, for
// the same reason the prototype filter and the twiddle table are: nothing that
// feeds a bit-exact comparison is computed in a shader.
[[nodiscard]] Expected<std::vector<float>> build_spectrum_window(
    std::uint32_t size, std::uint32_t taps_per_branch = GridParams{}.taps_per_branch,
    double attenuation_db = 120.0);

// log2(value) for a positive normal value, deterministically.
//
// Exposed because a bit-exact diff against the kernel proves the two agree and
// says nothing about whether either computes a logarithm. The test suite
// checks this against std::log2 the same way it checks det_atan2 against
// std::atan2.
//
// Vulkan permits GLSL's log2 several units in the last place, so a kernel that
// called it could not be refereed at all. This is an exponent split, which is
// exact, plus a five-term atanh series evaluated by descending Horner, which
// is a fixed sequence of correctly-rounded operations.
[[nodiscard]] float det_log2(float value);

// Twin of core/shaders/spectrum.comp, whole.
//
// channel_ring is the channelizer's output, channel-major at
// k * chan_blocks + (m & chan_mask), exactly as core/shaders/pfb_fft.comp
// wrote it. twiddles is the N-entry circle from build_twiddles(N), the same
// float array the device was given, and window the spectrum_window_length(N)
// floats from build_spectrum_window(N), which is the analysis window followed
// by the channel-shape correction. frame receives channels * N/2 decibel
// values, ascending in frequency across the whole span.
[[nodiscard]] Status reference_spectrum(const SpectrumParams& params,
                                        ConstComplexSpan channel_ring,
                                        ConstComplexSpan twiddles,
                                        ConstRealSpan window,
                                        RealSpan frame);

}  // namespace revenant::dsp
