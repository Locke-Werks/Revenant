// The desktop's digital voice level, applied to what the phone is sent.
//
// The owner asked on 2026-11-03 whether upsampling the vocoder audio to 48 kHz
// could come with normalising it. The desktop deliberately does not AGC
// digital voice (ui/audio/voice_gain.h says why: levelling decoded voice rides
// each talker and pumps between syllables), so the phone does what the desktop
// does instead: one fixed boost, the desktop's audio/voiceGainDb, here, then
// the desktop's soft limiter from ui/audio/mix_stages.h on the sum, in
// audio_mix.h. Both ui headers are Qt-free and are reused as they are, so the
// two cannot drift apart.
//
// Only receivers whose audio is decoded voice get it. The engine already
// levels every analogue mode, and boosting those would stack a second gain on
// the first.
//
// The boost is read from the registry key the desktop writes, polled the same
// way the display settings are, so turning the control on the desktop reaches
// a phone that is already listening within a couple of seconds.

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "audio/voice_gain.h"
#include "core/rpc/types.h"
#include "tools/webgate/audio_rate.h"
#include "tools/webgate/display_sync.h"

namespace revenant::webgate {

// Whether a demodulator's audio is decoded voice, which the engine plays at
// whatever level the vocoder lands on and does not level.
[[nodiscard]] constexpr bool is_digital_voice(rpc::Demod demod)
{
    switch (demod) {
    case rpc::Demod::P25p1:
    case rpc::Demod::Dstar:
    case rpc::Demod::Tetra:
    case rpc::Demod::Dmr:
        return true;
    default:
        return false;
    }
}

// audio/voiceGainDb as the text QSettings stored ("30"), through the desktop's
// own rules: whole decibels, clamped to its travel, anything that is not a
// number is zero. Missing is zero too, which is the stream as the engine sent
// it.
[[nodiscard]] double parse_voice_gain_db(std::optional<std::string_view> text);

[[nodiscard]] double read_voice_gain_db(const DisplayValueReader& read);

// HKCU\Software\Locke Werks\Revenant\audio. Answers nothing off Windows.
[[nodiscard]] DisplayValueReader registry_audio_reader();

// The boost, re-read at most once an interval however often it is asked for.
// Any thread.
class VoiceGainSource {
public:
    using Clock = std::chrono::steady_clock;

    explicit VoiceGainSource(DisplayValueReader reader,
                             std::function<Clock::time_point()> clock = {},
                             std::chrono::milliseconds interval = std::chrono::seconds(2));

    [[nodiscard]] double current();

private:
    DisplayValueReader reader_;
    std::function<Clock::time_point()> clock_;
    std::chrono::milliseconds interval_;
    std::mutex mutex_;
    std::optional<Clock::time_point> read_at_;
    double db_ = 0.0;
};

// The boost, on one receiver's 48 kHz mono stream. The soft limiter is not
// here: it runs on the sum of every stream, in PhoneMixer, as on the desktop.
// Not thread safe; it lives with the receiver's audio path on the strand.
class VoiceGainStage {
public:
    // About 20 ms, long enough that a step in gain is not heard as a click and
    // short enough to be done before the next poll's change could stack on it.
    static constexpr std::size_t kRampSamples = kPhoneAudioRate / 50;

    // The stream at `db` of boost. The span is good until the next call. A
    // change in `db` ramps linearly in amplitude from wherever the gain is
    // now; the first call starts at its target, since there is nothing before
    // it to ramp from.
    [[nodiscard]] std::span<const float> process(double db, std::span<const float> mono)
    {
        const double target = static_cast<double>(ui::voice_gain_amplitude(db));
        if (!started_) {
            started_ = true;
            current_ = target_ = target;
        } else if (target != target_) {
            target_ = target;
            step_ = (target_ - current_) / static_cast<double>(kRampSamples);
            remaining_ = kRampSamples;
        }
        out_.resize(mono.size());
        for (std::size_t i = 0; i < mono.size(); ++i) {
            if (remaining_ > 0) {
                current_ += step_;
                if (--remaining_ == 0) {
                    current_ = target_;
                }
            }
            out_[i] = static_cast<float>(static_cast<double>(mono[i]) * current_);
        }
        return out_;
    }

private:
    std::vector<float> out_;
    bool started_ = false;
    double current_ = 1.0;
    double target_ = 1.0;
    double step_ = 0.0;
    std::size_t remaining_ = 0;
};

}  // namespace revenant::webgate
