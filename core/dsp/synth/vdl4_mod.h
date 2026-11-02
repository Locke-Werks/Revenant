// A VDL Mode 4 transmitter, from the same clauses of ETSI EN 301 842-1 as
// core/decode/vdl4.h, and carrying the same assumption about the HDLC
// framing that file states. It is core/dsp/synth/ais_mod.h at 19 200 bit/s,
// modulation index 0.25 (2 400 Hz) and BT 0.28, clause 5.2; the packet bits
// are ais_packet_bits()'s, whose training sequence of alternating bits
// starting with a zero is clause 5.2's "0101 ... 0101".

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/vdl4.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct Vdl4ModConfig {
    dsp::SampleRate rate = 96000;
    double amplitude = 1.0;
    double bit_rate_error = 0.0;
    double carrier_offset_hz = 0.0;
};

// Each burst's octets, framed with flags and FCS, rendered as GFSK bursts
// with the carrier off for 64 bits around each.
[[nodiscard]] Expected<std::vector<dsp::Complex32>> vdl4_render(
    const Vdl4ModConfig& config, std::span<const std::vector<std::uint8_t>> bursts);

}  // namespace revenant::siggen
