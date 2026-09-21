// The seam's shared parts: a name per family, a one-line frame description,
// and the argument check every decode() owes its caller.
//
// core/decode/vocoder.h says why there is no codec constant here either.

#include "core/decode/vocoder.h"

#include <cstddef>
#include <format>

namespace revenant::decode {

std::string_view vocoder_kind_name(VocoderKind kind) noexcept
{
    // No default label, per docs/conventions.md, so a fourth family raises
    // C4062 here rather than printing "unknown" for the rest of time.
    switch (kind) {
        case VocoderKind::Imbe:
            return "imbe";
        case VocoderKind::Codec2:
            return "codec2";
        case VocoderKind::External:
            return "external";
    }
    return "invalid";
}

std::string describe_vocoder_frame(const VocoderFrame& frame)
{
    return std::format("{}, {} bits in, {} samples out at {} Hz",
                       vocoder_kind_name(frame.kind),
                       frame.bit_count,
                       frame.pcm_frames,
                       frame.sample_rate);
}

Status check_vocoder_call(const VocoderFrame& shape,
                          std::span<const std::uint8_t> bits,
                          std::span<float> out)
{
    if (bits.size() != shape.bit_count) {
        return fail(std::format(
            "vocoder decode was given {} bits and this decoder takes exactly {} "
            "({}); hand it one whole frame, one bit per byte, unpacked",
            bits.size(),
            shape.bit_count,
            describe_vocoder_frame(shape)));
    }

    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i] > 1u) {
            // Named to the byte, because the usual cause is a caller that
            // packed its bits and the index says where its first non-boolean
            // byte landed, which is the fastest route to which layer did it.
            return fail(std::format(
                "vocoder decode was given {} at bit index {}, and every byte must be 0 or 1; "
                "the caller is passing packed bits or a soft decision, and this seam takes "
                "one hard bit per byte in transmission order",
                static_cast<unsigned>(bits[i]),
                i));
        }
    }

    if (out.size() < shape.pcm_frames) {
        return fail(std::format(
            "vocoder decode was given room for {} samples and one frame is {} "
            "({}); enlarge the output buffer to at least one whole frame",
            out.size(),
            shape.pcm_frames,
            describe_vocoder_frame(shape)));
    }

    return {};
}

}  // namespace revenant::decode
