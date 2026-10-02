// The digital voice level: one boost the mix applies to every receiver whose
// audio is decoded voice rather than a demodulator's output.
//
// WHY DIGITAL VOICE NEEDS ITS OWN. The engine levels what a subscription
// carries by mode, docs/rpc.md "What a subscription hears is levelled": the
// amplitude modes to a -12 dBFS peak, nfm and wfm by a fixed 0.25 that puts
// full deviation at the same -12 dBFS, and the digital voice taps not at all.
// A P25 receiver plays IMBE straight off the vocoder, core/rpc/voice_audio.h,
// at whatever level the synthesis lands, and the owner heard it on 2026-10-02
// as far quieter than an nfm receiver's noise between calls. The strip gain
// cannot make up the difference, because its travel tops out at unity
// (kRackGainFloorDb), so a quiet stream can be turned down to match a loud one
// and never the other way round.
//
// A BOOST, NOT A SECOND AGC. Levelling decoded voice would ride each talker's
// loudness and pump between syllables, and the engine deliberately leaves the
// voice taps alone. One fixed gain, set by ear and remembered, is the control
// a listener can predict. The soft limiter on the sum, audio/mix_stages.h,
// still holds whatever this pushes up under full scale.
//
// Qt-free and header-only, so ui/tests holds the rules on their own.

#pragma once

#include <algorithm>
#include <cmath>
#include <string>

namespace revenant::ui {

// The control's travel in decibels. Zero is the stream as the engine sent it,
// which is the level every release before this one played.
inline constexpr double kVoiceGainMaxDb = 30.0;

// Whole decibels, inside the travel. A value that is not a number is zero: it
// can only come from a setting nobody should have written, and silence would
// be the wrong way to fail. Rounded because the control steps in whole
// decibels and every distinct value is a registry write; see
// AudioPlayer::setVoiceGainDb.
[[nodiscard]] inline double clamp_voice_gain_db(double db)
{
    if (!std::isfinite(db)) {
        return db > 0.0 ? kVoiceGainMaxDb : 0.0;
    }
    return std::round(std::clamp(db, 0.0, kVoiceGainMaxDb));
}

// The amplitude the mix multiplies a voice slot by.
[[nodiscard]] inline float voice_gain_amplitude(double db)
{
    return static_cast<float>(std::pow(10.0, clamp_voice_gain_db(db) / 20.0));
}

// "+12 dB", or "0 dB" at the bottom, for the control's readout.
[[nodiscard]] inline std::string voice_gain_text(double db)
{
    const auto whole = static_cast<long>(clamp_voice_gain_db(db));
    return whole == 0 ? std::string("0 dB") : "+" + std::to_string(whole) + " dB";
}

}  // namespace revenant::ui
