// A VHF DSC transmitter, written from the same clauses of ITU-R M.493-15
// Annex 1 as core/decode/dsc.h.
//
// WHY THIS EXISTS
//
// The reason core/dsp/synth/fsk_mod.h gives, unchanged. The one thing a round
// trip cannot see is a misreading both ends share, and the ten-bit code is
// where that would matter most; tests/decode/test_dsc.cpp checks the code
// against Table A1-1 as printed, by fingerprint, for that reason.
//
// WHAT IT PRODUCES
//
// Either the audio an FM receiver would make of channel 70, the two tones of
// clause 1.3.2, or the RF signal itself as complex baseband: clause 1.3.2's
// frequency modulation with 6 dB per octave of pre-emphasis, which it calls
// phase modulation, at index 2.0.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/dsc.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::Hertz;
using dsp::SampleRate;

// The information characters of a call, both format specifiers to the end of
// sequence, from the fields decode::dsc_parse fills: Tables A1-4.1 to A1-4.9
// in reverse. The format and the category choose the layout; for a call
// other than a distress alert, relay or acknowledgement, Message 2 is the
// `frequencies` elements when there are any and `message_symbols` otherwise.
// Fails on a format clause 4.1 does not list, and on an identity of more
// than nine digits.
[[nodiscard]] Expected<std::vector<std::uint8_t>> dsc_encode(const decode::DscCall& call);

// Table A1-5 in reverse: one three-character element.
[[nodiscard]] Expected<std::array<std::uint8_t, 3>> dsc_frequency_element(
    const decode::DscFrequency& element);

// The characters on the air, slot by slot, per clause 3.2 and Figure 1 b):
// DX and RX alternating, the six DX phasing characters and the eight RX ones,
// the information characters in DX from slot 12 and again in RX five slots
// later, the error-check character of clause 10.2 after the end of sequence
// in both, and the end of sequence twice more in DX, clause 9.
[[nodiscard]] std::vector<std::uint8_t> dsc_air_symbols(std::span<const std::uint8_t> information);

// Clause 3.4.2's 20-bit dot pattern, then each slot's ten bits per clause
// 1.1.1, Y as 1.
[[nodiscard]] std::vector<std::uint8_t> dsc_bits(std::span<const std::uint8_t> information);

struct DscModConfig {
    SampleRate rate = 48000;
    Hertz y_hz = decode::kDscVhfYHz;
    Hertz b_hz = decode::kDscVhfBHz;
    double bit_rate = decode::kDscVhfBitRate;

    // Peak level of the 2100 Hz tone in the audio; the 1300 Hz tone is
    // below it by the pre-emphasis when that is on.
    double amplitude = 0.5;

    // Clause 1.3.2's 6 dB per octave: in the audio an FM receiver without
    // de-emphasis makes, each tone's level is proportional to its frequency.
    bool preemphasis = true;

    // Clause 1.3.2's index for the RF rendering.
    double modulation_index = decode::kDscVhfModulationIndex;

    // Unmodulated carrier before the first call, between calls and after the
    // last, in bits, as a radio keys up before the dot pattern.
    std::size_t lead_bits = 40;
    std::size_t gap_bits = 40;
    std::size_t tail_bits = 40;

    // Fractional error of the transmitter's bit clock; clause 1.3.2 allows
    // 30 ppm.
    double bit_rate_error = 0.0;
};

// Continuous-phase two-tone audio, one call's bits after another.
[[nodiscard]] Expected<std::vector<float>> dsc_render_audio(
    const DscModConfig& config, std::span<const std::vector<std::uint8_t>> calls_bits);

// The RF signal as complex baseband: the carrier's phase is the modulation
// index times the sine of the subcarrier's continuous phase.
[[nodiscard]] Expected<std::vector<Complex32>> dsc_render_baseband(
    const DscModConfig& config, std::span<const std::vector<std::uint8_t>> calls_bits);

}  // namespace revenant::siggen
