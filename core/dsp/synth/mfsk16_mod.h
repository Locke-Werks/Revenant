// Transmitter for MFSK16 and MFSK8, the counterpart of core/decode/mfsk16.h
// and written from the same four documents that header cites.
//
// Text to tones: IZ8BLY varicode, the R = 1/2 K = 7 coder from a zero
// register, ten diagonal interleavers from zero, Gray weighting, then
// core/dsp/synth/mfsk_mod.h for the audio. The tone sequence opens with spec
// C.12's eight symbols of the lowest tone and closes with enough zero data
// bits to carry every coded bit out of the interleaver and the receiver's
// Viterbi window, which more than covers C.12's four.

#pragma once

#include <string_view>
#include <vector>

#include "core/decode/mfsk16.h"
#include "core/dsp/synth/mfsk_mod.h"
#include "core/error.h"

namespace revenant::siggen {

struct Mfsk16ModConfig {
    SampleRate rate = 8000;
    decode::Mfsk16Mode mode = decode::Mfsk16Mode::Mfsk16;
    double base_hz = 1000.0;
    double amplitude = 0.5;
    double drift_hz_per_s = 0.0;
    double lead_in_samples = 0.0;
    double tail_samples = 0.0;
    bool lower_sideband = false;
    // Zero data bits after the text, before the flush.
    int tail_bits = 96;
};

// The tone sequence for text, Latin-1.
[[nodiscard]] std::vector<int> mfsk16_tones(const Mfsk16ModConfig& config, std::string_view text);

[[nodiscard]] Expected<std::vector<float>> mfsk16_render(const Mfsk16ModConfig& config,
                                                         std::string_view text);

}  // namespace revenant::siggen
