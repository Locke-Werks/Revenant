// Continuous-stream Viterbi decoding of a feedforward convolutional code.
//
// The block decoder in dv_codes.h keeps the whole trellis and traces back once
// at the end, which suits a framed burst. A continuous stream (CCSDS telemetry,
// MFSK16, THOR) has no end to trace back from, so this decoder keeps a fixed
// window of survivor decisions and releases each bit once it is `traceback`
// steps old. The textbook rule of thumb is that survivors merge within about
// five constraint lengths, so the default window is 5K.
//
// Source: textbook theory only (Viterbi 1971; Lin and Costello, "Error Control
// Coding", chapter 12 on truncated survivor memory and on puncturing). The
// CCSDS generators are from CCSDS 131.0-B, section 3.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/error.h"

namespace revenant::decode {

// CCSDS 131.0-B rate 1/2, K = 7: G1 = 171 octal, G2 = 133 octal, written in
// the dv_codes.h convention of D^0 in the least significant bit, which is the
// bit reversal of the octal form. G2's output is inverted on the channel, so a
// caller decoding CCSDS passes kCcsdsK7InvertMask. MFSK16 and THOR use the same
// pair without the inversion.
inline constexpr std::uint32_t kCcsdsK7Memory = 6;
inline constexpr std::uint32_t kCcsdsK7G1 = 0x4F;
inline constexpr std::uint32_t kCcsdsK7G2 = 0x6D;
inline constexpr std::uint32_t kCcsdsK7InvertMask = 0b10;

struct StreamingViterbiConfig {
    std::uint32_t memory = 0;
    std::vector<std::uint32_t> generators;

    // Decided bits leave the decoder this many steps after they went in. Zero
    // picks 5 * (memory + 1).
    std::size_t traceback = 0;

    // Puncturing pattern over the mother code's output bits, in encoder output
    // order and repeating: 1 means the bit is transmitted, 0 means it was
    // deleted. Its length must be a whole number of encoder steps. Empty means
    // no puncturing.
    std::vector<std::uint8_t> puncture;

    // Bit g set means generator g's output is inverted on the channel.
    std::uint32_t invert_mask = 0;
};

class StreamingViterbi {
public:
    [[nodiscard]] static Expected<StreamingViterbi> create(StreamingViterbiConfig config);

    // Feeds received soft values, punctured as transmitted, in the SoftSpan
    // sign convention. Chunks may split anywhere, including inside a step;
    // the output does not depend on where they split. Decided bits are
    // appended to `out`.
    void push(SoftSpan soft, std::vector<std::uint8_t>& out);

    // Releases the bits still inside the traceback window, tracing from state
    // zero when `terminated` (the encoder was flushed) and from the best state
    // otherwise. The decoder is reset afterwards.
    void flush(bool terminated, std::vector<std::uint8_t>& out);

    void reset();

    [[nodiscard]] std::size_t latency() const { return traceback_; }

private:
    StreamingViterbi() = default;
    void step(std::vector<std::uint8_t>& out);
    [[nodiscard]] std::size_t best_state() const;

    std::uint32_t memory_ = 0;
    std::size_t outputs_ = 0;
    std::size_t states_ = 0;
    std::size_t traceback_ = 0;
    std::vector<std::uint8_t> puncture_;
    std::uint32_t invert_mask_ = 0;

    std::vector<std::uint32_t> branch_bits_;  // [state * 2 + bit]

    std::vector<float> metric_;
    std::vector<float> next_metric_;
    // Ring of traceback + 1 steps; per step, per destination state, the top
    // bit of the predecessor the shift threw away.
    std::vector<std::uint8_t> dropped_;
    std::size_t steps_ = 0;

    std::vector<float> pending_;  // one step of depunctured mother-code values
    std::size_t puncture_pos_ = 0;
};

// Deletes the bits a puncturing pattern marks 0. A helper for transmitters and
// tests; the pattern convention is StreamingViterbiConfig::puncture's.
[[nodiscard]] std::vector<std::uint8_t> puncture_bits(ConstBitSpan encoded,
                                                      std::span<const std::uint8_t> pattern);

}  // namespace revenant::decode
