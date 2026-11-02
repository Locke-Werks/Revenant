// Metop AHRPT: complex baseband to VCDUs and instrument source packets, with
// each packet named by the instrument TD 18 assigns its VC and APID to.
//
// SPECIFICATION
//
// EUMETSAT, "TD 18 Metop Direct Readout AHRPT Technical Description",
// EUM/OPS/TEN/08/1663 v3A, 12 July 2021, fetched 2026-11-02 from
// user.eumetsat.int/s3/eup-strapi-media/TD_18_Metop_Direct_Readout_AHRPT_
// Technical_Description_v3_A_1cb789b653.pdf. Cited as "TD 18".
//
//   - Table 3: 1701.3 MHz (or 1707.0), 4.5 MHz bandwidth, 3.5 Mbit/s data
//     (4.666667 Mbit/s total coded), QPSK, FEC 3/4.
//   - Section 3.1 and figure 2: CADUs carrying CVCDUs, VCDUs, M_PDUs and
//     instrument source packets as CCSDS source packets; section 3.3: the
//     M_PDU packet zone is 884 octets.
//   - Table 1: VCID and APID of each instrument, which instrument_name()
//     carries.
//
// TD 18 defers protocol detail to the Metop Space to Ground Interface
// Specification, MO-IF-MMT-SY-0001, which is not publicly available, so the
// parameters TD 18 does not state are taken from the free documents that
// define the structures it names: the CADU, RS(255,223) depth 4, 255-bit
// randomiser and marker per CGMS LRIT/HRIT Global Specification 2.8 8.3
// (884 octets of packet zone fixes the 892-octet VCDU of that section),
// and the rate 3/4 code per CCSDS 131.0-B-5 3.4 and table 3-1, which has no
// symbol inversion. 3.5 Mbit/s over 3/4 over two bits a symbol gives the
// 2.333 Msym/s of docs/modes.md. The roll-off is not stated anywhere in hand.
// EPS.ASPI.DS.0675, which docs/modes.md names, did not resolve (2026-11-02).
//
// WHERE IT STOPS
//
// At instrument source packets. The AVHRR/3 packet layout (APIDs 103 and
// 104) is in MO-IF-MMT-SY-0001 and the AVHRR ICDs, none of which is free, so
// no AVHRR image is made. During data denial (TD 18, citing EPS/SYS/SPE/95413) packets are
// encrypted; they are delivered as received and nothing attempts to decrypt
// them.
//
// CLEAN ROOM
//
// No AHRPT decoder was read: not SatDump, AAPP's source, MetopDump or any
// other.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "core/decode/ccsds_downlink.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// TD 18 table 3 and the arithmetic above.
inline constexpr double kAhrptDataRate = 3.5e6;
inline constexpr double kAhrptSymbolRate = kAhrptDataRate / 0.75 / 2.0;

// TD 18 table 1: the instrument for a VC and APID, or an empty view.
[[nodiscard]] std::string_view ahrpt_instrument_name(std::uint8_t virtual_channel, std::uint16_t apid);

struct AhrptConfig {
    double sample_rate = 6.0e6;
    double symbol_rate = kAhrptSymbolRate;
    double roll_off = 0.5;
};

struct AhrptPacket {
    std::uint8_t virtual_channel = 0;
    std::string_view instrument;
    const SpacePacket* packet = nullptr;
};

struct AhrptEvents {
    std::function<void(const CcsdsDownlinkFrame&)> on_frame;
    std::function<void(const AhrptPacket&)> on_packet;
};

class AhrptDecoder {
public:
    [[nodiscard]] static Expected<AhrptDecoder> create(const AhrptConfig& config, AhrptEvents events);
    void push(std::span<const dsp::Complex32> samples);
    void finish();
    [[nodiscard]] const CcsdsDownlinkStats& stats() const { return link_->stats(); }
    [[nodiscard]] bool locked() const { return link_->frame_locked(); }

private:
    AhrptDecoder() = default;
    std::unique_ptr<CcsdsDownlink> link_;
};

}  // namespace revenant::decode
