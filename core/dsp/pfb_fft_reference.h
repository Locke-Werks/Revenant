// CPU twin of core/shaders/pfb_fft.comp: the channelizer's M-point FFT.
//
// This is a referee, not a library. It exists so CI can prove that the kernel
// on the sample path computes what it claims to, on every vendor's driver, and
// it is written to match the kernel's operation ORDER rather than merely its
// mathematical result. An FFT's summation order determines its rounding: a
// decimation-in-time and a decimation-in-frequency transform of the same
// 64-point input were measured 131072 ULP apart, so there is no tolerance
// between two orderings that would not also admit an FFT that is simply wrong
// wherever an output happens to be small. The diff therefore demands
// bit-identical output, and these functions transcribe the shader's butterfly
// graph stage by stage, reading the same host-built twiddle table the kernel
// was handed.
//
// Read the two files side by side. Every statement here has a counterpart
// there, in the same order, the way core/dsp/complex_ops.cpp reads against
// core/shaders/cmul.comp.
//
// Written from published mathematics: Oppenheim and Schafer, Discrete-Time
// Signal Processing, chapter 9, for the radix-2 decimation-in-time transform
// and its bit-reversed load; harris, Multirate Signal Processing for
// Communication Systems, chapter 6, for the channelizer the transform sits
// inside and for the output phase correction at D < M. No GPL-licensed
// implementation was read; see docs/clean-room.md.

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::dsp {

// The kernel's full parameter set: its three grid specialization constants and
// its four push constants, in one struct so the twin and the dispatch that
// configures the pipeline cannot drift apart.
//
// The defaults are the project's grid: M = 64 channels, D = M/2 = 32, which is
// the 2x-oversampled design.
struct PfbFftParams {
    // Specialization constant 1. M, a power of two, at least 2.
    std::uint32_t channels = 64;

    // Specialization constant 2. D, which must divide M. D == M folds the
    // phase correction away entirely, in the shader at pipeline creation and
    // here at run time.
    std::uint32_t decimation = 32;

    // Specialization constant 3. log2(M). Carried rather than derived because
    // the kernel carries it, and a twin that derived it would not notice a
    // host that specialized it wrongly.
    std::uint32_t stages = 6;

    // Push constant. Absolute output block index of block 0, truncated to 32
    // bits exactly as the kernel receives it. SampleIndex is uint64 and GLSL
    // has no 64-bit integer here, so the host owns the full index and hands
    // down this offset. The truncation is harmless because both places it is
    // used reduce it modulo a power of two that divides 2^32.
    std::uint32_t block_base = 0;

    // Push constant. Number of output blocks in this dispatch, one workgroup
    // each.
    std::uint32_t block_count = 0;

    // Push constant. Per-channel ring capacity in blocks, a power of two, and
    // its mask. The channel ring is channel-major: a receiver reads one
    // channel's time series, so time is the contiguous axis.
    std::uint32_t out_ring_blocks = 0;
    std::uint32_t out_ring_mask = 0;
};

// Rejects a parameter set the kernel cannot run, or can run only
// non-deterministically.
//
// The non-obvious rejection is block_count > out_ring_blocks. That dispatch
// would have two workgroups writing the same ring slot with no ordering
// between them, so the device result depends on scheduling and no CPU twin can
// be bit-exact against it. It is a sizing mistake, not a race to be fixed with
// a barrier.
[[nodiscard]] Status validate(const PfbFftParams& params);

// Twin of core/shaders/pfb_fft.comp, whole.
//
// branch_values is the branch-filter output, block-major, block*M + slot, with
// the (M - r) mod M branch reversal already applied by kernel 1. twiddles is
// the M-entry circle from build_twiddles() in core/dsp/pfb.h, the same float
// array the device was given. channel_ring is written channel-major at
// k * out_ring_blocks + (m & out_ring_mask), exactly the slots the kernel
// writes and no others, so a caller can zero both buffers and compare all of
// them.
[[nodiscard]] Status reference_pfb_fft(const PfbFftParams& params,
                                       ConstComplexSpan branch_values,
                                       ConstComplexSpan twiddles,
                                       ComplexSpan channel_ring);

// The transform on its own: the same butterfly graph, without the output phase
// correction and without the channel-major scatter.
//
// Both spans are block-major, batch_count blocks of transform_size complex
// values, natural order in and natural order out. They must not alias. This is
// the surface a numerical probe wants, and it is the same code path
// reference_pfb_fft runs, so exercising it exercises the twin that referees
// the kernel rather than a second implementation that agrees by luck.
[[nodiscard]] Status reference_fft_radix2(ConstComplexSpan twiddles,
                                          ConstComplexSpan input,
                                          ComplexSpan output,
                                          std::uint32_t transform_size,
                                          std::uint32_t batch_count);

// log2 of a power-of-two transform size, which is the kernel's stage count.
// Rounds up for a size that is not a power of two, which validate() rejects
// anyway; the rounding exists so the loop terminates rather than to be used.
[[nodiscard]] constexpr std::uint32_t fft_stages(std::uint32_t transform_size) {
    std::uint32_t stages = 0;
    while (stages < 31 && (1u << stages) < transform_size) {
        ++stages;
    }
    return stages;
}

// Shared memory the kernel needs to hold one transform: M complex float32.
[[nodiscard]] constexpr std::uint32_t fft_shared_bytes(std::uint32_t transform_size) {
    return transform_size * 8u;
}

// Largest power-of-two transform that fits a device's shared memory budget.
//
// Call this with VkPhysicalDeviceLimits::maxComputeSharedMemorySize rather than
// assuming a number. The two devices in the conformance matrix do not agree on
// the budget: the AMD integrated part reports 32768 and the RTX 4090 reports
// 49152. Both happen to land on M = 4096 here, because 49152 bytes holds 6144
// complex float32 and the next power of two down is 4096, and in practice the
// usable figure is lower again once the driver has taken its own share. The
// point is not the number, which moves with the hardware. It is that a kernel
// which baked one in could not be launched on the device with the smaller cap,
// which is the one device that would have caught the bug. Above whatever this
// returns, the transform needs the multi-dispatch Stockham form, which is the
// same butterfly graph split across dispatches and leaves this twin valid.
[[nodiscard]] constexpr std::uint32_t max_fft_transform_size(
    std::uint32_t shared_memory_bytes) {
    std::uint32_t size = 0;
    for (std::uint32_t candidate = 1; candidate <= (1u << 28); candidate <<= 1) {
        if (fft_shared_bytes(candidate) > shared_memory_bytes) {
            break;
        }
        size = candidate;
    }
    return size;
}

// Where channel k of output block m lands in the channel ring. Channel-major,
// so one channel's time series is contiguous. Stated once here because the
// kernel, the twin and every consumer have to agree on it. block_index may be
// absolute or already reduced to a slot; the mask is applied here either way.
[[nodiscard]] constexpr std::size_t channel_ring_index(const PfbFftParams& params,
                                                       std::uint32_t channel,
                                                       std::uint32_t block_index) {
    return static_cast<std::size_t>(channel) * params.out_ring_blocks +
           (block_index & params.out_ring_mask);
}

}  // namespace revenant::dsp
