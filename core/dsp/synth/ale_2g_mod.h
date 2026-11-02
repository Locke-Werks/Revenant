// 2G ALE transmitter: ALE words to 8-FSK audio, for testing the decoder.
//
// MIL-STD-188-141D Appendix A, the same clauses core/decode/ale_2g.h cites:
// A.5.1.2 for the tones, A.5.1.3 for timing, figure A-10 for the bit order,
// A.5.2.2.4 for sending each word three times. The tones are phase
// continuous (A.5.1.2); every tone is a whole number of cycles in 8 ms
// (figure A-5), so a continuous phase accumulator also meets the
// "transitions at waveform maxima or minima" clause when the phase starts at
// zero of a cosine.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct Ale2gModConfig {
    dsp::SampleRate rate = 8000;
    double amplitude = 0.5;
    // Added to every tone, for a mistuned receiver.
    double offset_hz = 0.0;
    // Silence before and after, in samples.
    double lead_in_samples = 0.0;
    double tail_samples = 0.0;
};

// The tone sequence (0 = 750 Hz) for words, each sent three times.
[[nodiscard]] std::vector<int> ale_2g_tones(std::span<const std::uint32_t> words);

[[nodiscard]] Expected<std::vector<float>> ale_2g_render(const Ale2gModConfig& config,
                                                         std::span<const std::uint32_t> words);

}  // namespace revenant::siggen
