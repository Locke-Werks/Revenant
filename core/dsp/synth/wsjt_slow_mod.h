// Transmitters for the slow slotted MFSK modes (WSPR, JT9, JT4, JT65),
// written from the same documents as their receivers, for round-trip tests.
//
// Each renders phase-continuous FSK the way a sound card feeds an SSB
// transmitter: core/decode/wsjt_slow.h's slow_reference moved up to
// `frequency_hz` and placed `start_seconds` into a buffer. Every constant
// comes from the mode's SlowProtocol and channel-symbol function, never
// restated here.

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "core/decode/wsjt_slow.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::SampleRate;

struct SlowModConfig {
    SampleRate rate = 12000;
    // Tone 0 at the middle of the transmission.
    double frequency_hz = 1500.0;
    double start_seconds = 1.0;
    double amplitude = 0.5;
    // Linear frequency change across the transmission.
    double drift_hz = 0.0;
};

// Adds `tones` (one per symbol) into `audio`, which must already be sized.
[[nodiscard]] Status slow_add(const SlowModConfig& config, const decode::SlowProtocol& protocol,
                              std::span<const int> tones, std::span<float> audio);

// WSPR type 1: core/decode/wspr.h's packing and channel symbols.
[[nodiscard]] Status wspr_add(const SlowModConfig& config, std::string_view call, std::string_view grid,
                              int power_dbm, std::span<float> audio);

// JT9 slow submode 0 to 7 (A to H): core/decode/wsjt72.h's packing and
// core/decode/jt9.h's channel symbols.
[[nodiscard]] Status jt9_add(const SlowModConfig& config, std::string_view message, int submode,
                             std::span<float> audio);

}  // namespace revenant::siggen
