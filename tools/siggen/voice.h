// `siggen voice`: analogue voice on every modulation an operator meets it on,
// beside the controls a voice rule must not claim, for measuring the labels
// the detector puts on voice.
//
// THE SCENE, which is this file's choice and is fixed so two runs compare.
// Every voice emitter carries tools/siggen/speech.h's speech, each its own
// talker (its own seed):
//
//   AM, index 0.8             -450 kHz    NFM, 2.5 kHz peak deviation  -300 kHz
//   NFM, 5 kHz peak deviation -150 kHz    USB                           +50 kHz
//   LSB                       +200 kHz    CW at 20 WPM                 +300 kHz
//   BPSK at 1200 baud         +400 kHz    nothing                      +500 kHz
//
// 1200000 S/s, because on 32 coarse channels that is the 37.5 kHz spacing,
// 75000 S/s channels and, at a 2048-point second stage, the 36.6 Hz bins of
// the owner's 2.4 MS/s RTL-SDR grid over 64 channels, in half the file. Each
// emitter is rendered at 48000 S/s and lifted by 25.
//
// The two deviations are the land mobile ones core/dsp/synth/modulators.h
// names: 5 kHz for 25 kHz channels and 2.5 kHz for the 12.5 kHz refarming.
// Pre-emphasis is not applied; TIA-603's 6 dB per octave would move the
// deviation toward the top of the voice channel and is a later refinement.
//
// Every emitter sits at the same SNR in 2500 Hz, over its mean power across
// the whole loop. For AM and NFM that is the carrier's, which is there in the
// pauses too. For USB and LSB it averages the pauses in, so while the talker
// is talking a sideband emitter stands 1/talking_fraction higher, about 1 dB.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "tools/siggen/lift.h"

namespace revenant::siggen_voice {

inline constexpr dsp::SampleRate kVoiceSceneRate = 1'200'000;

struct VoiceSceneSpec {
    double seconds = 20.0;
    std::uint64_t seed = 20260924;
    double snr_2500_db = 20.0;
    double noise_dbfs = -60.0;
};

// What each slot holds and the label a correct detector would put on it.
struct VoiceTruth {
    std::string name;
    dsp::Hertz offset_hz = 0;

    // The nominal occupied band, as offsets from the capture's centre.
    dsp::Hertz low_hz = 0;
    dsp::Hertz high_hz = 0;

    // The label name a correct answer gives, empty for the empty slot.
    std::string label;
};

struct VoiceScene {
    std::vector<siggen_lift::LiftedEmitter> emitters;
    std::vector<VoiceTruth> truth;
};

[[nodiscard]] Expected<VoiceScene> build_voice_scene(const VoiceSceneSpec& spec);

// The capture, block by block, through tools/siggen/lift.h.
[[nodiscard]] Status render_voice_scene(const VoiceSceneSpec& spec,
                                        const siggen_lift::BlockSink& sink);

// Writes interleaved cf32 at kVoiceSceneRate, and a truth line per slot when
// truth_path is not empty.
[[nodiscard]] Status write_voice_scene(const VoiceSceneSpec& spec, const std::string& out_path,
                                       const std::string& truth_path);

}  // namespace revenant::siggen_voice
