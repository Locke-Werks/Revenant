// Transmitter for FSQ characters, the counterpart of core/decode/fsq.h and
// written from the same two documents: Varicode symbols, IFK+ over 33 tones
// with an offset of one, core/dsp/synth/mfsk_mod.h for the audio. IDLE
// (28,30) is sent before and after the text so the receiver has a previous
// tone to difference against and the last character is closed.

#pragma once

#include <string_view>
#include <vector>

#include "core/decode/fsq.h"
#include "core/dsp/synth/mfsk_mod.h"
#include "core/error.h"

namespace revenant::siggen {

struct FsqModConfig {
    SampleRate rate = 8000;
    double symbol_rate = decode::kFsqTopSymbolRate;
    double base_hz = 1350.0;
    double amplitude = 0.5;
    double drift_hz_per_s = 0.0;
    double lead_in_samples = 0.0;
    double tail_samples = 0.0;
    bool lower_sideband = false;
    int idle_before = 2;
    int idle_after = 2;
};

[[nodiscard]] std::vector<int> fsq_symbol_stream(const FsqModConfig& config, std::string_view text);
[[nodiscard]] Expected<std::vector<float>> fsq_render(const FsqModConfig& config,
                                                      std::string_view text);

}  // namespace revenant::siggen
