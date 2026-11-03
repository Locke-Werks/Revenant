// Opus encoding for the phone's low-latency live audio path.
//
// Mono only. A chunk with channel_count > 1 is the caller's problem (the
// engine link downmixes before this); interleaved stereo pushed here would
// play at half speed.
//
// Named LiveOpusEncoder because libopus declares its own OpusEncoder at global
// scope; the opus headers stay in the .cpp regardless.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/error.h"

namespace revenant::webgate {

// OPUS_APPLICATION_AUDIO at 32 kbit/s in 960-sample (20 ms) frames. The
// content is radio audio, not speech alone, so the speech-tuned application
// would smear tones and noise.
//
// Not thread safe. Use it from one thread.
class LiveOpusEncoder {
public:
    // Most packets held undrained: 250 is 5 s of audio.
    static constexpr std::size_t kMaxQueuedPackets = 250;

    // Only 48000 Hz is accepted: the frame size is 20 ms at that rate, and a
    // resample here would be silent pitch damage if the caller got it wrong.
    [[nodiscard]] static Expected<std::unique_ptr<LiveOpusEncoder>> create(
        std::uint32_t sample_rate);

    ~LiveOpusEncoder();
    LiveOpusEncoder(const LiveOpusEncoder&) = delete;
    LiveOpusEncoder& operator=(const LiveOpusEncoder&) = delete;

    // Any chunk size. Samples are mono floats; non-finite values become 0 and
    // the rest are clamped to [-1, 1], so a bad sample from the engine cannot
    // poison a frame. A frame is encoded each time 960 samples have
    // accumulated; the remainder waits for more.
    //
    // A frame that fails to encode is dropped, counted in dropped_packets(),
    // and the first reason is kept in error().
    void push(std::span<const float> samples);

    // Packets finished since the last call, one per 20 ms frame. The caller is
    // expected to drain this regularly: past kMaxQueuedPackets the oldest
    // packet is discarded (and counted in dropped_packets()) so a stalled
    // consumer cannot grow memory without bound.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> take_packets();

    // False once any frame has failed to encode; error() says why (the first
    // failure). Later pushes still work.
    [[nodiscard]] bool ok() const;
    [[nodiscard]] const std::string& error() const;

    // Frames lost to an encode failure or to queue overflow.
    [[nodiscard]] std::uint64_t dropped_packets() const;

private:
    struct Impl;
    explicit LiveOpusEncoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace revenant::webgate
