// GPS CNAV messages and the L2C and L5 signals that carry them, from
// IS-GPS-200N and IS-GPS-705J as gps_cnav.h lists.
//
// L2C is CM with the symbols and CL with none, time multiplexed: CM in the
// first 1.023 MHz slot of each pair, CL in the second. L5 is I5 with NH10 and
// the symbols, and Q5 with NH20 a quarter cycle behind it in carrier phase
// (IS-GPS-705J Table 3-II puts them in quadrature). Both get the C/N0 given;
// L2C's is the whole multiplexed signal's.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/gps_cnav.h"
#include "core/dsp/synth/gnss_mod.h"

namespace revenant::siggen {

// A 300-bit message: the header of IS-GPS-200N 30.3.3, bits 39 to 276 from
// `payload` (238 bits, or zeros when shorter), CRC-24Q in 277 to 300.
[[nodiscard]] std::vector<std::uint8_t> cnav_message(unsigned prn, unsigned type, unsigned tow,
                                                     bool alert,
                                                     std::span<const std::uint8_t> payload);

// Message type 10's WN, top and toe placed in a payload, the rest from `fill`.
[[nodiscard]] std::vector<std::uint8_t> cnav_type10_payload(unsigned wn, unsigned top,
                                                            unsigned toe,
                                                            std::span<const std::uint8_t> fill);

// Rate 1/2 K = 7 symbols for a continuous bit stream, register from zero.
[[nodiscard]] std::vector<std::uint8_t> cnav_encode(std::span<const std::uint8_t> bits);

struct CnavSatellite {
    unsigned prn = 1;
    double code_phase_chips = 0.0;  // of the band's primary code, at sample 0
    double doppler_hz = 0.0;
    double cn0_dbhz = 40.0;
    std::vector<std::uint8_t> symbols;  // FEC symbols
};

// CM then CL.
[[nodiscard]] std::vector<GnssSatelliteSpec> l2c_satellite(const CnavSatellite& sat);
// I5 then Q5. Symbols begin on a code period whose index is a multiple of 10.
[[nodiscard]] std::vector<GnssSatelliteSpec> l5_satellite(const CnavSatellite& sat);

}  // namespace revenant::siggen
