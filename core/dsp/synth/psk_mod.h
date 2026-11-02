// A root raised cosine M-PSK transmitter, for exercising the demodulator in
// core/decode/psk_demod.h.
//
// Renders complex baseband directly: each symbol's constellation point (from
// decode::psk_point, so both ends share one convention) weights a root raised
// cosine pulse evaluated in closed form at the exact sample times, which is
// what lets a timing offset be any fraction of a sample and a symbol rate be
// any ratio to the sample rate. OQPSK sends the quadrature rail half a symbol
// late. A carrier offset and starting phase are applied last.
//
// The closed form is the textbook one (Proakis, Digital Communications, 5th
// ed., section 9.2), with the removable singularities at t = 0 and
// t = +/- T/(4*alpha) taken by their limits.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/psk_demod.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::SampleRate;

struct PskModConfig {
    SampleRate rate = 48000;
    double symbol_rate = 4800.0;
    decode::PskModulation modulation = decode::PskModulation::Qpsk;
    double roll_off = 0.35;
    // Pulse truncation, symbols either side of its centre.
    std::size_t half_span_symbols = 8;
    double carrier_offset_hz = 0.0;
    double initial_phase = 0.0;  // radians
    // Delay of the first symbol's centre, in samples, beyond the pulse's half
    // span. Fractional values are exact.
    double timing_offset_samples = 0.0;
    // Fractional symbol clock error: positive makes symbols longer.
    double clock_error = 0.0;
    double amplitude = 1.0;
};

// Root raised cosine pulse at time t in symbol periods, peak 1 at t = 0 for
// roll_off 0 and slightly above (1 - alpha + 4 alpha/pi) otherwise.
[[nodiscard]] double rrc_pulse(double t_symbols, double roll_off);

// Renders symbol indices (taken modulo the order) as complex baseband. The
// output spans every pulse's whole support.
[[nodiscard]] Expected<std::vector<Complex32>> psk_render(const PskModConfig& config,
                                                          std::span<const std::uint8_t> symbols);

}  // namespace revenant::siggen
