// An MF/HF DSC transmitter's J2B audio, written from the same clauses of
// ITU-R M.493-16 Annex 1 as core/decode/dsc_hf.h.
//
// The characters on the air are core/dsp/synth/dsc_mod.h's: Figure 1 b)'s
// slot order does not change with band. What changes is clause 1.3.1's tones
// and rate and clause 3.4's dot pattern, which is 200 bits for most MF/HF
// calls where VHF always sends 20. The audio is what goes into an SSB
// transmitter's microphone input and what comes out of a receiver on the same
// sideband: no pre-emphasis, both tones at one level, silence between calls.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/dsc_hf.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

// Clause 3.4's dot pattern of `dot_bits` bits, then each slot's ten bits per
// clause 1.1.1, Y as 1.
[[nodiscard]] std::vector<std::uint8_t> dsc_hf_bits(std::span<const std::uint8_t> information,
                                                    std::size_t dot_bits);

struct DscHfModConfig {
    dsp::SampleRate rate = 48000;
    decode::DscHfSideband sideband = decode::DscHfSideband::Usb;
    double amplitude = 0.5;
    // Silence before, between and after calls, in bits.
    std::size_t lead_bits = 20;
    std::size_t gap_bits = 20;
    std::size_t tail_bits = 20;
    // Clause 1.3.1 allows 30 ppm.
    double bit_rate_error = 0.0;
};

// Continuous-phase two-tone audio, one call's bits after another.
[[nodiscard]] Expected<std::vector<float>> dsc_hf_render_audio(
    const DscHfModConfig& config, std::span<const std::vector<std::uint8_t>> calls_bits);

}  // namespace revenant::siggen
