// One mixed audio stream per phone session.
//
// The owner's decision of 2026-11-03: a phone hears what the desktop hears,
// which is every digital voice receiver at once with the one it is listening
// to, not only the one. The desktop does this in ui/audio/audio_mix.h at the
// sound card's rate. This is the phone's version at 48 kHz mono, with the
// desktop's rules reused where they are pure: the digital voice boost
// (voice_gain.h, from ui/audio/voice_gain.h) on each digital voice stream and
// the soft limiter on the sum (ui/audio/mix_stages.h). It feeds the live Opus
// path and the HLS keep listening path alike, so both play the same sound.
//
// WHO IS MIXED is not decided here. The caller names the streams as it pushes
// them, and which receivers it subscribed is one function in PhoneStreams
// (mix_members). A change in who a phone may hear touches that function and
// leaves this file alone.
//
// ALIGNMENT, and why it is simpler than the desktop's. Each stream is a queue
// of 48 kHz samples. Output is cut in 20 ms blocks, and a block is cut when
// the lead stream (the receiver the phone is listening to) holds a whole one.
// Every other stream contributes a whole block when it holds one and silence
// when it does not, and keeps what it has for the next block, so a stream
// running a little behind is delayed rather than chopped into pieces with
// holes. The engine delivers every receiver's audio from one pass, so in
// practice the streams arrive together and the delay is one block.
//
// A STALLED STREAM DOES NOT STALL THE MIX. When the lead has less than a block
// and another stream has kStallBlocks of them waiting, the block is cut anyway
// with the lead as silence. With no lead at all the first stream with a block
// paces it. A stream that falls more than kMaxQueued behind (its audio arrived
// late, in a burst) has the oldest of it dropped rather than played behind the
// others.
//
// SILENCE BETWEEN DIGITAL VOICE CALLS is a stream that sends nothing: it adds
// nothing, and the sum is the lead alone, untouched by the limiter below its
// threshold.
//
// Not thread safe; it lives on the phone's strand.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "audio/mix_stages.h"
#include "tools/webgate/audio_rate.h"
#include "tools/webgate/voice_gain.h"

namespace revenant::webgate {

class PhoneMixer {
public:
    // 20 ms at 48 kHz, one Opus frame.
    static constexpr std::size_t kBlock = kPhoneAudioRate / 50;
    static constexpr std::size_t kStallBlocks = 3;
    // 120 ms.
    static constexpr std::size_t kMaxQueued = 6 * kBlock;

    PhoneMixer() { limiter_.configure(kPhoneAudioRate); }

    // The receiver whose pace the mix follows. Not required to have a stream.
    void set_lead(std::optional<std::uint64_t> vrx) { lead_ = vrx; }

    // Adds mono 48 kHz samples to a receiver's stream. A digital voice stream
    // is boosted by `voice_db` on the way in; any other is added as it is.
    void push(std::uint64_t vrx, bool digital_voice, double voice_db,
              std::span<const float> mono)
    {
        Stream& s = streams_[vrx];
        if (digital_voice) {
            const auto boosted = s.boost.process(voice_db, mono);
            s.queue.insert(s.queue.end(), boosted.begin(), boosted.end());
        } else {
            s.queue.insert(s.queue.end(), mono.begin(), mono.end());
        }
    }

    // A receiver that is gone, or no longer mixed.
    void remove(std::uint64_t vrx) { streams_.erase(vrx); }

    // Every block that can be cut now, summed and limited. The span is good
    // until the next call.
    [[nodiscard]] std::span<const float> take()
    {
        out_.clear();
        // Before the cuts, and never the lead: a long chunk of the lead is not
        // late, it is simply a lot, and is cut in full. Any other stream that
        // has piled up past the window is a burst that would otherwise be
        // played ahead of real time by the stall rule below.
        for (auto& [vrx, s] : streams_) {
            if (lead_ == vrx || s.queue.size() <= kMaxQueued) {
                continue;
            }
            const std::size_t excess = s.queue.size() - kMaxQueued;
            s.queue.erase(s.queue.begin(), s.queue.begin() + static_cast<std::ptrdiff_t>(excess));
            dropped_ += excess;
        }
        while (ready()) {
            cut();
        }
        return out_;
    }

    // Samples dropped for arriving more than kMaxQueued behind, and blocks in
    // which a stream held something but less than a whole block.
    [[nodiscard]] std::uint64_t dropped() const { return dropped_; }
    [[nodiscard]] std::uint64_t padded_blocks() const { return padded_; }
    [[nodiscard]] std::size_t streams() const { return streams_.size(); }

private:
    struct Stream {
        std::deque<float> queue;
        VoiceGainStage boost;
    };

    [[nodiscard]] bool ready() const
    {
        std::size_t most = 0;
        for (const auto& [vrx, s] : streams_) {
            most = std::max(most, s.queue.size());
        }
        const auto lead = lead_ ? streams_.find(*lead_) : streams_.end();
        if (lead != streams_.end()) {
            return lead->second.queue.size() >= kBlock || most >= kStallBlocks * kBlock;
        }
        return most >= kBlock;
    }

    void cut()
    {
        float block[kBlock] = {};
        for (auto& [vrx, s] : streams_) {
            if (s.queue.size() < kBlock) {
                // Something waiting but not a whole block is a stream that is
                // late; nothing waiting is one that is idle, which is not worth
                // counting.
                if (!s.queue.empty()) {
                    ++padded_;
                }
                continue;
            }
            for (std::size_t i = 0; i < kBlock; ++i) {
                block[i] += s.queue[i];
            }
            s.queue.erase(s.queue.begin(), s.queue.begin() + static_cast<std::ptrdiff_t>(kBlock));
        }
        limiter_.process(block, kBlock, 1);
        out_.insert(out_.end(), block, block + kBlock);
    }

    std::map<std::uint64_t, Stream> streams_;
    std::optional<std::uint64_t> lead_;
    ui::SoftLimiter limiter_;
    std::vector<float> out_;
    std::uint64_t dropped_ = 0;
    std::uint64_t padded_ = 0;
};

}  // namespace revenant::webgate
