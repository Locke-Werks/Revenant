// `siggen labelled`: one wideband capture holding a signal of each kind the
// span's labels name, for looking at the labels.
//
// WHY THIS AND NOT `siggen wideband`
//
// The wideband scene draws its emitters from core/dsp/synth/modulators.h's
// palette, which is analogue modes and generic PSK and FSK. The digital
// protocols core/identify names have transmitters of their own in
// core/dsp/synth, each at its own rate, and none of them is on that palette.
// This renders one of each at its own rate, lifts it onto the wideband rate,
// and sums them with noise, so a capture exists in which every kind of label
// has something to be.
//
// THE SCENE, which is this file's choice and is fixed so two runs compare:
//
//   AM, voice-shaped         -800 kHz    NFM, voice-shaped      -600 kHz
//   CW at 20 WPM             -450 kHz    BPSK at 2400 baud      -300 kHz
//   P25 Phase 1 voice        -150 kHz    D-STAR                 +100 kHz
//   TETRA sync bursts        +250 kHz    M17 stream             +400 kHz
//   AX.25 on NFM             +550 kHz    RTTY at 45.45 baud     +700 kHz
//   USB, voice-shaped        +850 kHz
//
// "Voice-shaped" is Gaussian noise band-limited to 300 to 3000 Hz, so the
// analogue emitters fill their bands the way speech does rather than
// breaking into the line spectra a test tone makes, which docs/detection.md
// measured the detector reports line by line.
//
// 2160000 S/s, because it is 45 times 48000 and 30 times 72000, so every
// transmitter lifts by a whole factor, and on 16 coarse channels it runs
// 135000 S/s channels, which hold the 96000 S/s probe bucket a TETRA signal
// needs. Every emitter sits at the same SNR in 2500 Hz.

#pragma once

#include <cstdint>
#include <string>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen_labelled {

struct LabelledSceneSpec {
    std::string out_path;
    std::string truth_path;
    double seconds = 24.0;
    std::uint64_t seed = 20260923;
    double snr_2500_db = 25.0;
    double noise_dbfs = -60.0;
};

inline constexpr dsp::SampleRate kLabelledRate = 2'160'000;

// Writes interleaved cf32 at kLabelledRate, and a truth line per emitter when
// truth_path is not empty.
[[nodiscard]] Status render_labelled_scene(const LabelledSceneSpec& spec);

}  // namespace revenant::siggen_labelled
