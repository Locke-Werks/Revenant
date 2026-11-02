// A CCSDS telemetry transmitter: Space Packets into TM Transfer Frames, then
// Reed-Solomon, the pseudo-randomiser, the Attached Sync Marker and the
// convolutional code, giving channel bits for core/dsp/synth/psk_mod.h.
//
// SPECIFICATION
//
// The same four Blue Books as core/decode/ccsds.h, which says what each
// clause is: 131.0-B-5, 132.0-B-3, 732.0-B-4 and 133.0-B-2. The order of
// operations at the sending end is 131.0 figure 2-2 and 10.2.2: Reed-Solomon,
// then randomisation, then the marker, then the convolutional code over the
// marker and the randomised codeblock (9.2.1.4), then NRZ-M if used, which
// 131.0 3.3.3 b) places at the convolutional encoder's input.
//
// CLEAN ROOM
//
// Written from those documents. No CCSDS implementation was read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/ccsds.h"
#include "core/error.h"

namespace revenant::siggen {

struct CcsdsTxConfig {
    bool reed_solomon = false;
    decode::CcsdsRsConfig rs;
    decode::CcsdsRandomizer randomizer = decode::CcsdsRandomizer::Legacy255;
    bool convolutional = false;
    bool nrzm = false;
};

// Channel bits for a run of Transfer Frames, back to back with a marker
// before each (131.0 9.4). Every frame must be the same length, and with
// Reed-Solomon on that length must be CcsdsReedSolomon::data_octets(). With
// the convolutional code on, the encoder starts from zero and `tail_bits`
// zero bits are encoded after the last frame so a receiver's traceback can
// release it.
[[nodiscard]] Expected<std::vector<std::uint8_t>> ccsds_channel_bits(
    const CcsdsTxConfig& config, std::span<const std::vector<std::uint8_t>> frames,
    std::size_t tail_bits = 64);

// Packs Space Packets back to back into the data fields of successive TM
// Transfer Frames of one virtual channel, setting each frame's First Header
// Pointer per 132.0 4.1.2.7.6 and its counts from `first`, and appending the
// FECF when `fecf` is set. The last frame is completed with an Idle Packet
// (133.0 4.1.3.3.4.4) when there is room for one, else with idle octets.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> ccsds_pack_tm_frames(
    const decode::TmFrameHeader& first, std::size_t frame_octets, bool fecf,
    std::span<const std::vector<std::uint8_t>> packets);

// A Space Packet: header from `header` with its length set from `data`.
[[nodiscard]] std::vector<std::uint8_t> ccsds_space_packet(decode::SpacePacketHeader header,
                                                           std::span<const std::uint8_t> data);

}  // namespace revenant::siggen
