// Transmitter for DominoEX, the counterpart of core/decode/dominoex.h and
// written from the same documents: nibble varicode, IFK+ over 18 tones,
// core/dsp/synth/mfsk_mod.h for the audio. Idle before and after the text is
// the secondary alphabet, Table note 9, by default its SPACE.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/decode/dominoex.h"
#include "core/dsp/synth/mfsk_mod.h"
#include "core/error.h"

namespace revenant::siggen {

struct DominoModConfig {
    SampleRate rate = 8000;
    decode::DominoSpeed speed = decode::DominoSpeed::D11;
    double base_hz = 1000.0;
    double amplitude = 0.5;
    double drift_hz_per_s = 0.0;
    double lead_in_samples = 0.0;
    double tail_samples = 0.0;
    bool lower_sideband = false;
    // Secondary text sent before and after the primary text.
    std::string idle_before = "    ";
    std::string idle_after = "    ";
};

// Nibbles for primary text with secondary idle around it.
[[nodiscard]] std::vector<int> domino_nibble_stream(const DominoModConfig& config,
                                                    std::string_view text);
[[nodiscard]] Expected<std::vector<float>> domino_render(const DominoModConfig& config,
                                                         std::string_view text);

}  // namespace revenant::siggen
