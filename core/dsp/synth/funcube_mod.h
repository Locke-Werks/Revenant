// A FUNcube telemetry transmitter: 256-octet frames to channel symbols for
// core/dsp/synth/psk_mod.h at 1200 symbols a second.
//
// SPECIFICATION
//
// As core/decode/funcube.h: the FUNcube-1 Downlink Data document, Issue 1.0
// as flown, and Phil Karn's AO-40 coded telemetry proposal v1.2, which
// funcube.h says how each is used. The block coding is
// decode::funcube_encode_block; this file adds the frame builder and the
// differential encoder.
//
// CLEAN ROOM
//
// No FUNcube or AO-40 implementation was read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/funcube.h"

namespace revenant::siggen {

// Channel symbols for consecutive frames, each 256 octets, back to back.
// With `differential`, a one is sent as a change of phase.
[[nodiscard]] std::vector<std::uint8_t> funcube_symbols(std::span<const std::vector<std::uint8_t>> frames,
                                                        bool differential = true);

// Packs a frame from a header, real-time values and 200 payload octets, in
// the layout decode::funcube_parse reads.
[[nodiscard]] std::vector<std::uint8_t> funcube_build_frame(std::uint8_t satellite_id, std::uint8_t frame_type,
                                                            const decode::FuncubeRealTime& real_time,
                                                            std::span<const std::uint8_t> payload);

}  // namespace revenant::siggen
