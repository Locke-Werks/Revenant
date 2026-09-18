// This include must come first. It carries the pragma that disables
// floating-point contraction for this translation unit, and a pragma that
// appears after a function definition does not apply to it.
#include "core/dsp/reference_fp.h"

#include "core/dsp/convert.h"

#include <format>
#include <string_view>

#include "core/dsp/denormal_mode.h"

namespace revenant::dsp {
namespace {

static_assert(kReferenceFpDisciplineApplied,
              "core/dsp/reference_fp.h must be included before anything else in a reference "
              "implementation");

// The kernels read their source as an array of uint, because 8-bit and 16-bit
// storage in a shader need VK_KHR_8bit_storage or VK_KHR_16bit_storage and
// their matching features, which core/gpu/context.cpp does not request.
constexpr std::uint64_t kBytesPerWord = 4;

// Assembles the word the device loads, least significant byte first.
//
// Every implementation Vulkan runs on is little-endian, so this is a memcpy on
// every machine the project targets. It is written out rather than
// reinterpret_cast so that the ordering is a statement in the file instead of
// a property of whatever host the twin last ran on.
[[nodiscard]] std::uint32_t load_word(std::span<const std::byte> packed,
                                      std::uint32_t word_index) {
    const std::size_t base = static_cast<std::size_t>(word_index) * kBytesPerWord;
    return static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(packed[base])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(packed[base + 1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(packed[base + 2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(packed[base + 3])) << 24);
}

// GLSL bitfieldExtract on an unsigned operand: the field is zero extended.
[[nodiscard]] constexpr std::uint32_t extract_unsigned(std::uint32_t word, int offset,
                                                       int bits) {
    const std::uint32_t mask = (bits == 32) ? 0xFFFFFFFFU : ((1U << bits) - 1U);
    return (word >> offset) & mask;
}

// GLSL bitfieldExtract on a signed operand: the field's most significant bit
// is replicated, which is the sign extension two's complement needs. The
// unsigned overload would turn every negative sample into a large positive
// one, which is the single mistake this conversion invites.
//
// Left shift then arithmetic right shift. Both steps are well defined in
// C++20 and later: conversion to a signed type is modular two's complement,
// and right shift of a negative value is arithmetic.
[[nodiscard]] constexpr std::int32_t extract_signed(std::uint32_t word, int offset, int bits) {
    const int spare = 32 - offset - bits;
    const auto raised = static_cast<std::int32_t>(word << spare);
    return raised >> (32 - bits);
}

// The checks every twin shares. They exist because both sides of a bit-exact
// comparison can be wrong in the same way: a ring smaller than the window
// aliases the window onto itself, and the kernel would do it identically, so
// the diff would pass while neither side read what the caller meant.
[[nodiscard]] Status check_request(std::string_view who, std::span<const std::byte> packed,
                                   const ConvertParams& params, ComplexSpan ring,
                                   std::uint64_t bytes_per_sample) {
    const std::uint64_t capacity = static_cast<std::uint64_t>(params.ring_mask) + 1U;
    if ((capacity & static_cast<std::uint64_t>(params.ring_mask)) != 0U) {
        return fail(std::format("{} ring_mask {} is not a power of two minus one", who,
                                params.ring_mask));
    }

    if (ring.size() < capacity) {
        return fail(std::format("{} ring holds {} samples, mask {} implies a capacity of {}",
                                who, ring.size(), params.ring_mask, capacity));
    }

    if (params.count > capacity) {
        return fail(std::format("{} asked to convert {} samples into a ring of {}; the window "
                                "would alias onto itself",
                                who, params.count, capacity));
    }

    // The kernel computes src_offset + index in 32 bits and would wrap back to
    // the front of the buffer. Nothing legitimate does that, so say so rather
    // than reproducing it.
    const std::uint64_t last_sample =
        static_cast<std::uint64_t>(params.src_offset) + params.count;
    if (last_sample > 0xFFFFFFFFULL) {
        return fail(std::format("{} src_offset {} plus count {} overflows the 32-bit sample "
                                "index the kernel uses",
                                who, params.src_offset, params.count));
    }

    // Whole words, because that is the granularity the device loads at. For
    // the 8-bit formats an odd final sample leaves half a word of padding that
    // the kernel still reads, so a buffer sized to exactly the sample bytes is
    // two bytes short of what the dispatch touches. Rejecting it here is the
    // only place that hazard is visible before a driver decides what an
    // out-of-bounds storage read returns.
    const std::uint64_t bytes_needed = last_sample * bytes_per_sample;
    const std::uint64_t words_needed = (bytes_needed + kBytesPerWord - 1U) / kBytesPerWord;
    const std::uint64_t words_bound = words_needed * kBytesPerWord;
    if (static_cast<std::uint64_t>(packed.size()) < words_bound) {
        return fail(std::format("{} reads {} whole words ({} bytes) for {} samples at offset "
                                "{}, source buffer holds {} bytes",
                                who, words_needed, words_bound, params.count, params.src_offset,
                                packed.size()));
    }

    return {};
}

}  // namespace

float cu8_to_float(std::uint8_t code) {
    // Exactly the kernel's two operations in the kernel's order: subtract the
    // bias, then multiply by the scale. The subtraction is exact for all 256
    // codes, since every result is a multiple of 0.5 below 128 in magnitude,
    // so the multiply is the only rounding in the conversion and it is
    // correctly rounded on both sides.
    return (static_cast<float>(code) - kCu8Bias) * kCu8Scale;
}

float cs8_to_float(std::int8_t code) {
    // One multiply by 2^-7, which is exact for every code. There is no bias:
    // a signed format's zero is code 0, so the conversion adds no DC and code
    // 0 gives +0.0 rather than -0.0.
    return static_cast<float>(code) * kCs8Scale;
}

float cs16_to_float(std::int16_t code) {
    // One multiply by 2^-15, exact for every code, for the same reasons.
    return static_cast<float>(code) * kCs16Scale;
}

Status reference_convert_cu8_cf32(std::span<const std::byte> packed,
                                  const ConvertParams& params, ComplexSpan ring) {
    if (const auto ok = check_request("reference_convert_cu8_cf32", packed, params, ring, 2);
        !ok) {
        return ok;
    }

    if (params.count == 0) {
        return {};
    }

    // No input here can produce a denormal: the smallest magnitude any cu8
    // code converts to is 0.5 * kCu8Scale, about 3.9e-03. The guard is the
    // project's policy for a reference all the same, and it is what makes the
    // twin give the same answer whatever mode its caller was in. See
    // core/dsp/denormal_mode.h.
    const ScopedDenormalFlush flush_denormals;

    for (std::uint32_t index = 0; index < params.count; ++index) {
        // Deliberately 32-bit. The kernel has no 64-bit integer, so this is
        // the arithmetic the device performs.
        const std::uint32_t src_index = params.src_offset + index;

        // Two bytes per sample and four per word, so a sample's I/Q pair never
        // straddles a word boundary and one load serves both. An odd sample
        // sits in the upper half of its word.
        const std::uint32_t word = load_word(packed, src_index >> 1U);
        const int shift = static_cast<int>((src_index & 1U) << 4U);

        const std::uint32_t i_code = extract_unsigned(word, shift, 8);
        const std::uint32_t q_code = extract_unsigned(word, shift + 8, 8);

        const float i_value = cu8_to_float(static_cast<std::uint8_t>(i_code));
        const float q_value = cu8_to_float(static_cast<std::uint8_t>(q_code));

        // The addition is allowed to wrap modulo 2^32, exactly as the kernel's
        // does. The capacity is a power of two dividing 2^32, so masking the
        // wrapped value lands on the slot the unwrapped arithmetic names.
        const std::size_t slot =
            static_cast<std::size_t>((params.dst_offset + index) & params.ring_mask);
        ring[slot] = Complex32{i_value, q_value};
    }

    return {};
}

Status reference_convert_cs8_cf32(std::span<const std::byte> packed,
                                  const ConvertParams& params, ComplexSpan ring) {
    if (const auto ok = check_request("reference_convert_cs8_cf32", packed, params, ring, 2);
        !ok) {
        return ok;
    }

    if (params.count == 0) {
        return {};
    }

    // The smallest nonzero magnitude here is kCs8Scale itself, 7.8e-03, so no
    // denormal is reachable. The guard is the project's policy for a
    // reference and pins the thread's mode regardless of the caller's.
    const ScopedDenormalFlush flush_denormals;

    for (std::uint32_t index = 0; index < params.count; ++index) {
        const std::uint32_t src_index = params.src_offset + index;

        const std::uint32_t word = load_word(packed, src_index >> 1U);
        const int shift = static_cast<int>((src_index & 1U) << 4U);

        const std::int32_t i_code = extract_signed(word, shift, 8);
        const std::int32_t q_code = extract_signed(word, shift + 8, 8);

        const float i_value = cs8_to_float(static_cast<std::int8_t>(i_code));
        const float q_value = cs8_to_float(static_cast<std::int8_t>(q_code));

        const std::size_t slot =
            static_cast<std::size_t>((params.dst_offset + index) & params.ring_mask);
        ring[slot] = Complex32{i_value, q_value};
    }

    return {};
}

Status reference_convert_cs16_cf32(std::span<const std::byte> packed,
                                   const ConvertParams& params, ComplexSpan ring) {
    if (const auto ok = check_request("reference_convert_cs16_cf32", packed, params, ring, 4);
        !ok) {
        return ok;
    }

    if (params.count == 0) {
        return {};
    }

    // The smallest nonzero magnitude here is kCs16Scale, 3.1e-05, so no
    // denormal is reachable. The guard is the project's policy for a
    // reference and pins the thread's mode regardless of the caller's.
    const ScopedDenormalFlush flush_denormals;

    for (std::uint32_t index = 0; index < params.count; ++index) {
        // Four bytes per sample and four per word, so one sample is exactly
        // one word and there is no trailing partial word, unlike the two
        // 8-bit formats.
        const std::uint32_t word = load_word(packed, params.src_offset + index);

        const std::int32_t i_code = extract_signed(word, 0, 16);
        const std::int32_t q_code = extract_signed(word, 16, 16);

        const float i_value = cs16_to_float(static_cast<std::int16_t>(i_code));
        const float q_value = cs16_to_float(static_cast<std::int16_t>(q_code));

        const std::size_t slot =
            static_cast<std::size_t>((params.dst_offset + index) & params.ring_mask);
        ring[slot] = Complex32{i_value, q_value};
    }

    return {};
}

}  // namespace revenant::dsp
