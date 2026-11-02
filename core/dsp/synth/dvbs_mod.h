// A DVB-S transmitter for the round trip tests of core/decode/dvbs.h, plus
// builders for the PAT, PMT and SDT sections it carries.
//
// ETSI EN 300 421 V1.1.2 (1997-08), clauses 4.4 and 4.5, in transmit order:
// energy dispersal, RS(204,188), the I=12 convolutional interleaver, the
// punctured K=7 code of table 2 and Gray QPSK per figure 5, shaped by
// core/dsp/synth/psk_mod.h. Sections follow ITU-T H.222.0 (05/2006) 2.4.4.3
// and 2.4.4.8 and ETSI EN 300 468 V1.17.1 5.2.3 and 6.2.33.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/decode/dvbs.h"
#include "core/dsp/synth/psk_mod.h"
#include "core/error.h"

namespace revenant::siggen {

using TsPacketBytes = std::array<std::uint8_t, decode::kTsPacketBytes>;

// QPSK symbol indices, for psk_render, carrying `packets` (whole groups of
// eight are not required; energy dispersal restarts every eight packets
// counted from the first). The convolutional encoder starts at state zero and
// is not flushed, as on a continuous carrier.
[[nodiscard]] std::vector<std::uint8_t> dvbs_symbols(std::span<const TsPacketBytes> packets,
                                                     decode::DvbsCodeRate rate);

struct DvbsModConfig {
    PskModConfig psk;  // modulation is forced to QPSK
    decode::DvbsCodeRate rate = decode::DvbsCodeRate::R1_2;
};

[[nodiscard]] Expected<std::vector<Complex32>> dvbs_render(const DvbsModConfig& config,
                                                           std::span<const TsPacketBytes> packets);

// A null packet, PID 0x1FFF, H.222.0 2.4.3.3.
[[nodiscard]] TsPacketBytes ts_null_packet();

// Splits a section into TS packets on `pid`, pointer_field zero, stuffed with
// 0xFF. `continuity` is advanced per packet.
[[nodiscard]] std::vector<TsPacketBytes> ts_section_packets(std::uint16_t pid,
                                                            std::span<const std::uint8_t> section,
                                                            std::uint8_t& continuity);

// Whole sections including CRC_32.
[[nodiscard]] std::vector<std::uint8_t> ts_build_pat(std::uint16_t transport_stream_id,
                                                     std::uint8_t version,
                                                     std::span<const decode::TsProgram> programs);
[[nodiscard]] std::vector<std::uint8_t> ts_build_pmt(
    std::uint16_t program_number, std::uint8_t version, std::uint16_t pcr_pid,
    std::span<const decode::TsElementaryStream> streams);
[[nodiscard]] std::vector<std::uint8_t> ts_build_sdt(std::uint16_t transport_stream_id,
                                                     std::uint16_t original_network_id,
                                                     std::uint8_t version,
                                                     std::span<const decode::TsService> services);

}  // namespace revenant::siggen
