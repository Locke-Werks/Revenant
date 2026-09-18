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

}  // namespace revenant::gpu::shaders
