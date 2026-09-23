// P25 voice as an audio stream: what subscribeAudio serves on a p25p1
// receiver in place of the complex baseband the receiver itself delivers.
//
// OWNER DECISION, 2026-09-23. For a digital voice receiver the decoded voice
// takes the place of the receiver's analog audio. A P25 receiver plays IMBE
// voice at 8000 S/s, is silent between calls, and plays nothing for an
// encrypted call, so a listener never hears discriminator noise. D-STAR and
// TETRA have no voice codec in this tree, so their audio stays refused.
//
// This is the smallest change docs/rpc.md named for it, answered question by
// question:
//
//   What the chunk's index counts. Frames at 8000 S/s, derived from the
//   receiver's own stream index rather than counted by this object:
//   floor(start * 8000 / rate). A chunk covers exactly the stretch of time
//   the receiver chunk it came from covers, so an upstream gap in the
//   receiver's stream is a gap of the same length here and the client's gap
//   accounting reads it for what it is.
//
//   Whether the time between calls is silence or no chunks. Silence, zeros at
//   the full 8000 S/s with squelchOpen false, which is how a closed squelch
//   already crosses: a stream that stops is indistinguishable from a receiver
//   that went away, and AudioChunk carries its own rate, so nothing in the
//   schema moves. An encrypted call is the same silence, because P25Voice
//   hands nothing out for one; docs/modes.md draws that line.
//
//   Whether the refusal becomes a voice subscription or a second method. The
//   subscription: subscribeAudio on a p25p1 receiver now answers with this.
//
// WHY A PRE-ROLL. P25Voice hands a call over nine voice frames at a time,
// 1440 samples, when an LDU is complete, and the next LDU completes 180 ms of
// air time later, detected at the end of whichever engine block holds its
// last symbol. Played out at 8000 S/s from the moment the first LDU arrived,
// the buffer would reach empty at the same instant the next LDU is due and
// underrun by up to one block on every LDU. So the first voice of a call is
// put a pre-roll behind the start of the chunk it arrived in, and the buffer
// runs that much ahead of empty for the rest of the call.
//
// The pre-roll is the longer of 50 ms and the longest chunk seen, and the
// chunk is the part that matters. An LDU that completes at the very end of
// its chunk has its voice scheduled from that chunk's start, so the next one
// is due up to one chunk later than the drain assumes; a pre-roll of at least
// one chunk is what covers that, and nothing shorter does. On a 2.4 MS/s
// source a chunk is 27 ms at the 65536-sample blocks revenant-engine defaults
// to and 6.8 ms at 16384. tests/rpc/test_rpc_voice.cpp feeds this 341 ms chunks, and
// a fixed 50 ms underran there and put a run of zeros inside the call.
//
// WHY A HEADER. The same argument core/rpc/decoders.h makes: this runs on the
// engine's completion thread inside the server's audio sink, and a Catch2
// case can drive it with no socket in the way.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <utility>
#include <vector>

#include "core/decode/imbe.h"
#include "core/decode/p25p1.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/rpc/decoders.h"

namespace revenant::rpc {

// The vocoder's own rate, which is the rate every chunk this stream hands out
// carries.
inline constexpr std::uint32_t kP25VoiceRateHz = decode::ImbeDecoder::kSampleRateHz;

// The shortest silence put ahead of the first voice of a call, in frames at
// kP25VoiceRateHz. See WHY A PRE-ROLL above for why it can be longer.
inline constexpr std::size_t kP25VoicePrerollFrames = 400;

// The most voice held waiting to be played, one second. The decoder cannot run
// ahead of the air, so reaching it means the receiver's stream went on without
// this object emitting, and the oldest is dropped rather than played late.
inline constexpr std::size_t kP25VoiceHeldFrames = kP25VoiceRateHz;

// One receiver chunk's worth of voice-domain audio.
struct VoiceChunk {
    std::vector<float> samples;

    // The first frame's index at kP25VoiceRateHz. See the note at the top.
    std::uint64_t start = 0;

    // Any frame in this chunk was voice rather than the silence between calls.
    bool voiced = false;
};

class P25AudioStream {
public:
    // Built for the rate the receiver delivers, which is the chunk's rather
    // than anything VrxStatus says, on the argument DecodeRoute makes.
    [[nodiscard]] static Expected<P25AudioStream> create(dsp::SampleRate rate) {
        if (rate <= 0) {
            return fail(std::format("a P25 voice stream needs a positive input rate, got {}",
                                    rate));
        }
        decode::P25Config config;
        config.filter_taps = decoders_detail::scaled_taps(config.filter_taps, config.rate, rate);
        config.rate = rate;
        auto built = decode::P25Phase1::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the p25p1 voice stream"));
        }
        return P25AudioStream(rate, std::move(*built));
    }

    // One receiver chunk in, the voice-rate chunk covering the same time out.
    //
    // `discard` is the retune fence: the chunk is from the tuning the client
    // left, so it is not decoded, and the time it covers goes out as silence
    // so the timeline stays whole.
    //
    // A chunk that is not complex baseband at the rate this was built for is
    // refused, in decoders_detail::require_complex's words, and nothing is
    // written. Terminal, on RdsRoute::fault's argument: it is shape, and the
    // engine refuses a retune that changes shape.
    [[nodiscard]] Status process(const DecoderChunk& chunk, bool discard, VoiceChunk& out) {
        out.samples.clear();
        out.voiced = false;
        if (auto shape = decoders_detail::require_complex("p25p1 voice", chunk, rate_);
            !shape) {
            return shape;
        }

        const std::uint64_t first = voice_index(chunk.start);
        const std::uint64_t last = voice_index(chunk.start + chunk.frames());
        out.start = first;

        if (!discard && chunk.frames() > 0) {
            decoders_detail::to_complex(chunk, iq_);
            frames_.clear();
            if (auto processed = decoder_.process(iq_, frames_); !processed) {
                return processed;
            }
            pcm_.clear();
            for (const decode::P25Frame& frame : frames_) {
                if (auto pushed = voice_.push(frame, pcm_); !pushed) {
                    return pushed;
                }
            }
            hold(pcm_);
        }

        const auto count = static_cast<std::size_t>(last - first);
        longest_chunk_ = std::max(longest_chunk_, count);
        out.samples.resize(count, 0.0F);
        for (std::size_t i = 0; i < count; ++i) {
            out.samples[i] = next(out.voiced);
        }
        return {};
    }

    // Forgets the transmitter and everything held, for a retune.
    void reset() {
        decoder_.reset();
        voice_.reset();
        held_.clear();
        held_head_ = 0;
        playing_ = false;
        preroll_ = 0;
    }

    [[nodiscard]] const decode::P25CallState& call() const noexcept { return voice_.call(); }
    [[nodiscard]] dsp::SampleRate rate() const noexcept { return rate_; }

    // Voice frames decoded and waiting to be played, for the tests.
    [[nodiscard]] std::size_t held_frames() const noexcept { return held_.size() - held_head_; }

private:
    P25AudioStream(dsp::SampleRate rate, decode::P25Phase1 decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    // floor(index * 8000 / rate), written so it cannot overflow for any index
    // a stream reaches: the quotient and the remainder are scaled apart.
    [[nodiscard]] std::uint64_t voice_index(std::uint64_t index) const {
        const auto rate = static_cast<std::uint64_t>(rate_);
        return (index / rate) * kP25VoiceRateHz + ((index % rate) * kP25VoiceRateHz) / rate;
    }

    void hold(std::span<const float> pcm) {
        if (pcm.empty()) {
            return;
        }
        if (held_head_ > 0 && held_head_ * 2 >= held_.size()) {
            held_.erase(held_.begin(), held_.begin() + static_cast<std::ptrdiff_t>(held_head_));
            held_head_ = 0;
        }
        held_.insert(held_.end(), pcm.begin(), pcm.end());
        const std::size_t waiting = held_.size() - held_head_;
        if (waiting > kP25VoiceHeldFrames) {
            held_head_ += waiting - kP25VoiceHeldFrames;
        }
    }

    // The next frame to hand out: pre-roll, voice, or the silence between.
    [[nodiscard]] float next(bool& voiced) {
        const bool waiting = held_head_ < held_.size();
        if (!playing_) {
            if (!waiting) {
                return 0.0F;
            }
            playing_ = true;
            preroll_ = std::max(kP25VoicePrerollFrames, longest_chunk_);
        }
        if (preroll_ > 0) {
            --preroll_;
            return 0.0F;
        }
        if (!waiting) {
            // The call ended, or a data unit was lost to the channel. Either
            // way the next voice that arrives starts behind a fresh pre-roll.
            playing_ = false;
            return 0.0F;
        }
        voiced = true;
        return held_[held_head_++];
    }

    dsp::SampleRate rate_;
    decode::P25Phase1 decoder_;
    decode::P25Voice voice_;

    std::vector<dsp::Complex32> iq_;
    std::vector<decode::P25Frame> frames_;
    std::vector<float> pcm_;

    std::vector<float> held_;
    std::size_t held_head_ = 0;
    bool playing_ = false;
    std::size_t preroll_ = 0;

    // In frames at kP25VoiceRateHz. Kept across reset(), because the block
    // size is the engine's and a retune does not move it.
    std::size_t longest_chunk_ = 0;
};

}  // namespace revenant::rpc
