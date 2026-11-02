// Codec 2 speech, through the linked libcodec2.
//
// LINKED, NOT CLEAN-ROOMED
//
// Codec 2 has no specification: its vector quantiser codebooks are trained
// data that exist only in the library, so a decoder written from the
// algorithm description would not be bit-compatible with any transmitter.
// docs/modes.md, "Digital voice, collected", records the exception, and the
// library is LGPL-2.1, read from the notice at the head of src/codec2.h, which
// section 3 of that licence lets a recipient take under GPL-2.0-or-later.
// libcodec2 1.2.0 arrives from vcpkg-overlays/codec2/, built as the vocoder
// alone. Nothing here was written from libcodec2's source: this file calls the
// public API in codec2.h and every frame size comes back from the library at
// run time rather than being written down here.
//
// WHAT IS WRAPPED
//
// The four modes Revenant's voice protocols carry: 3200 and 1600 for M17
// (M17 Part I, 3.2.1, Table 3.4: data type 10 is voice at 3200 bit/s, 11 is
// voice and data at 1600 bit/s), and 1300 and 700C, which docs/modes.md names
// for FreeDV. Audio is 8 kHz mono, float32 in +-1, the scale every other
// audio surface in the engine uses.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/error.h"

struct CODEC2;

namespace revenant::decode {

enum class Codec2Mode : std::uint8_t {
    Mode3200,
    Mode1600,
    Mode1300,
    Mode700C,
};

// Codec 2 runs at 8 kHz in every mode Revenant uses.
inline constexpr std::uint32_t kCodec2SampleRate = 8000;

class Codec2Voice {
   public:
    [[nodiscard]] static Expected<Codec2Voice> create(Codec2Mode mode);

    Codec2Voice(Codec2Voice&& other) noexcept;
    Codec2Voice& operator=(Codec2Voice&& other) noexcept;
    Codec2Voice(const Codec2Voice&) = delete;
    Codec2Voice& operator=(const Codec2Voice&) = delete;
    ~Codec2Voice();

    [[nodiscard]] Codec2Mode mode() const noexcept { return mode_; }

    // As the library reports them for this mode.
    [[nodiscard]] std::size_t bits_per_frame() const noexcept { return bits_; }
    [[nodiscard]] std::size_t bytes_per_frame() const noexcept { return (bits_ + 7) / 8; }
    [[nodiscard]] std::size_t samples_per_frame() const noexcept { return samples_; }

    // One packed frame, bytes_per_frame() bytes, most significant bit first
    // as the library packs it, to exactly samples_per_frame() samples.
    [[nodiscard]] Status decode(std::span<const std::uint8_t> frame, std::span<float> out);

    // samples_per_frame() samples in +-1 to one packed frame. For the
    // transmitters that make test signals.
    [[nodiscard]] Status encode(std::span<const float> speech, std::span<std::uint8_t> frame);

    // Decodes as many whole frames as `bytes` holds and appends the audio.
    [[nodiscard]] Status decode_all(std::span<const std::uint8_t> bytes, std::vector<float>& out);

   private:
    Codec2Voice() = default;

    CODEC2* state_ = nullptr;
    Codec2Mode mode_ = Codec2Mode::Mode3200;
    std::size_t bits_ = 0;
    std::size_t samples_ = 0;
    std::vector<short> pcm_;
};

}  // namespace revenant::decode
