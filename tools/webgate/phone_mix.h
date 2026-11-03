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
// HOW THE ENGINE DELIVERS VOICE, which decides every number below. A p25p1
// receiver's audio is not an LDU at a time. core/rpc/voice_audio.h holds each
// decoded LDU (9 frames, 180 ms) behind a pre-roll and hands out one chunk per
// receiver chunk, covering exactly that chunk's time at 8000 S/s, zeros between
// calls. So voice arrives continuously in the engine's own cadence, 7 to 27 ms
// a chunk, which resampled to 48 kHz is not a whole number of 20 ms blocks.
// The plugin voice of dmr and dstar travels the same route. The mixer is
// nonetheless built to take a burst of an LDU at once, because a decode lane
// that falls behind and catches up produces exactly that.
//
// TIMELINE. The lead stream, the receiver the phone is listening to, sets the
// pace: a block is cut when the lead holds a whole one, and the lead is never
// skipped. A lead that is one sample short waits for that sample. Every other
// stream is read through a jitter buffer: it contributes nothing until it has
// kPrimeBlocks (200 ms, one LDU) queued, then a whole block per cut, so its
// own chunk sizes cannot put holes in it. It holds up to kMaxQueued (400 ms);
// past that the oldest is dropped. If it does run dry it contributes what it
// has, is counted, and primes again.
//
// STALLS ARE MEASURED IN TIME. A stream that has pushed nothing for kStall
// (200 ms) is stalled: its queue is cleared (a few milliseconds of an old call
// would otherwise play at the start of the next) and it must prime again. When
// the lead is stalled, or there is none, the mix is paced by the wall clock
// instead, one block per 20 ms, so the voice streams are heard and the output
// never runs ahead of real time. The clock is passed in, so a test owns it.
//
// SILENCE BETWEEN DIGITAL VOICE CALLS is a stream of zeros: it adds nothing,
// and the sum is the lead alone, untouched by the limiter below its threshold.
//
// Not thread safe; it lives on the phone's strand.

#pragma once

#include <algorithm>
#include <chrono>
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
    using Clock = std::chrono::steady_clock;

    // 20 ms at 48 kHz, one Opus frame.
    static constexpr std::size_t kBlock = kPhoneAudioRate / 50;
    static constexpr auto kBlockTime = std::chrono::milliseconds(20);
    // One LDU of P25 voice, 180 ms, rounded up to whole blocks.
    static constexpr std::size_t kPrimeBlocks = 10;
    static constexpr std::size_t kMaxQueued = 20 * kBlock;
    static constexpr auto kStall = std::chrono::milliseconds(200);

    PhoneMixer() { limiter_.configure(kPhoneAudioRate); }

    // The receiver whose pace the mix follows. Not required to have a stream.
    void set_lead(std::optional<std::uint64_t> vrx) { lead_ = vrx; }

    // Adds mono 48 kHz samples to a receiver's stream. A digital voice stream
    // is boosted by `voice_db` on the way in; any other is added as it is.
    void push(std::uint64_t vrx, bool digital_voice, double voice_db,
              std::span<const float> mono, Clock::time_point now)
    {
        const auto [it, created] = streams_.try_emplace(vrx);
        Stream& s = it->second;
        if (created) {
            s.last_push = now;
        }
        expire(s, now);
        if (digital_voice) {
            const auto boosted = s.boost.process(voice_db, mono);
            s.queue.insert(s.queue.end(), boosted.begin(), boosted.end());
        } else {
            s.queue.insert(s.queue.end(), mono.begin(), mono.end());
        }
        s.last_push = now;
        if (!s.primed && s.queue.size() >= kPrimeBlocks * kBlock) {
            s.primed = true;
        }
    }

    // A receiver that is gone, or no longer mixed.
    void remove(std::uint64_t vrx) { streams_.erase(vrx); }

    // Every block that can be cut by `now`, summed and limited. The span is
    // good until the next call.
    [[nodiscard]] std::span<const float> take(Clock::time_point now)
    {
        out_.clear();
        for (auto& [vrx, s] : streams_) {
            expire(s, now);
            // Never the lead: a long chunk of the lead is not late, it is
            // simply a lot, and is cut in full.
            if (lead_ != vrx && s.queue.size() > kMaxQueued) {
                const std::size_t excess = s.queue.size() - kMaxQueued;
                s.queue.erase(s.queue.begin(),
                              s.queue.begin() + static_cast<std::ptrdiff_t>(excess));
                dropped_ += excess;
            }
        }
        for (;;) {
            const auto lead = lead_ ? streams_.find(*lead_) : streams_.end();
            if (lead != streams_.end() && now - lead->second.last_push <= kStall) {
                // The lead sets the pace. Short of a block it waits, however
                // much the others hold.
                if (lead->second.queue.size() < kBlock) {
                    break;
                }
                cut();
                wall_mark_ = now;
                continue;
            }
            // No lead audio: the wall clock sets the pace, one block per
            // kBlockTime, from where the lead last left it.
            if (!voice_ready()) {
                break;
            }
            if (!wall_mark_ || now - *wall_mark_ > kPrimeBlocks * kBlockTime) {
                // At most the allowance of catching up, so a long silence does
                // not become a long burst.
                wall_mark_ = now - kBlockTime;
            }
            if (now - *wall_mark_ < kBlockTime) {
                break;
            }
            cut();
            *wall_mark_ += kBlockTime;
        }
        return out_;
    }

    // Samples dropped for arriving more than kMaxQueued behind, and blocks in
    // which a primed stream ran dry.
    [[nodiscard]] std::uint64_t dropped() const { return dropped_; }
    [[nodiscard]] std::uint64_t padded_blocks() const { return padded_; }
    [[nodiscard]] std::size_t streams() const { return streams_.size(); }

private:
    struct Stream {
        std::deque<float> queue;
        VoiceGainStage boost;
        Clock::time_point last_push{};
        bool primed = false;
    };

    // A stalled stream starts again: what it still holds is the tail of
    // something that ended.
    static void expire(Stream& s, Clock::time_point now)
    {
        if (now - s.last_push > kStall && (!s.queue.empty() || s.primed)) {
            s.queue.clear();
            s.primed = false;
        }
    }

    [[nodiscard]] bool voice_ready() const
    {
        return std::ranges::any_of(streams_, [this](const auto& entry) {
            const Stream& s = entry.second;
            return lead_ != entry.first && s.primed && s.queue.size() >= kBlock;
        });
    }

    void cut()
    {
        float block[kBlock] = {};
        for (auto& [vrx, s] : streams_) {
            const bool lead = lead_ == vrx;
            if (!lead && !s.primed) {
                continue;
            }
            const std::size_t have = std::min(s.queue.size(), kBlock);
            for (std::size_t i = 0; i < have; ++i) {
                block[i] += s.queue[i];
            }
            s.queue.erase(s.queue.begin(), s.queue.begin() + static_cast<std::ptrdiff_t>(have));
            if (have < kBlock && !lead) {
                // Ran dry: what it had is played, padded, and it fills again.
                ++padded_;
                s.primed = false;
            }
        }
        limiter_.process(block, kBlock, 1);
        out_.insert(out_.end(), block, block + kBlock);
    }

    std::map<std::uint64_t, Stream> streams_;
    std::optional<std::uint64_t> lead_;
    std::optional<Clock::time_point> wall_mark_;
    ui::SoftLimiter limiter_;
    std::vector<float> out_;
    std::uint64_t dropped_ = 0;
    std::uint64_t padded_ = 0;
};

}  // namespace revenant::webgate
