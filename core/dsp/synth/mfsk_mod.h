// Transmitter for M-ary FSK audio, the counterpart of core/decode/mfsk_demod.h.
//
// WHY THIS EXISTS
//
// The reason core/dsp/synth/fsk_mod.h gives: the demodulator has no GPU twin,
// and a round trip through a transmitter is what stands in for one. This one
// knows nothing of any mode. It turns a tone index sequence into audio; the
// mode files above it decide what the sequence is.
//
// WHAT IT PRODUCES
//
// Real audio from a single phase-continuous oscillator whose frequency steps
// between tones, optionally smoothed by a Gaussian filter on the frequency
// trajectory (GFSK, the textbook construction with the filter's
// bandwidth-time product as the parameter). A constant offset is a different
// base_hz; drift_hz_per_s adds a linear ramp so a test can exercise the AFC.

#pragma once

#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::SampleRate;

struct MfskModConfig {
    SampleRate rate = 8000;
    int tones = 16;
    double tone_spacing_hz = 15.625;
    double symbol_rate = 15.625;
    // Frequency of tone 0 at the first symbol.
    double base_hz = 1000.0;
    double amplitude = 0.5;

    // Gaussian frequency smoothing, as the filter's bandwidth-time product.
    // Zero keys the tones with no smoothing.
    double gaussian_bt = 0.0;

    // Linear frequency ramp, from the first symbol on.
    double drift_hz_per_s = 0.0;

    // Silence before the first symbol and after the last, in samples.
    // Fractional, so a test can put the symbol boundaries anywhere.
    double lead_in_samples = 0.0;
    double tail_samples = 0.0;
};

[[nodiscard]] Expected<std::vector<float>> mfsk_render(const MfskModConfig& config,
                                                       std::span<const int> tone_sequence);

}  // namespace revenant::siggen
