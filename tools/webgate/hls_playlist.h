// The HLS side of the "keep listening" path: AAC access units wrapped in
// fragmented mp4 by hand, and the live playlist that lists them.
//
// iOS Safari keeps playing an HLS <audio> with the phone locked, which a
// WebSocket-fed Web Audio graph does not survive. One segmenter per receiver.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/error.h"

namespace revenant::webgate {

class AacEncoder;

// Not thread safe. push() and the readers must run on the same thread, which
// in the gateway is the kj event loop; the spans it returns point into its
// storage and are good until the next push().
class HlsSegmenter {
public:
    // sample_rate must be one the AAC encoder offers (44100 or 48000). A
    // segment holds whole 1024-sample AAC frames, because a frame cannot be
    // split across segments, and the count is rounded down so a segment is
    // never longer than the target: 1 s gives 46 frames (0.981 s at 48 kHz),
    // where 47 would be 1.003 s and overrun a TARGETDURATION of 1.
    explicit HlsSegmenter(std::uint32_t sample_rate, double segment_seconds = 1.0,
                          std::size_t window = 6);
    ~HlsSegmenter();
    HlsSegmenter(const HlsSegmenter&) = delete;
    HlsSegmenter& operator=(const HlsSegmenter&) = delete;

    // False when the encoder could not be created or a push to it later
    // failed; error() says why. A failed segmenter accepts push() and
    // produces nothing more.
    [[nodiscard]] bool ok() const { return encoder_ != nullptr && !failed_; }
    [[nodiscard]] const std::string& error() const { return error_; }

    // Test seam: replaces the encoder's push so a mid-stream failure can be
    // injected. Production code never sets it.
    void set_push_override_for_test(std::function<Status(std::span<const float>)> fn) {
        push_override_ = std::move(fn);
    }

    // Mono float samples in [-1, 1], any chunk size.
    void push(std::span<const float> samples);

    // A segment still inside the sliding window, by media sequence number.
    [[nodiscard]] std::optional<std::span<const std::uint8_t>> segment(
        std::uint64_t sequence) const;

    // ftyp + moov, the target of #EXT-X-MAP.
    [[nodiscard]] std::span<const std::uint8_t> init_segment() const { return init_; }

    // The live media playlist. Empty until the first segment completes: a
    // playlist with no segments is not valid HLS, so there is nothing to serve.
    [[nodiscard]] std::string playlist() const;

private:
    struct Segment {
        std::uint64_t sequence = 0;
        double seconds = 0.0;
        std::vector<std::uint8_t> bytes;
    };

    void cut_segment();

    std::uint32_t sample_rate_;
    std::size_t frames_per_segment_;
    std::size_t window_;
    std::string error_;
    bool failed_ = false;
    std::function<Status(std::span<const float>)> push_override_;
    std::unique_ptr<AacEncoder> encoder_;
    std::vector<std::uint8_t> init_;
    std::vector<std::vector<std::uint8_t>> pending_;
    std::deque<Segment> segments_;
    std::uint64_t next_sequence_ = 0;
    // Total AAC frames already placed in segments, which is the media timeline.
    std::uint64_t frames_emitted_ = 0;
};

// Which segmenter answers /hls/<vrx>/... Entries are borrowed: the owner
// removes a segmenter before destroying it. Same single-thread rule as
// HlsSegmenter.
class HlsRegistry {
public:
    void set(std::uint64_t vrx, HlsSegmenter* segmenter) { map_[vrx] = segmenter; }
    void remove(std::uint64_t vrx) { map_.erase(vrx); }
    [[nodiscard]] HlsSegmenter* find(std::uint64_t vrx) const {
        const auto it = map_.find(vrx);
        return it == map_.end() ? nullptr : it->second;
    }

    // Called by the HTTP side for every playlist or segment request it
    // answers, so a session whose phone is locked knows its stream is still
    // being listened to.
    std::function<void(std::uint64_t)> on_fetch;
    void fetched(std::uint64_t vrx) const {
        if (on_fetch) {
            on_fetch(vrx);
        }
    }

private:
    std::map<std::uint64_t, HlsSegmenter*> map_;
};

}  // namespace revenant::webgate
