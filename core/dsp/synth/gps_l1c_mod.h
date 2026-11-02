// GPS L1C CNAV-2 frames and the three signals a satellite puts on L1 for the
// L1C decoder: C/A, the L1C pilot with its overlay, and the L1C data
// channel. Written from IS-GPS-800J and IS-GPS-200N, as gps_l1c.h lists.
//
// POWER
//
// IS-GPS-800J Table 3.2-1 gives the terrestrial minimum received power as
// -158.25 dBW for L1CP and -163 dBW for L1CD; IS-GPS-200N Table 3-Va gives
// -158.5 dBW for C/A. The pilot and data C/N0 are set from the C/A one by
// those differences, +0.25 dB and -4.5 dB.
//
// TIMING
//
// The L1C code epoch falls on a C/A code epoch (IS-GPS-800J 3.2.2.1), here
// `epoch_offset` C/A periods after the C/A epoch nearest sample 0, and the
// first `lead_periods` 10 ms periods are the tail of frames[0], so a whole
// frame starts at period `lead_periods`. The pilot is BOC(1,1) throughout;
// see gps_l1c.h for why the BOC(6,1) part of TMBOC is not modelled.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/gps_l1c.h"
#include "core/dsp/synth/gnss_mod.h"

namespace revenant::siggen {

// The 1800 symbols of one frame: 576 subframe 2 and 250 subframe 3 data
// bits, CRC-24Q appended to each, LDPC encoded, interleaved, behind the TOI.
[[nodiscard]] std::vector<std::uint8_t> l1c_frame_symbols(unsigned toi,
                                                          std::span<const std::uint8_t> sf2,
                                                          std::span<const std::uint8_t> sf3);

struct L1cSatellite {
    unsigned prn = 1;
    double ca_code_phase_chips = 0.0;  // C/A, at sample 0, 0 to 1023
    unsigned epoch_offset = 0;         // 0 to 9
    unsigned lead_periods = 0;         // below 1800
    double doppler_hz = 0.0;
    double ca_cn0_dbhz = 40.0;
    std::vector<std::vector<std::uint8_t>> frames;  // each 1800 symbols
};

// C/A, pilot and data, in that order.
[[nodiscard]] std::vector<GnssSatelliteSpec> l1c_satellite(const L1cSatellite& sat);

}  // namespace revenant::siggen
