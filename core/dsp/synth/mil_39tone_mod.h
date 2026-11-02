// MIL-STD-188-110 39-tone parallel mode transmitter, for testing the decoder.
//
// MIL-STD-188-110B Appendix B, the clauses core/decode/mil_39tone.h cites:
// the preamble of B.5.4.1 with table B-II's amplitudes and phases (or the
// extended preamble of B.5.4.2), the framing and super blocks of B.5.4.3,
// and table B-V phase changes on the table B-IV tones, with the Doppler tone
// 6 dB above a data tone (B.5.5). A data tone's phase within an element runs
// from that element's onset, the reading core/decode/mil_39tone.h states.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/mil_39tone.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct Tone39ModConfig {
    dsp::SampleRate rate = 8000;
    decode::Tone39Mode mode;
    bool extended_preamble = false;
    // The RMS of the data portion is amplitude / sqrt(2), as for a sine of
    // that amplitude.
    double amplitude = 0.5;
    double offset_hz = 0.0;
    double lead_in_samples = 0.0;
    double tail_samples = 0.0;
};

[[nodiscard]] Expected<std::vector<float>> tone39_render(const Tone39ModConfig& config,
                                                         std::span<const std::uint8_t> data);

}  // namespace revenant::siggen
