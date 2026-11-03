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
// calls. So voice arrives continuously in the engine's own cadence, which
// resampled to 48 kHz is not a whole number of 20 ms blocks. The plugin voice
// of dmr and dstar travels the same route. The mixer is nonetheless built to
// take a burst of an LDU at once, because a decode lane that falls behind and
// catches up produces exactly that.
//
// A CHUNK IS AS LONG AS THE SOURCE MAKES IT. The engine cuts receiver chunks
// by sample count, so on a 65536-sample block a 2.4 MS/s radio gives 27 ms and
// a 96 kS/s recording gives 683 ms, one chunk a second and a half. Every
// number that is a time here is therefore a floor, and each stream raises it
// from the longest chunk it has pushed, the way core/rpc/voice_audio.h sizes
// its pre-roll from the longest chunk: a chunk is due about its own length
// after the last, so no gap shorter than that is a stall.
//
// TIMELINE. The lead stream, the receiver the phone is listening to, sets the
// pace: a block is cut when the lead holds a whole one, and the lead is never
// skipped. A lead that is one sample short waits for that sample. Every other
// stream is read through a jitter buffer: it contributes nothing until it has
// queued the longer of 200 ms (one LDU) and its longest chunk plus two blocks,
// then a whole block per cut, so its own chunk sizes cannot put holes in it.
// It holds up to the longer of 400 ms and that plus two chunks; past that the
// oldest is dropped. If it does run dry it contributes what it has, is
// counted, and primes again.
//
// STALLS ARE MEASURED IN TIME. A stream that has pushed nothing for the longer
// of 200 ms and two and a half of its longest chunks is stalled: its queue is
// cleared (a few milliseconds of an old call would otherwise play at the start
// of the next) and it must prime again. The lead is never judged stalled by
// its own push, so the partial block it holds between chunks is not thrown
// away. When the lead is stalled, or there is none, the mix is paced by the
// wall clock instead, one block per 20 ms, so the voice streams are heard and
// the output never runs ahead of real time. The clock is passed in, so a test
// owns it.
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
    static constexpr std::size_t kWindowBlocks = 20;
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
        // The lead's own push is proof that it is alive, so its partial block
        // is kept however long the chunk took to come.
        if (lead_ != vrx) {
            expire(s, now, mono.size());
        }
        s.longest = std::max(s.longest, mono.size());
        if (digital_voice) {
            const auto boosted = s.boost.process(voice_db, mono);
            s.queue.insert(s.queue.end(), boosted.begin(), boosted.end());
        } else {
            s.queue.insert(s.queue.end(), mono.begin(), mono.end());
        }
        s.last_push = now;
        if (!s.primed && s.queue.size() >= prime_blocks(s) * kBlock) {
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
            expire(s, now, 0);
            // Never the lead: a long chunk of the lead is not late, it is
            // simply a lot, and is cut in full.
            const std::size_t window = window_blocks(s) * kBlock;
            if (lead_ != vrx && s.queue.size() > window) {
                const std::size_t excess = s.queue.size() - window;
                s.queue.erase(s.queue.begin(),
                              s.queue.begin() + static_cast<std::ptrdiff_t>(excess));
                dropped_ += excess;
            }
        }
        for (;;) {
            const auto lead = lead_ ? streams_.find(*lead_) : streams_.end();
            if (lead != streams_.end() && now - lead->second.last_push <= stall(lead->second)) {
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
            if (!wall_mark_ || now - *wall_mark_ > catch_up()) {
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

    // Samples dropped for arriving more than the window behind, and blocks in
    // which a primed stream ran dry.
    [[nodiscard]] std::uint64_t dropped() const { return dropped_; }
    [[nodiscard]] std::uint64_t padded_blocks() const { return padded_; }
    [[nodiscard]] std::size_t streams() const { return streams_.size(); }

private:
    struct Stream {
        std::deque<float> queue;
        VoiceGainStage boost;
        Clock::time_point last_push{};
        // The longest chunk pushed since the stream last started, in samples.
        std::size_t longest = 0;
        bool primed = false;
    };

    [[nodiscard]] static std::size_t chunk_blocks(std::size_t samples)
    {
        return (samples + kBlock - 1) / kBlock;
    }

    [[nodiscard]] static std::chrono::microseconds duration_of(std::size_t samples)
    {
        return std::chrono::microseconds(static_cast<std::int64_t>(samples) * 1'000'000 /
                                         static_cast<std::int64_t>(kPhoneAudioRate));
    }

    // How long without a push before a stream is stalled: a chunk is due about
    // its own length after the last, so two and a half of the longest is a
    // missed one.
    [[nodiscard]] static Clock::duration stall(const Stream& s, std::size_t incoming = 0)
    {
        const auto longest = duration_of(std::max(s.longest, incoming));
        return std::max<Clock::duration>(kStall, longest * 5 / 2);
    }

    [[nodiscard]] static std::size_t prime_blocks(const Stream& s)
    {
        return std::max(kPrimeBlocks, chunk_blocks(s.longest) + 2);
    }

    [[nodiscard]] static std::size_t window_blocks(const Stream& s)
    {
        return std::max(kWindowBlocks, prime_blocks(s) + 2 * chunk_blocks(s.longest));
    }

    // A stalled stream starts again: what it still holds is the tail of
    // something that ended, and the chunk size it had is of that something.
    static void expire(Stream& s, Clock::time_point now, std::size_t incoming)
    {
        if (now - s.last_push > stall(s, incoming) && (!s.queue.empty() || s.primed)) {
            s.queue.clear();
            s.primed = false;
            s.longest = 0;
        }
    }

    // How far behind the clock the wall pacing may run to catch up: a chunk of
    // the longest stream, since output is only cut when one arrives.
    [[nodiscard]] Clock::duration catch_up() const
    {
        std::size_t longest = 0;
        for (const auto& [vrx, s] : streams_) {
            longest = std::max(longest, s.longest);
        }
        return duration_of(longest) + kPrimeBlocks * kBlockTime;
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
