// CPU twins of the three format-conversion kernels.
//
// core/shaders/convert_cu8_cf32.comp, convert_cs8_cf32.comp and
// convert_cs16_cf32.comp widen a source's native sample width into the ring's
// canonical Complex32 during upload, so the native bytes cross the bus exactly
// once and the host never makes a pass over every sample. These are the
// referees for those three kernels: bit-identical output for identical input,
// not merely the same mathematical result.
//
// Being bit-identical is easier here than it is for a filter, and the reason
// is worth stating because it constrains what the kernels are allowed to do.
// Vulkan requires OpFMul to be correctly rounded and permits OpFDiv 2.5 ULP of
// error, so a division in a kernel is not reproducible across vendors and no
// CPU twin could referee it. All three kernels therefore multiply by a float
// constant, and the constants below are those same floats, asserted against
// the exact words the SPIR-V modules carry. Two implementations performing one
// correctly-rounded multiply by the same float agree by construction.
//
// These live in core/ rather than tests/ because a reference in the test tree
// is a reference nobody ships, reviews or keeps current. See
// docs/conventions.md, "Reference implementations".

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::dsp {

// cu8 is offset binary and its zero sits between codes 127 and 128, not on a
// code. Biasing by 127.5 centres the range so code 0 and code 255 map to
// exactly -1 and +1 and the table is antisymmetric, which is what makes the
// mean of a uniform code stream exactly zero. Biasing by 128 instead leaves
// half an LSB of DC, and DC at complex baseband is a spur at the centre of the
// spectrum, at -48.2 dBFS, that looks exactly like a carrier on the tuned
// frequency. 255/2 needs nine significand bits, so every subtraction by this
// value is exact.
inline constexpr float kCu8Bias = 127.5F;

// The float nearest 2/255, which is 1/127.5. Not exactly representable. That
// costs a departure from the exact ratio, not any disagreement with the
// kernel: both sides multiply by this same float. Measured over all 256
// codes, 126 land one step from the correctly-rounded ideal, worst case
// 7.41e-08 absolute, 1.25 ULP at that code's own magnitude and 0.62 ULP
// against full scale.
//
// The value is forced rather than chosen. It is the only one of the three
// adjacent floats for which both endpoints land on exactly +/-1.0; one ULP low
// gives 0.99999994 and one ULP high gives 1.00000012.
inline constexpr float kCu8Scale = 0.00784313772F;

// 2^-7 and 2^-15, both exactly representable, so the signed conversions round
// nowhere at all: every one of the 256 cs8 codes and every one of the 65536
// cs16 codes converts exactly.
//
// Scaling by the full negative code rather than the full positive one is a
// decision, not an oversight. Two's complement has one more negative code than
// positive, and no scaling removes that. Dividing by 127 or 32767 would put
// +/-full-scale-positive on exactly +/-1.0, but sends the most negative code to
// -1.00787401 and -1.00003052 respectively, outside the unit interval that
// every headroom calculation downstream assumes, and would round on every
// sample. The kernels' header comments carry the full argument.
inline constexpr float kCs8Scale = 0.0078125F;
inline constexpr float kCs16Scale = 0.000030517578125F;

// The exact words the three SPIR-V modules carry, so a mistyped digit in
// either this file or a .comp is a compile error here rather than a
// bit-exactness failure in CI with no obvious cause. Verified against the
// OpConstant operands emitted by glslangValidator for target-env vulkan1.3.
static_assert(std::bit_cast<std::uint32_t>(kCu8Bias) == 0x42FF0000U,
              "kCu8Bias must be the float in convert_cu8_cf32.comp");
static_assert(std::bit_cast<std::uint32_t>(kCu8Scale) == 0x3C008081U,
              "kCu8Scale must be the float in convert_cu8_cf32.comp");
static_assert(std::bit_cast<std::uint32_t>(kCs8Scale) == 0x3C000000U,
              "kCs8Scale must be the float in convert_cs8_cf32.comp");
static_assert(std::bit_cast<std::uint32_t>(kCs16Scale) == 0x38000000U,
              "kCs16Scale must be the float in convert_cs16_cf32.comp");

// The endpoint properties the conversions are chosen for, checked rather than
// asserted in prose. Constant evaluation is correctly rounded IEEE, so these
// are the same operations the twins perform at runtime under
// core/dsp/reference_fp.h.
static_assert((0.0F - kCu8Bias) * kCu8Scale == -1.0F, "cu8 code 0 must map to exactly -1");
static_assert((255.0F - kCu8Bias) * kCu8Scale == 1.0F, "cu8 code 255 must map to exactly +1");
static_assert(-128.0F * kCs8Scale == -1.0F, "cs8 code -128 must map to exactly -1");
static_assert(127.0F * kCs8Scale == 0.9921875F, "cs8 full scale positive is one LSB short");
static_assert(-32768.0F * kCs16Scale == -1.0F, "cs16 code -32768 must map to exactly -1");
static_assert(32767.0F * kCs16Scale == 0.999969482421875F,
              "cs16 full scale positive is one LSB short");

// The four values every convert kernel takes as push constants, in the order
// it declares them. The host builds one of these and both sides read the same
// numbers, so a mismatch between the kernel's view of the ring and the twin's
// is a compile error rather than a silent divergence.
//
// Four tightly packed uint32 is the scalar push-constant layout of the
// shaders' Params block, so this struct can be handed to a dispatch as bytes
// with no repacking. The static assert below is what keeps that true.
struct ConvertParams {
    // Ring capacity minus one. The capacity is a power of two, which is what
    // makes the wrap exact: the kernel computes (dst_offset + i) & ring_mask,
    // the addition wraps modulo 2^32, and a power-of-two capacity divides
    // 2^32, so the mask names the slot the unwrapped arithmetic would have.
    std::uint32_t ring_mask = 0;

    // Where converted sample 0 goes. A ring offset, or equivalently the low 32
    // bits of its absolute sample index; under the mask they are the same
    // value. A ring offset and not a SampleIndex because SampleIndex is uint64
    // and the kernels have no 64-bit integer, so the host owns the absolute
    // index. core/shaders/pfb_branch.comp makes the same trade.
    std::uint32_t dst_offset = 0;

    // Index of the first packed sample in the source buffer, IN SAMPLES and
    // not in bytes. A block rarely starts at the front of a reused staging
    // buffer, and binding the buffer at a byte offset instead is not available
    // on an arbitrary sample boundary: minStorageBufferOffsetAlignment is 16
    // bytes on the discrete card here.
    std::uint32_t src_offset = 0;

    // Samples to convert. One invocation each.
    std::uint32_t count = 0;
};

static_assert(sizeof(ConvertParams) == 4 * sizeof(std::uint32_t),
              "ConvertParams must be four packed uint32 to alias the kernels' push constant "
              "block");

// One code pair's worth of the conversion, which is the whole arithmetic of
// each kernel and the single definition of it on the host side.
//
// Defined in core/dsp/convert.cpp rather than inline here on purpose. A
// subtract feeding a multiply is exactly the shape a compiler contracts into
// an FMA, and inlining it would expose that shape to whatever floating-point
// settings the calling translation unit happens to carry. Keeping the bodies
// in a translation unit that includes core/dsp/reference_fp.h first means the
// arithmetic is performed under the project's discipline wherever it is
// called from.
[[nodiscard]] float cu8_to_float(std::uint8_t code);
[[nodiscard]] float cs8_to_float(std::int8_t code);
[[nodiscard]] float cs16_to_float(std::int16_t code);

// Twins of the three kernels.
//
// packed is the staging buffer exactly as the device sees it: the source's
// native bytes, little-endian, indexed from its own start and not from the
// block's. src_offset says where in it the block begins.
//
// The twins read it the way the kernels do, assembling 32-bit words and
// extracting fields, rather than reading bytes directly. The result is the
// same on any machine this project builds on, and stating the assembly makes
// the endianness assumption visible instead of implicit. It also means the
// twins reject exactly what would be an out-of-bounds read on the device: the
// 8-bit formats pack two samples per word, so an odd final sample leaves half
// a word of padding that the kernel still loads, and a buffer sized to exactly
// the sample bytes is two bytes short of it.
//
// ring is the destination, at least ring_mask + 1 samples long. The twins
// write at masked offsets and let the 32-bit addition wrap, so they exercise
// the same wrap arithmetic the kernels do rather than the index a wider
// calculation would have produced. Samples outside the written window are left
// alone.
//
// count greater than the ring capacity is rejected. Both sides would alias the
// window onto itself identically, so a diff would pass while both were wrong.
[[nodiscard]] Status reference_convert_cu8_cf32(std::span<const std::byte> packed,
                                                const ConvertParams& params,
                                                ComplexSpan ring);

[[nodiscard]] Status reference_convert_cs8_cf32(std::span<const std::byte> packed,
                                                const ConvertParams& params,
                                                ComplexSpan ring);

[[nodiscard]] Status reference_convert_cs16_cf32(std::span<const std::byte> packed,
                                                 const ConvertParams& params,
                                                 ComplexSpan ring);

}  // namespace revenant::dsp
