// A PSK31 and PSK63 transmitter, written from the same article as the
// receiver in core/decode/psk31.h.
//
// WHY IT EXISTS
//
// The reason core/dsp/synth/dv_mod.h gives: a decoder with no GPU kernel has
// no bit-exact scalar twin, and a round trip through a transmitter written
// from the same clauses is what stands in its place. Where the article leaves
// something unstated, the pulse shape above all, the choice is made once in
// core/decode/psk31.h and both ends follow it, so the round trip is blind to
// it and the header there says so.
//
// WHAT IT RENDERS
//
// Audio, the way a sound card feeds an SSB transmitter: a tone at `tone_hz`
// carrying the modulation. The analytic form, a complex exponential at the
// tone frequency, is rendered first and the audio is its real part.
//
// NOISE ON AUDIO IS NOT NOISE ON THE ANALYTIC SIGNAL, BY 3 DB
//
// The channel simulator in core/dsp/synth/channel.h works on complex
// samples, and the obvious route, calibrated complex noise on the analytic
// signal and then the real part, delivers audio 3.01 dB worse than the level
// asked for. The real part halves the signal's power and halves the noise's,
// but the noise's half is spread over the positive frequencies only, fs/2
// wide instead of fs, so its density per hertz is unchanged while the
// signal's power has halved. Put another way, the real part folds the
// complex noise at -f onto +f, and the signal has nothing at -f to fold.
//
// This was found by measurement, not foreseen: the first PSK31 error rates
// came out a steady 3 dB worse than differential BPSK theory allows, and the
// realised audio signal to noise ratio, measured by differencing noisy audio
// against clean, was 3.0 dB under the figure passed in.
// analytic_to_noisy_audio below compensates, and
// tests/decode/test_psk31.cpp measures the audio it produces.
//
// THE CONSTANTS LIVE IN THE DECODER
//
// Every specified value is included from core/decode/psk31.h and
// core/decode/varicode.h, never restated, so the two ends cannot drift apart
// in a way the round trip would hide.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/decode/psk31.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::Hertz;
using dsp::SampleRate;

struct Psk31ModConfig {
    SampleRate rate = 48000;

    // Audio frequency of the carrier. Integer hertz, per docs/conventions.md.
    Hertz tone_hz = 1000;

    decode::Psk31Mode mode = decode::Psk31Mode::Bpsk31;

    // Peak amplitude of the carrier.
    double amplitude = 1.0;

    // QEX page 6: a transmission starts with idle, continuous zeros, so that
    // "the timing will pull into sync quickly", and ends with a tail of
    // unmodulated carrier. NOT SPECIFIED LENGTHS: 64 symbols of each is two
    // seconds of PSK31. The receiver's acquisition in core/decode/psk31.h
    // wants a couple of seconds of signal and its timing and Viterbi stages
    // hold back their last window, so a transmission shorter than this at
    // either end loses characters to the receiver's latency rather than to
    // noise.
    std::size_t preamble_symbols = 64;
    std::size_t postamble_symbols = 64;
};

// The data bits of a whole transmission: preamble zeros, the text in
// Varicode with its two-zero gaps, and the postamble ones.
[[nodiscard]] Expected<std::vector<std::uint8_t>> psk31_message_bits(const Psk31ModConfig& config,
                                                                     std::string_view text);

// The phase shift of each symbol, in the quarter turns of
// core/decode/psk31.h, for a run of data bits. Binary per QEX page 6; QPSK
// through the page 9 table with the encoder register starting from zero,
// which is the state the idle sequence leaves it in.
[[nodiscard]] std::vector<std::uint8_t> psk31_symbol_shifts(decode::Psk31Mode mode,
                                                            std::span<const std::uint8_t> bits);

// Renders data bits as the analytic signal at the tone, one symbol per bit.
[[nodiscard]] Expected<std::vector<Complex32>> psk31_render_analytic(
    const Psk31ModConfig& config, std::span<const std::uint8_t> bits);

// The real part of a rendered analytic signal: what a receiver's demodulator
// hands to its audio sink.
[[nodiscard]] std::vector<float> analytic_to_audio(std::span<const Complex32> analytic);

// The real part after adding white noise, calibrated so the AUDIO has the
// stated signal to noise ratio in `reference_bandwidth_hz` of positive
// frequency: signal power over the noise power in that many hertz of the
// one-sided audio spectrum, which is how docs/snr-convention.md's 2500 Hz
// figure reads for a receiver's audio. The analytic signal is not modified.
[[nodiscard]] Expected<std::vector<float>> analytic_to_noisy_audio(
    std::span<const Complex32> analytic, double snr_in_reference_bandwidth_db,
    Hertz reference_bandwidth_hz, SampleRate rate, std::uint64_t seed);

}  // namespace revenant::siggen
