// D-STAR and DMR voice as an audio stream, through a vocoder plugin: what
// subscribeAudio serves on a dstar or dmr receiver.
//
// OWNER DECISION, 2026-10-02. DMR and D-STAR voice routes to a loaded vocoder
// plugin, and without one the receiver plays silence and says nothing about
// it. The same decision as P25's of 2026-09-23 otherwise: the decoded voice
// takes the place of the receiver's analog audio, silent between calls.
// core/rpc/voice_audio.h is the P25 stream this follows, and its notes on the
// output index, the silence between calls and the pre-roll hold here
// unchanged; they are not repeated.
//
// WHAT IS HANDED TO THE PLUGIN, AND WHY THAT UNIT
//
// Each mode's own framing unit, not a vocoder frame. D-STAR hands over the 72
// bits of one voice frame (JARL standard, clause 4.1.2 b) and DMR the 216
// vocoder socket bits of one voice burst (TS 102 361-1), VS(215) first. Both
// are where the mode's own document stops, so neither carries a constant from
// a vocoder's specification. How a plugin divides a burst into its codec's
// frames is the plugin's business. Which plugin serves a mode is the name
// convention core/decode/vocoder_abi.h states beside rv_vocoder_desc::name.
//
// The output rate is the plugin's. With no plugin there is nothing to decode
// and the stream is silence at kPluginVoiceSilentRateHz, an engineering choice
// that matches P25's so a client opens one sink rate for either.
//
// DMR: ONE CALL AT A TIME, AND NEVER A PRIVATE ONE
//
// A DMR receiver hears both TDMA slots, and two calls interleaved into one
// stream would be neither call. So the stream follows one slot: the first
// whose voice arrives, until a terminator with LC closes its call or no voice
// has come from it for kDmrVoiceReleaseSeconds. The other slot's voice is
// dropped meanwhile.
//
// A call marked private, by a PI header or the Privacy bit of the Service
// Options in its Full LC, plays silence and its bits never reach a plugin.
// docs/modes.md's line is that this project decodes what is sent in the clear
// and does not decrypt; a vocoder handed encrypted bits would only make
// noise, and making that noise is not this stream's job either.
//
// A HEADER, for decoders.h's reason: it runs on a decode lane and a Catch2
// case drives it with no socket.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "core/decode/dmr.h"
#include "core/decode/dstar.h"
#include "core/decode/vocoder.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/rpc/decoders.h"
#include "core/rpc/voice_audio.h"

namespace revenant::rpc {

enum class PluginVoiceMode : std::uint8_t { Dstar, Dmr };

// The demodulator name a mode's plugin is found by, and the bits its framing
// hands over per unit.
[[nodiscard]] constexpr const char* plugin_voice_mode_name(PluginVoiceMode mode) noexcept {
    switch (mode) {
        case PluginVoiceMode::Dstar: return "dstar";
        case PluginVoiceMode::Dmr: return "dmr";
    }
    return "invalid";
}

[[nodiscard]] constexpr std::uint32_t plugin_voice_unit_bits(PluginVoiceMode mode) noexcept {
    switch (mode) {
        case PluginVoiceMode::Dstar: return static_cast<std::uint32_t>(decode::kDStarVoiceBits);
        case PluginVoiceMode::Dmr: return static_cast<std::uint32_t>(decode::kDmrVoiceBits);
    }
    return 0;
}

// The rate of the silence a mode with no plugin plays. An engineering choice,
// P25's rate, so a client's sink opens at one rate for every voice receiver.
inline constexpr std::uint32_t kPluginVoiceSilentRateHz = kP25VoiceRateHz;

// How long a DMR slot's voice may stop before the stream lets another slot's
// call in. An engineering choice: longer than a missed burst or two, at 60 ms
// a burst per slot, and short enough that a call answered on the other slot
// is heard from its first second.
inline constexpr double kDmrVoiceReleaseSeconds = 1.0;

class PluginVoiceStream {
public:
    // `vocoder` may be null, which is the no-plugin case: the stream decodes
    // nothing and plays silence.
    [[nodiscard]] static Expected<PluginVoiceStream> create(
        PluginVoiceMode mode, dsp::SampleRate rate, std::unique_ptr<decode::Vocoder> vocoder) {
        if (rate <= 0) {
            return fail(std::format("a {} voice stream needs a positive input rate, got {}",
                                    plugin_voice_mode_name(mode), rate));
        }
        if (vocoder != nullptr) {
            const decode::VocoderFrame shape = vocoder->shape();
            if (shape.bit_count != plugin_voice_unit_bits(mode) || shape.pcm_frames == 0 ||
                shape.sample_rate == 0) {
                return fail(std::format(
                    "the {} voice stream was given the vocoder {} ({}), and it takes {} bits "
                    "a unit with a non-zero output",
                    plugin_voice_mode_name(mode), vocoder->name(),
                    decode::describe_vocoder_frame(shape), plugin_voice_unit_bits(mode)));
            }
        }

        PluginVoiceStream stream(mode, rate, std::move(vocoder));
        if (stream.vocoder_ == nullptr) {
            return stream;
        }
        if (mode == PluginVoiceMode::Dstar) {
            decode::DStarConfig config;
            config.filter_taps =
                decoders_detail::scaled_taps(config.filter_taps, config.rate, rate);
            config.rate = rate;
            auto built = decode::DStar::create(config);
            if (!built) {
                return std::unexpected(with_context(built.error(), "building the dstar voice stream"));
            }
            stream.dstar_.emplace(std::move(*built));
        } else {
            decode::DmrConfig config;
            config.filter_taps =
                decoders_detail::scaled_taps(config.filter_taps, config.rate, rate);
            config.rate = rate;
            auto built = decode::Dmr::create(config);
            if (!built) {
                return std::unexpected(with_context(built.error(), "building the dmr voice stream"));
            }
            stream.dmr_.emplace(std::move(*built));
        }
        return stream;
    }

    // One chunk of the receiver's complex baseband in, the same stretch of
    // time out at out_rate(). `discard` is the retune fence: the chunk is not
    // decoded and its stretch is silence.
    [[nodiscard]] Status process(const DecoderChunk& chunk, bool discard, VoiceChunk& out) {
        out.samples.clear();
        out.voiced = false;
        if (auto shape = decoders_detail::require_complex(plugin_voice_mode_name(mode_), chunk,
                                                         rate_);
            !shape) {
            return shape;
        }

        const std::uint64_t first = out_index(chunk.start);
        const std::uint64_t last = out_index(chunk.start + chunk.frames());
        out.start = first;

        if (!discard && chunk.frames() > 0 && vocoder_ != nullptr) {
            decoders_detail::to_complex(chunk, iq_);
            if (dstar_) {
                if (auto done = decode_dstar(); !done) {
                    return done;
                }
            } else if (dmr_) {
                if (auto done = decode_dmr(); !done) {
                    return done;
                }
            }
        }

        const auto count = static_cast<std::size_t>(last - first);
        longest_chunk_ = std::max(longest_chunk_, count);
        out.samples.resize(count, 0.0F);
        for (std::size_t i = 0; i < count; ++i) {
            out.samples[i] = next(out.voiced);
        }
        return {};
    }

    // Everything carried between chunks, for a retune: the decoder's sync,
    // the vocoder's interframe state, the slot being followed and the held
    // audio, which was the old frequency's.
    void reset() {
        if (dstar_) {
            dstar_->reset();
        }
        if (dmr_) {
            dmr_->reset();
        }
        if (vocoder_ != nullptr) {
            vocoder_->reset();
        }
        slots_ = {};
        following_ = false;
        followed_slot_ = 0;
        held_.clear();
        held_head_ = 0;
        playing_ = false;
        preroll_ = 0;
    }

    [[nodiscard]] std::uint32_t out_rate() const noexcept { return out_rate_; }
    [[nodiscard]] bool has_vocoder() const noexcept { return vocoder_ != nullptr; }

    // The DMR slot being followed, 0 for none. Always 0 for D-STAR.
    [[nodiscard]] std::uint8_t followed_slot() const noexcept {
        return following_ ? followed_slot_ : 0;
    }

private:
    PluginVoiceStream(PluginVoiceMode mode, dsp::SampleRate rate,
                      std::unique_ptr<decode::Vocoder> vocoder)
        : mode_(mode),
          rate_(rate),
          vocoder_(std::move(vocoder)),
          out_rate_(vocoder_ != nullptr ? vocoder_->shape().sample_rate
                                        : kPluginVoiceSilentRateHz) {}

    [[nodiscard]] std::uint64_t out_index(std::uint64_t index) const {
        const auto rate = static_cast<std::uint64_t>(rate_);
        return (index / rate) * out_rate_ + ((index % rate) * out_rate_) / rate;
    }

    // One unit through the plugin. A unit the plugin will not decode, a frame
    // that failed its error correction above all, is that much silence rather
    // than a gap, so the call keeps its timing.
    void decode_unit(std::span<const std::uint8_t> bits) {
        const std::size_t frames = vocoder_->shape().pcm_frames;
        pcm_.assign(frames, 0.0F);
        if (!vocoder_->decode(bits, pcm_)) {
            std::fill(pcm_.begin(), pcm_.end(), 0.0F);
        }
        hold(pcm_);
    }

    [[nodiscard]] Status decode_dstar() {
        transmissions_.clear();
        if (auto processed = dstar_->process(iq_, transmissions_); !processed) {
            return processed;
        }
        for (const decode::DStarTransmission& piece : transmissions_) {
            for (const decode::DStarVoiceFrame& frame : piece.frames) {
                decode_unit(frame.voice);
            }
            if (piece.ended) {
                vocoder_->reset();
            }
        }
        return {};
    }

    [[nodiscard]] Status decode_dmr() {
        bursts_.clear();
        if (auto processed = dmr_->process(iq_, bursts_); !processed) {
            return processed;
        }
        const auto release = static_cast<std::uint64_t>(kDmrVoiceReleaseSeconds *
                                                        static_cast<double>(rate_));
        for (const decode::DmrBurst& burst : bursts_) {
            const std::uint8_t slot = std::min<std::uint8_t>(burst.slot, 2);
            SlotCall& call = slots_[slot];
            const std::uint64_t at = burst.first_sample;

            // A call is over when its terminator says so, and also when it has
            // been quiet past the release: a terminator lost to a fade must not
            // leave the slot's next call inheriting this one's privacy.
            if (call.active && at > call.last_voice + release) {
                call = SlotCall{};
            }
            // A voice LC header opens a call, so it starts from nothing.
            if (burst.full_lc && burst.full_lc->carrier == decode::DmrLcCarrier::VoiceHeader) {
                call = SlotCall{};
            }

            // What marks the call private, from wherever it arrives.
            if (burst.pi_header) {
                call.private_call = true;
            }
            if (burst.full_lc && burst.full_lc->service_options &&
                burst.full_lc->service_options->privacy) {
                call.private_call = true;
            }

            const bool terminator =
                (burst.data_type && *burst.data_type == static_cast<std::uint8_t>(
                                                            decode::DmrDataType::TerminatorWithLc)) ||
                (burst.full_lc && burst.full_lc->carrier == decode::DmrLcCarrier::Terminator);
            if (terminator) {
                call = SlotCall{};
                if (following_ && followed_slot_ == slot) {
                    following_ = false;
                    vocoder_->reset();
                }
                continue;
            }

            if (burst.voice_burst == 0 || burst.payload.size() != decode::kDmrVoiceBits) {
                continue;
            }
            call.active = true;
            call.last_voice = at;

            // Let a followed call that went quiet go, so the other slot can be
            // heard.
            if (following_ && followed_slot_ != slot) {
                const SlotCall& followed = slots_[followed_slot_];
                if (!followed.active || at > followed.last_voice + release) {
                    following_ = false;
                    vocoder_->reset();
                }
            }
            if (!following_) {
                following_ = true;
                followed_slot_ = slot;
            }
            if (slot != followed_slot_ || call.private_call) {
                continue;
            }
            decode_unit(burst.payload);
        }
        return {};
    }

    // voice_audio.h's buffer, unchanged: held at most a second, played after
    // a pre-roll at least as long as the longest chunk.
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
        if (waiting > out_rate_) {
            held_head_ += waiting - out_rate_;
        }
    }

    [[nodiscard]] float next(bool& voiced) {
        const bool waiting = held_head_ < held_.size();
        if (!playing_) {
            if (!waiting) {
                return 0.0F;
            }
            playing_ = true;
            const std::size_t floor =
                (kP25VoicePrerollFrames * out_rate_) / kP25VoiceRateHz;
            preroll_ = std::max(floor, longest_chunk_);
        }
        if (preroll_ > 0) {
            --preroll_;
            return 0.0F;
        }
        if (!waiting) {
            playing_ = false;
            return 0.0F;
        }
        voiced = true;
        return held_[held_head_++];
    }

    // One DMR slot's call: whether it has voice, when its last voice burst
    // arrived in input samples, and whether it is private.
    struct SlotCall {
        bool active = false;
        std::uint64_t last_voice = 0;
        bool private_call = false;
    };

    PluginVoiceMode mode_;
    dsp::SampleRate rate_;
    std::unique_ptr<decode::Vocoder> vocoder_;
    std::uint32_t out_rate_;

    std::optional<decode::DStar> dstar_;
    std::optional<decode::Dmr> dmr_;
    std::vector<dsp::Complex32> iq_;
    std::vector<decode::DStarTransmission> transmissions_;
    std::vector<decode::DmrBurst> bursts_;
    std::vector<float> pcm_;

    // Indexed by DmrBurst::slot, 0 for a burst not yet joined to one.
    std::array<SlotCall, 3> slots_{};
    bool following_ = false;
    std::uint8_t followed_slot_ = 0;

    std::vector<float> held_;
    std::size_t held_head_ = 0;
    bool playing_ = false;
    std::size_t preroll_ = 0;
    std::size_t longest_chunk_ = 0;
};

}  // namespace revenant::rpc
