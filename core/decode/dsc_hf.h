// DSC on MF and HF: the audio of an SSB receiver on a DSC calling or working
// channel, to calls.
//
// SPECIFICATION
//
// Recommendation ITU-R M.493-16 (12/2023), "Digital selective-calling system
// for use in the maritime mobile service", Annex 1, read from the ITU's own
// publication service (itu.int/rec/R-REC-M.493) on 2026-11-02. Clauses used
// here beyond those core/decode/dsc.h lists: 1.3.1 (F1B or J2B, 170 Hz shift,
// 100 bit/s, the J2B audio centred on 1 700 Hz, and 1 700 +/- 85 Hz on working
// channels), 1.3.3 (receiver bandwidth not over 300 Hz), 1.4 (the higher tone
// is B, the lower Y) and 3.4.1 and 3.4.2 (the 200-bit and 20-bit dot patterns
// at MF and HF).
//
// Everything above the bit stream, the ten-bit code, the phasing, time
// diversity, the formats and the error-check character, is clause for clause
// the same at MF/HF as at VHF, so this file reuses core/decode/dsc.h's
// DscDecoder with the MF/HF tones and rate rather than writing it again.
// Clause 1.3.1 is the only physical layer difference that reaches the
// decoder; the dot pattern's length is clause 3.4 and the decoder finds
// phasing by character recognition whatever came before it (clause 3.1,
// Note 1).
//
// WHAT THIS TAKES AS INPUT
//
// The audio of a receiver in USB with its dial 1 700 Hz below the assigned
// frequency, which is how clause 1.3.1's J2B is offered to and taken from an
// SSB set, so Y is 1 615 Hz and B 1 785 Hz. An LSB receiver inverts the
// audio spectrum and exchanges the tones: `dsc_hf_config` takes the sideband.
// F1B on a calling channel is the same two RF frequencies and decodes from
// the same audio.
//
// CLEAN ROOM
//
// No DSC implementation was read.

#pragma once

#include <cstddef>

#include "core/decode/dsc.h"
#include "core/dsp/types.h"

namespace revenant::decode {

// Clause 1.3.1: centre 1 700 Hz, shift 170 Hz, 100 bit/s. Clause 1.4: the
// higher tone is B.
inline constexpr Hertz kDscHfCentreHz = 1700;
inline constexpr Hertz kDscHfShiftHz = 170;
inline constexpr Hertz kDscHfYHz = kDscHfCentreHz - kDscHfShiftHz / 2;  // 1 615
inline constexpr Hertz kDscHfBHz = kDscHfCentreHz + kDscHfShiftHz / 2;  // 1 785
inline constexpr double kDscHfBitRate = 100.0;

// Clause 3.4.1 and 3.4.2.
inline constexpr std::size_t kDscHfLongDotBits = 200;
inline constexpr std::size_t kDscHfShortDotBits = 20;

enum class DscHfSideband : unsigned char { Usb, Lsb };

// DscDecoder's configuration for MF/HF audio at `rate` from a receiver on
// `sideband`.
[[nodiscard]] DscConfig dsc_hf_config(SampleRate rate, DscHfSideband sideband = DscHfSideband::Usb);

// Clause 3.4: the dot pattern a call of this kind goes out with at MF/HF.
// 200 bits for distress alerts, distress acknowledgements, relays to a
// geographic area, relay acknowledgements to all ships, and calls addressed
// to ships; 20 bits for acknowledgements of individual calls and calls to
// coast stations. Which of the last two a call is cannot be told from the
// call alone, so `to_coast_or_acknowledgement` says it.
[[nodiscard]] std::size_t dsc_hf_dot_bits(const DscCall& call, bool to_coast_or_acknowledgement);

}  // namespace revenant::decode
