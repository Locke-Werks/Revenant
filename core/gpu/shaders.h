// The compiled shader registry.
//
// Every kernel is compiled, validated and embedded at build time by
// cmake/CompileShaders.cmake. This is the only place the generated headers are
// included, so the SPIR-V words exist once in the binary rather than once per
// translation unit that dispatches a kernel.

#pragma once

#include <cstdint>
#include <span>

namespace revenant::gpu::shaders {

// Copy in to out. The plumbing check: proves data arrived before any kernel is
// asked whether its arithmetic is right.
[[nodiscard]] std::span<const std::uint32_t> identity();

// Element-wise complex multiply, decorated NoContraction throughout. Twin of
// revenant::dsp::reference_cmul.
[[nodiscard]] std::span<const std::uint32_t> cmul();

// Stage one of the channelizer: the polyphase branch filters. Writes branch r
// to FFT input slot (M - r) mod M, which is what lets stage two be an ordinary
// forward transform. Twin of revenant::dsp::reference_pfb_branches.
[[nodiscard]] std::span<const std::uint32_t> pfb_branch();

// Stage two: the M-point transform, with the output phase correction folded
// into the write. Twin of revenant::dsp::reference_pfb_fft.
[[nodiscard]] std::span<const std::uint32_t> pfb_fft();

// Native source formats widened into the ring's canonical Complex32.
//
// These run as part of the upload rather than on the host. An RTL-SDR delivers
// unsigned 8-bit pairs, and converting host-side would put a pass over every
// sample back on the CPU and quadruple what crosses the bus: at 20 MS/s that
// is 40 MB/s against 160. Twins in core/dsp/convert.h.
[[nodiscard]] std::span<const std::uint32_t> convert_cu8_cf32();
[[nodiscard]] std::span<const std::uint32_t> convert_cs8_cf32();
[[nodiscard]] std::span<const std::uint32_t> convert_cs16_cf32();

// The per-receiver fine stage: mix by the residual offset, filter, resample.
// Twin of revenant::dsp::reference_vrx_fine.
[[nodiscard]] std::span<const std::uint32_t> vrx_fine();

// The demodulators, one mode selected by a specialization constant so the
// branch folds away at pipeline creation. Twin of
// revenant::dsp::reference_vrx_demod.
[[nodiscard]] std::span<const std::uint32_t> vrx_demod();

}  // namespace revenant::gpu::shaders
