// A VDES ASM-TER transmitter, from the same clauses of ITU-R M.2092-2 as
// core/decode/vdes.h: a payload and its CRC-32 scrambled, mapped to the
// alternating pi/4-QPSK of Figure 11 behind the Table 1 syncword and the
// Table 3 Link ID, and shaped by a root raised cosine of roll-off 0.35.
//
// The ramps are four symbols each, Table 7. Clause A2-1.2.3.1 leaves the
// modulation during the ramp unspecified; this sends the syncword's first
// set of points with a raised cosine envelope. The ten bits Table 7 adds
// after the CRC of an uncoded burst are zeros, scrambled; core/decode/vdes.h
// says why that choice cannot matter to a receiver. Link IDs 5 to 7 pass
// payload and CRC through the clause A2-1.2.4 turbo encoder before the
// scrambler, Figure 6.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/vdes.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct VdesAsmModConfig {
    // A whole number of samples per symbol.
    dsp::SampleRate rate = 48000;
    double amplitude = 1.0;
    double carrier_offset_hz = 0.0;
    // Silence before, between and after bursts, in symbols.
    std::size_t gap_symbols = 64;
};

struct VdesAsmBurstSpec {
    // 1, 2 or 3 (uncoded) or 5, 6 or 7 (turbo coded).
    unsigned link_id = 1;
    // Exactly the format's packet bits / 8 - 4 octets.
    std::vector<std::uint8_t> payload;
};

// The header of Tables 26 to 28 followed by application data, zero filled to
// the format's payload length.
[[nodiscard]] Expected<std::vector<std::uint8_t>> vdes_asm_build_payload(
    unsigned link_id, const decode::VdesAsmMessage& header,
    std::span<const std::uint8_t> application_data);

[[nodiscard]] Expected<std::vector<dsp::Complex32>> vdes_asm_render(
    const VdesAsmModConfig& config, std::span<const VdesAsmBurstSpec> bursts);

}  // namespace revenant::siggen
