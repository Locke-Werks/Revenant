// The vocoder seam.
//
// One interface, several implementations, none of which knows about the
// others. Revenant needs at least three: a software IMBE for P25 Phase 1,
// Codec 2 for M17 and FreeDV, and whatever a loaded plugin turns out to be,
// which is how a codec with no published algorithm reaches audio at all. See
// docs/modes.md for which modes sit behind which of those.
//
// WHY THERE IS NOT A SINGLE CODEC CONSTANT IN THIS FILE
//
// Clean room, per docs/clean-room.md: every constant in this tree names the
// document and clause it came from. A frame size written here would have no
// citation to give, because this file was written from no codec's
// specification and its author has opened none of them. So the frame shape is
// data, not a constant: an implementation declares its own, having read its
// own document, and cites it there. VocoderFrame is the shape of that
// declaration and nothing in it is a number this file chose.
//
// The same reasoning is why there is no table here mapping a mode to a frame
// size. That table is a set of specification figures wearing a switch
// statement.
//
// WHAT AN IMPLEMENTATION OWES
//
// shape() before anything else, and it does not change over the life of the
// object. decode() either writes exactly shape().pcm_frames samples and
// returns success, or writes nothing and returns an error that says which
// number was wrong. Anything in between is the failure this project spent a
// day removing: output that looks like audio and is not.
//
// A refusal names the number, the cause and the fix. check_vocoder_call below
// writes the three commonest of those refusals once, so every implementation
// rejects a malformed call in the same words.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// VocoderKind
// ---------------------------------------------------------------------------

// What family an implementation belongs to, for the operator and the log.
// Nothing dispatches on it: a caller that needs a particular decoder matches
// on the whole VocoderFrame, because two members of one family with different
// frame sizes are not interchangeable and the family name alone would say they
// were.
//
// kExternal is every codec Revenant does not implement, reached through a
// plugin. The plugin's name() says what it actually is. Revenant does not
// classify a codec it cannot decode, and an enumerator per proprietary codec
// would be Revenant asserting a taxonomy it has no way to check.
//
// Switches over this enum write no default, per docs/conventions.md, so a
// fourth family arrives as a diagnostic at every site rather than as a silent
// fallthrough at one of them.
enum class VocoderKind : std::uint8_t {
    Imbe,
    Codec2,
    External,
};

[[nodiscard]] std::string_view vocoder_kind_name(VocoderKind kind) noexcept;

// ---------------------------------------------------------------------------
// VocoderFrame
// ---------------------------------------------------------------------------

// What one compressed frame looks like, declared by the implementation.
//
// bit_count is bits in, one per byte. pcm_frames is samples out. Both are
// exact: a decoder is a fixed-size block operation and a caller that has
// half a frame has nothing to decode yet.
struct VocoderFrame {
    VocoderKind kind = VocoderKind::External;

    // Bits in one compressed frame, as the implementation takes them. Whether
    // that is before or after forward error correction is the
    // implementation's choice and belongs in its name().
    std::uint32_t bit_count = 0;

    // Audio samples one frame produces.
    std::uint32_t pcm_frames = 0;

    // Output sample rate in whole hertz, per the integer-hertz rule in
    // docs/conventions.md.
    std::uint32_t sample_rate = 0;

    [[nodiscard]] friend bool operator==(const VocoderFrame&, const VocoderFrame&) = default;
};

// "<kind>, <n> bits in, <m> samples out at <r> Hz". One line, for a log or a
// refusal, so two frame shapes that do not match can be printed side by side
// and the difference read off. The numbers come from the argument; this file
// still supplies none of its own.
[[nodiscard]] std::string describe_vocoder_frame(const VocoderFrame& frame);

// ---------------------------------------------------------------------------
// Vocoder
// ---------------------------------------------------------------------------

class Vocoder {
public:
    virtual ~Vocoder() = default;

    Vocoder() = default;
    Vocoder(const Vocoder&) = delete;
    Vocoder& operator=(const Vocoder&) = delete;
    Vocoder(Vocoder&&) = delete;
    Vocoder& operator=(Vocoder&&) = delete;

    // Fixed for the life of the object.
    [[nodiscard]] virtual VocoderFrame shape() const noexcept = 0;

    // bits are one per byte, 0 or 1, in transmission order.
    // out is caller-allocated with at least shape().pcm_frames.
    [[nodiscard]] virtual Status decode(std::span<const std::uint8_t> bits,
                                        std::span<float> out) = 0;

    virtual void reset() = 0;

    // What this is, for a person. Stable for the life of the object.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

// The three refusals every implementation of decode() owes, written once.
//
// Call it first in decode() and return its error unchanged. It checks that
// bits.size() is exactly shape.bit_count, that every byte is 0 or 1, and that
// out.size() is at least shape.pcm_frames, and each message names the number
// it got, the number it wanted and what to do about it.
//
// The byte check is not pedantry. "One bit per byte" is the seam's only
// unenforced clause, and a caller that packs eight bits into a byte and passes
// bit_count/8 bytes fails the size check loudly, while a caller that passes
// 0x00 and 0xFF per bit passes the size check and decodes noise. The first
// mistake is free to find and the second is not.
[[nodiscard]] Status check_vocoder_call(const VocoderFrame& shape,
                                        std::span<const std::uint8_t> bits,
                                        std::span<float> out);

}  // namespace revenant::decode
