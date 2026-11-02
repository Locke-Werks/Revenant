// A dPMR transmitter, written from the same documents as the decoder in
// core/decode/dpmr.h, ETSI TS 102 490 V1.9.1 and TS 102 658 V2.6.1.
//
// Every specified constant and every coding step is included from the
// decoder's header rather than restated, so the round trip checks the
// receiver against the coding chain, not against a second reading of the
// document. What it cannot check is that reading itself; see the decoder's
// DEVIATIONS.
//
// WHAT IT SENDS
//
// One transmission per 490 4.2.3: a Header (preamble of 490 6.1.6, FS1, HI0,
// channel code, HI1), whole superframes of four payload frames with the IDs
// spread across the CCH per 490 9.1, and the End frame. TCH blocks are
// whatever the caller supplies; no vocoder is run.
//
// THE SHAPING FILTER HAS UNIT GAIN AT ZERO HERTZ, for the reason
// core/dsp/synth/m17_mod.h gives: 490 12.2.2.1 states the deviation of each
// symbol, which only a filter of unit DC gain reproduces.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/dpmr.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::SampleRate;

struct DpmrModConfig {
    SampleRate rate = 48000;
    double amplitude = 1.0;
};

using DpmrTchFrame =
    std::array<std::array<std::uint8_t, decode::kDpmrTchBits / 8>, decode::kDpmrTchBlocks>;

struct DpmrMessage {
    // 490 8.1.1: an ISF address is the common ID then sixteen ones.
    std::uint32_t called_id = decode::dpmr_isf_address(7);
    std::uint32_t own_id = decode::dpmr_isf_address(7);
    std::uint8_t mode = 0;    // 490 5.7: voice
    std::uint8_t format = 1;  // 490 5.8: peer to peer
    std::uint8_t header_type = 0;  // 490 5.11: communication start
    std::uint16_t channel_code = 0x145;  // 490 Table 6.1, 446.003125 MHz, group A
    std::uint32_t slow_data = 0;
    // One per payload frame; rounded up to whole superframes with zeros.
    std::vector<DpmrTchFrame> payload;
    decode::DpmrEnd end{};
};

// The whole transmission, in sending order, as the integers of 490 Table 12.1.
[[nodiscard]] std::vector<int> dpmr_symbols(const DpmrMessage& message);

// Renders symbols as 4FSK complex baseband per 490 12.2.2. The rate must be
// a whole multiple of 2400.
[[nodiscard]] Expected<std::vector<Complex32>> dpmr_render_symbols(const DpmrModConfig& config,
                                                                   std::span<const int> symbols);

[[nodiscard]] Expected<std::vector<Complex32>> dpmr_render(const DpmrModConfig& config,
                                                           const DpmrMessage& message);

}  // namespace revenant::siggen
