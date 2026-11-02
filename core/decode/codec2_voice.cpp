// Codec 2 speech through the linked libcodec2. See codec2_voice.h for why
// this is linked rather than written here.

#include "core/decode/codec2_voice.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <codec2/codec2.h>

namespace revenant::decode {

namespace {

// The mode constants are codec2.h's own, named rather than numbered.
int library_mode(Codec2Mode mode) {
    switch (mode) {
        case Codec2Mode::Mode3200:
            return CODEC2_MODE_3200;
        case Codec2Mode::Mode1600:
            return CODEC2_MODE_1600;
        case Codec2Mode::Mode1300:
            return CODEC2_MODE_1300;
        case Codec2Mode::Mode700C:
            return CODEC2_MODE_700C;
    }
    return CODEC2_MODE_3200;
}

// The library speaks 16-bit PCM; the engine speaks float in +-1.
constexpr float kFullScale = 32768.0F;

}  // namespace

Expected<Codec2Voice> Codec2Voice::create(Codec2Mode mode) {
    Codec2Voice voice;
    voice.mode_ = mode;
    voice.state_ = codec2_create(library_mode(mode));
    if (voice.state_ == nullptr) {
        return fail("codec2_create refused mode " + std::to_string(library_mode(mode)) +
                    ": the linked libcodec2 was built without it");
    }
    voice.bits_ = static_cast<std::size_t>(codec2_bits_per_frame(voice.state_));
    voice.samples_ = static_cast<std::size_t>(codec2_samples_per_frame(voice.state_));
    voice.pcm_.resize(voice.samples_);
    return voice;
}

Codec2Voice::Codec2Voice(Codec2Voice&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      mode_(other.mode_),
      bits_(other.bits_),
      samples_(other.samples_),
      pcm_(std::move(other.pcm_)) {}

Codec2Voice& Codec2Voice::operator=(Codec2Voice&& other) noexcept {
    if (this != &other) {
        if (state_ != nullptr) {
            codec2_destroy(state_);
        }
        state_ = std::exchange(other.state_, nullptr);
        mode_ = other.mode_;
        bits_ = other.bits_;
        samples_ = other.samples_;
        pcm_ = std::move(other.pcm_);
    }
    return *this;
}

Codec2Voice::~Codec2Voice() {
    if (state_ != nullptr) {
        codec2_destroy(state_);
    }
}

Status Codec2Voice::decode(std::span<const std::uint8_t> frame, std::span<float> out) {
    if (frame.size() != bytes_per_frame()) {
        return fail("Codec 2 frame is " + std::to_string(frame.size()) + " bytes, mode needs " +
                    std::to_string(bytes_per_frame()));
    }
    if (out.size() != samples_) {
        return fail("Codec 2 output is " + std::to_string(out.size()) + " samples, mode writes " +
                    std::to_string(samples_));
    }
    codec2_decode(state_, pcm_.data(), frame.data());
    std::ranges::transform(pcm_, out.begin(),
                           [](short s) { return static_cast<float>(s) / kFullScale; });
    return {};
}

Status Codec2Voice::encode(std::span<const float> speech, std::span<std::uint8_t> frame) {
    if (speech.size() != samples_ || frame.size() != bytes_per_frame()) {
        return fail("Codec 2 encode wants " + std::to_string(samples_) + " samples into " +
                    std::to_string(bytes_per_frame()) + " bytes");
    }
    std::ranges::transform(speech, pcm_.begin(), [](float x) {
        return static_cast<short>(std::lround(std::clamp(x, -1.0F, 32767.0F / kFullScale) * kFullScale));
    });
    codec2_encode(state_, frame.data(), pcm_.data());
    return {};
}

Status Codec2Voice::decode_all(std::span<const std::uint8_t> bytes, std::vector<float>& out) {
    const std::size_t n = bytes_per_frame();
    for (std::size_t at = 0; at + n <= bytes.size(); at += n) {
        const std::size_t base = out.size();
        out.resize(base + samples_);
        auto status = decode(bytes.subspan(at, n), std::span<float>(out).subspan(base));
        if (!status) {
            out.resize(base);
            return status;
        }
    }
    return {};
}

}  // namespace revenant::decode
