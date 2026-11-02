// Meteor-M LRPT: complex baseband to VCDUs and instrument Space Packets.
//
// SPECIFICATION
//
// There is no Roshydromet interface document for LRPT that this project has
// been able to obtain (docs/modes.md, "Image and weather"). What is
// implemented is the CCSDS framing the mode is built on, from documents only:
//
//   - CCSDS 131.0-B-5 (September 2023): the marker 0x1ACFFC1D (9.3), the
//     basic rate 1/2 K = 7 code with G2 inverted (3.3), the 255-bit
//     pseudo-randomiser (10.4.2) and RS(255,223) at depth 4 in the dual basis
//     (4.3), through core/decode/ccsds_downlink.h;
//   - CCSDS 732.0-B-4 (October 2021): the AOS frame header and M_PDU;
//   - CCSDS 133.0-B-2 (June 2020): Space Packets.
//
// The parameters that choose among those options are from docs/modes.md's
// LRPT row, which is a summary and not a document: QPSK at 72 ksym/s (80
// ksym/s on some passes), r = 1/2 K = 7, four interleaved RS(255,223), a
// 1020-octet coded VCDU after the marker. That the code is the basic one
// with the G2 inversion of 131.0 3.3, that the randomiser is the 255-bit
// one, and that the VCDU has no insert zone, are this decoder's defaults,
// chosen because they are the Blue Book's, and are all configuration: none
// has been checked against a recording, because none was available. Some
// spacecraft are reported to send offset QPSK; `oqpsk` selects it.
//
// WHERE IT STOPS
//
// At Space Packets, sorted by APID. The MSU-MR imager's packet payload (its
// segmenting, the DCT and its quantisation tables) has no obtainable
// document, and docs/clean-room.md allows only derivation by observation of
// real frames, labelled as such. No recording was available, so nothing of
// the payload is decoded and no APID is given an instrument name: APIDs are
// counted and reported as numbers.
//
// CLEAN ROOM
//
// No LRPT decoder was read: not SatDump, meteor_demod, meteor_decoder,
// LeanHRPT or any other.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>

#include "core/decode/ccsds_downlink.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

inline constexpr double kLrptSymbolRate = 72000.0;
inline constexpr double kLrptSymbolRateFast = 80000.0;

struct LrptConfig {
    double sample_rate = 288000.0;
    double symbol_rate = kLrptSymbolRate;
    bool oqpsk = false;
    bool g2_inverted = true;
    std::size_t insert_zone_octets = 0;
    // Not given by any document in hand; the RRC demodulator's default.
    double roll_off = 0.6;
};

struct LrptApidStats {
    std::size_t packets = 0;
    std::size_t octets = 0;
    std::uint8_t virtual_channel = 0;
};

struct LrptEvents {
    std::function<void(const CcsdsDownlinkFrame&)> on_frame;
    std::function<void(std::uint8_t virtual_channel, const SpacePacket&)> on_packet;
};

class LrptDecoder {
public:
    [[nodiscard]] static Expected<LrptDecoder> create(const LrptConfig& config, LrptEvents events);

    void push(std::span<const dsp::Complex32> samples);
    void finish();

    [[nodiscard]] const CcsdsDownlinkStats& stats() const { return link_->stats(); }
    [[nodiscard]] const std::map<std::uint16_t, LrptApidStats>& apids() const { return *apids_; }
    [[nodiscard]] bool locked() const { return link_->frame_locked(); }
    [[nodiscard]] double frequency_offset_hz() const { return link_->frequency_offset_hz(); }

private:
    LrptDecoder() = default;
    // Held by pointer so the packet callback's capture stays valid when the
    // decoder is moved.
    std::unique_ptr<std::map<std::uint16_t, LrptApidStats>> apids_;
    std::unique_ptr<CcsdsDownlink> link_;
};

}  // namespace revenant::decode
