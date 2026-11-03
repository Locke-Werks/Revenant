// Opus encoding for the phone's low-latency live audio path.
//
// Mono only. A chunk with channel_count > 1 is the caller's problem (the
// engine link downmixes before this); interleaved stereo pushed here would
// play at half speed.
//
// libopus's own encoder type is also called OpusEncoder, at global scope. This
// class lives in revenant::webgate and the opus headers stay in the .cpp, so
// the two never meet in one translation unit's lookup.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/error.h"

namespace revenant::webgate {

// OPUS_APPLICATION_AUDIO at 32 kbit/s in 960-sample (20 ms) frames. The
// content is radio audio, not speech alone, so the speech-tuned application
// would smear tones and noise.
//
// Not thread safe. Use it from one thread.
class OpusEncoder {
public:
    // Only 48000 Hz is accepted: the frame size is 20 ms at that rate, and a
    // resample here would be silent pitch damage if the caller got it wrong.
    [[nodiscard]] static Expected<std::unique_ptr<OpusEncoder>> create(std::uint32_t sample_rate);

    ~OpusEncoder();
    OpusEncoder(const OpusEncoder&) = delete;
    OpusEncoder& operator=(const OpusEncoder&) = delete;

    // Any chunk size. Samples are mono floats in [-1, 1]. A frame is encoded
    // each time 960 samples have accumulated; the remainder waits for more.
    void push(std::span<const float> samples);

    // Packets finished since the last call, one per 20 ms frame.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> take_packets();

private:
    struct Impl;
    explicit OpusEncoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace revenant::webgate
