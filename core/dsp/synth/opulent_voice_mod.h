// An Opulent Voice frame source, from the same document as
// core/decode/opulent_voice.h, and an antipodal bit channel to carry it.
//
// The frame layer only. The decoder's header says why there is no MSK here:
// the document's modulation does not add up and docs/modes.md says to ask ORI
// before writing one. opv_render_antipodal puts one bit on one complex sample
// at +1 or -1, so siggen::add_awgn can be applied at a stated SNR and the real
// part read back as soft bits. Its SNR is therefore Es/N0 per coded bit, not
// anything an on-air measurement would report.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/opulent_voice.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;

// Frames in order, each the 2168 transmitted bits of OPV 8.7, concatenated.
[[nodiscard]] Expected<std::vector<std::uint8_t>> opv_frame_bits(
    std::span<const std::array<std::uint8_t, decode::kOpvFrameBytes>> frames);

// A frame of OPV 2.1: station, the default token, zero reserved, payload.
[[nodiscard]] Expected<std::array<std::uint8_t, decode::kOpvFrameBytes>> opv_make_frame(
    std::string_view station, std::span<const std::uint8_t> payload);

// Bit 0 to +1, bit 1 to -1, the dv_codes soft sign convention.
[[nodiscard]] std::vector<Complex32> opv_render_antipodal(std::span<const std::uint8_t> bits);

}  // namespace revenant::siggen
