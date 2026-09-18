// CPU twin of core/shaders/pfb_branch.comp, the polyphase branch filter.
//
// Bit-identical to the kernel, not close to it. The twin transcribes the
// kernel's operation order rather than its mathematical result: the same
// ascending tap index q, the same accumulate-then-multiply-add shape, the same
// output slot. Reordering a 16-tap accumulation moves the result by up to
// 110592 ULP for 2.4e-07 of absolute error, so "computes the same sum" is not
// the same claim and would not catch a kernel that had drifted.
//
// It lives in core/ rather than tests/ because a reference in the test tree is
// a reference nobody ships, reviews or keeps current. See
// docs/conventions.md, "Reference implementations".

#pragma once

#include <cstdint>

#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::dsp {

// The three values core/shaders/pfb_branch.comp takes as push constants, in
// the order it declares them. The host builds one of these and both sides read
// the same numbers, so a mismatch between the kernel's view of the ring and the
// twin's view is a compile error rather than a silent divergence.
//
// Three tightly packed uint32 is the scalar push-constant layout of the
// shader's Params block, so this struct can be handed to a dispatch as bytes
// with no repacking. The static assert below is what keeps that true.
struct PfbBranchParams {
    // Input ring capacity minus one. The capacity is a power of two, which is
    // what makes the wrap exact: the kernel computes (base - n) & ring_mask,
    // the subtraction wraps modulo 2^32, and a power-of-two capacity divides
    // 2^32, so the mask names the sample the unwrapped arithmetic would have.
    std::uint32_t ring_mask = 0;

    // Ring offset of x[mD] for output block 0.
    //
    // A ring offset and not a SampleIndex. SampleIndex is uint64 and GLSL has
    // no 64-bit integer in this kernel, so the host owns the absolute index and
    // passes down the 32-bit offset. The twin takes the same 32-bit value for
    // the same reason it takes the same taps: so that it is exercising the
    // arithmetic the device will do, including the wrap.
    std::uint32_t base_offset = 0;

    // Number of output blocks this dispatch produces. Block b is centred on
    // input x[base_offset + b*D].
    std::uint32_t block_count = 0;
};

static_assert(sizeof(PfbBranchParams) == 3 * sizeof(std::uint32_t),
              "PfbBranchParams must be three packed uint32 to alias the kernel's push "
              "constant block");

// Twin of core/shaders/pfb_branch.comp.
//
// prototype_taps is grid.prototype_length() real taps, exactly the float array
// uploaded to the device: the twin never designs the filter itself, so the
// coefficients are identical by construction rather than by agreement.
//
// iq_ring is the input ring, at least params.ring_mask + 1 samples long, and
// indexed exactly as the kernel indexes it.
//
// branch_output receives params.block_count * grid.channels values, block-major
// as block*M + slot, with slot = (M - r) mod M. That reversal is what lets the
// next stage be an ordinary forward FFT; see the kernel's header comment for
// the derivation. The twin writes the same slot order for the same reason the
// kernel does, because that ordering is the interface to the FFT stage.
[[nodiscard]] Status reference_pfb_branches(const GridParams& grid,
                                            ConstRealSpan prototype_taps,
                                            ConstComplexSpan iq_ring,
                                            const PfbBranchParams& params,
                                            ComplexSpan branch_output);

}  // namespace revenant::dsp
