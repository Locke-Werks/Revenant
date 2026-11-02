// MIL-STD-188-110C serial (single-tone) transmitter, for tests and the
// signal generator: tribit numbers from core/decode/mil110.h to real audio.
//
// MIL-STD-188-110C with Change Notice 1, 5.3.2.3.9: tribit n is the 1800 Hz
// carrier at n * 45 degrees, 2400 symbols/s. The document asks only that
// the baseband be filtered; this uses a root raised cosine of roll-off 0.35
// spanning eight symbols, which keeps the signal inside 585 to 3015 Hz.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/mil110.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct Mil110ModConfig {
    // A whole multiple of 2400.
    dsp::SampleRate rate = 9600;
    // Peak of an unmodulated carrier of the same power.
    double amplitude = 0.5;
    // Added to the 1800 Hz carrier, for a mistuned receiver.
    double offset_hz = 0.0;
    // Silence before and after, in samples.
    std::size_t lead_in_samples = 0;
    std::size_t tail_samples = 0;
};

[[nodiscard]] Expected<std::vector<float>> mil110_render(const Mil110ModConfig& config,
                                                         std::span<const std::uint8_t> tribits);

// mil110_transmit_symbols then mil110_render.
[[nodiscard]] Expected<std::vector<float>> mil110_render_bits(const Mil110ModConfig& config,
                                                              decode::Mil110Mode mode,
                                                              std::span<const std::uint8_t> bits);

}  // namespace revenant::siggen
