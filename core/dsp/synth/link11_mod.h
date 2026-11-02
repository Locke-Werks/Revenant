// Link-11 (TADIL A) conventional waveform transmitter, for testing the
// decoder.
//
// MIL-STD-188-203-1A, the clauses core/decode/link11.h cites: five preamble
// frames of 605 and 2915 Hz with the 2915 Hz tone reversed each frame
// (5.1.4.1), the 16-tone phase reference (5.1.4.2), then the information
// segment's frames phase modulated per table I, at the levels of 5.1.3.3.
// The phase reference phases are not specified beyond keeping the peak to
// RMS ratio within 8.5 dB (5.2.8.1.2); these use a quadratic phase law, which
// keeps a multitone's peaks low. Each data tone's phase runs from its frame's
// onset unless `continuous_phase` is set.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/link11.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct Link11ModConfig {
    dsp::SampleRate rate = 8000;
    decode::Link11Rate frame_rate = decode::Link11Rate::Fast;
    bool continuous_phase = false;
    int preamble_frames = 5;
    // The data portion has the power of a sine of this amplitude.
    double amplitude = 0.5;
    double offset_hz = 0.0;
    double lead_in_samples = 0.0;
    double tail_samples = 0.0;
};

// The information segment's frames (bit location i in bit i) after the
// phase reference.
[[nodiscard]] Expected<std::vector<float>> link11_render(const Link11ModConfig& config,
                                                         std::span<const std::uint32_t> frames);

// Frames for a picket reply: start code, the words with their parity, and
// the picket stop code (5.1.6.1.2).
[[nodiscard]] std::vector<std::uint32_t> link11_picket_reply(std::span<const std::uint32_t> words);

// Frames for a DNCS interrogation with message: start code, words, control
// stop code, then the address code of the next picket (5.1.6.1.1 b).
[[nodiscard]] std::vector<std::uint32_t> link11_interrogation(std::span<const std::uint32_t> words, int address);

}  // namespace revenant::siggen
