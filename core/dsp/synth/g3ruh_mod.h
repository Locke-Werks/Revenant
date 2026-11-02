// A G3RUH 9600 bit/s modem's transmit audio, written from the same article as
// core/decode/g3ruh.h.
//
// The bits are core/dsp/synth/fsk_mod.h's hdlc_bits, then NRZI as a TNC
// applies it, then Miller's scrambler. The waveform stands in for the
// article's EPROM FIR, which it specifies only by its response, "-6 db at
// 4800 Hz, -60db at 7500 Hz": a Blackman-windowed sinc low-pass with its
// half-amplitude point at 4800 Hz, applied to rectangular bits. That is the
// audio applied to the varactor, and also what an ideal nfm receiver's
// discriminator gives back; `g3ruh_render_baseband` is the FM signal itself.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/g3ruh.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct G3ruhModConfig {
    dsp::SampleRate rate = 48000;
    double baud = decode::kG3ruhBaud;
    double amplitude = 0.5;
    // Flags before, between and after frames.
    std::size_t leading_flags = 32;
    std::size_t flags_between = 4;
    std::size_t trailing_flags = 4;
    // The article's +/- 3 kHz, for the RF rendering.
    double deviation_hz = 3000.0;
    double baud_error = 0.0;
};

// Flags and stuffed frames, NRZI, scrambled: the bits on the air.
[[nodiscard]] std::vector<std::uint8_t> g3ruh_bits(const G3ruhModConfig& config,
                                                   std::span<const std::vector<std::uint8_t>> frames);

[[nodiscard]] Expected<std::vector<float>> g3ruh_render_audio(const G3ruhModConfig& config,
                                                              std::span<const std::uint8_t> bits);

[[nodiscard]] Expected<std::vector<dsp::Complex32>> g3ruh_render_baseband(
    const G3ruhModConfig& config, std::span<const std::uint8_t> bits);

}  // namespace revenant::siggen
