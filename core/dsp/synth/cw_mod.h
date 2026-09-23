// A Morse keyer, written from the same Recommendation as the decoder in
// core/decode/cw.h.
//
// WHY IT EXISTS
//
// The reason core/dsp/synth/dv_mod.h gives: the round trip through a
// transmitter written from the same clauses stands in for the scalar twin a
// host decoder does not have. The table and every timing constant come from
// core/decode/cw.h, never restated here.
//
// WHAT IT KEYS
//
// Standard timing per ITU-R M.1677-1 Annex 1 Part I clause 2, or Farnsworth
// spacing per the ARRL Morse Transmission Timing Standard clause 2.2 when an
// overall speed below the character speed is asked for. A space in the text
// is a word space. A hand sender's irregularity is available as a random
// stretch of every element and space, seeded, so the decoder's tolerance can
// be measured rather than asserted.
//
// The carrier's edges are raised-cosine ramps. NOT IN THE RECOMMENDATION,
// which is about the code and not the keying waveform; an unshaped edge
// splatters, and every transmitter shapes its keying somehow. Each ramp is
// centred on the edge it shapes, so an element measured at half amplitude
// is exactly the clause 2 length.
//
// The output is the analytic signal at the tone, for the reason
// core/dsp/synth/psk31_mod.h gives: noise goes on through
// analytic_to_noisy_audio there, and the audio is the real part.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/decode/cw.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::Hertz;
using dsp::SampleRate;

struct CwModConfig {
    SampleRate rate = 48000;
    Hertz tone_hz = 700;

    // Character speed, PARIS words per minute.
    double wpm = 20.0;

    // Overall speed. Zero, or anything at or above wpm, is standard spacing;
    // below wpm is Farnsworth.
    double overall_wpm = 0.0;

    double amplitude = 1.0;

    // Rise and fall time of each edge, in seconds. See the header.
    double edge_seconds = 0.005;

    // Standard deviation of a random stretch applied to every element and
    // space, as a fraction of its length. Zero is a perfect keyer.
    double jitter = 0.0;
    std::uint64_t seed = 1;

    // Key-up before the first element and after the last, in seconds.
    double lead_in_seconds = 0.5;
    double tail_seconds = 1.0;
};

struct CwKeyRun {
    bool key_down = false;
    double seconds = 0.0;
};

// The key-down and key-up runs that send `text`, lead-in and tail included.
[[nodiscard]] Expected<std::vector<CwKeyRun>> cw_key_runs(const CwModConfig& config,
                                                          std::string_view text);

// Renders those runs as the analytic signal at the tone.
[[nodiscard]] Expected<std::vector<Complex32>> cw_render_analytic(const CwModConfig& config,
                                                                  std::string_view text);

}  // namespace revenant::siggen
