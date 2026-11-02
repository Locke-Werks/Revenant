// GPS CNAV. See gps_cnav_mod.h.

#include "core/dsp/synth/gps_cnav_mod.h"

#include <array>

#include "core/decode/dv_codes.h"
#include "core/decode/gps_l1c.h"

namespace revenant::siggen {

using namespace revenant::decode::gnss;

namespace {

void put(std::vector<std::uint8_t>& v, unsigned first, unsigned count, unsigned value) {
    for (unsigned i = 0; i < count; ++i) {
        v[first - 1 + i] = static_cast<std::uint8_t>((value >> (count - 1 - i)) & 1U);
    }
}

}  // namespace

std::vector<std::uint8_t> cnav_message(unsigned prn, unsigned type, unsigned tow, bool alert,
                                       std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> m(300, 0);
    put(m, 1, 8, 0x8B);  // 10001011
    put(m, 9, 6, prn);
    put(m, 15, 6, type);
    put(m, 21, 17, tow);
    put(m, 38, 1, alert ? 1U : 0U);
    for (std::size_t i = 0; i < 238 && i < payload.size(); ++i) {
        m[38 + i] = payload[i];
    }
    const std::uint32_t crc = crc24q(std::span<const std::uint8_t>(m).first(276));
    put(m, 277, 24, crc);
    return m;
}

std::vector<std::uint8_t> cnav_type10_payload(unsigned wn, unsigned top, unsigned toe,
                                              std::span<const std::uint8_t> fill) {
    // Payload index 0 is message bit 39.
    std::vector<std::uint8_t> p(238, 0);
    for (std::size_t i = 0; i < p.size() && i < fill.size(); ++i) {
        p[i] = fill[i];
    }
    auto at = [&](unsigned first, unsigned count, unsigned value) {
        for (unsigned i = 0; i < count; ++i) {
            p[first - 39 + i] = static_cast<std::uint8_t>((value >> (count - 1 - i)) & 1U);
        }
    };
    at(39, 13, wn);
    at(55, 11, top);
    at(71, 11, toe);
    return p;
}

std::vector<std::uint8_t> cnav_encode(std::span<const std::uint8_t> bits) {
    static constexpr std::array<std::uint32_t, 2> kGen = {kCnavG1, kCnavG2};
    decode::ConvolutionalCode code;
    code.memory = 6;
    code.generators = kGen;
    auto r = decode::convolutional_encode(code, bits);
    return r ? *r : std::vector<std::uint8_t>{};
}

std::vector<GnssSatelliteSpec> l2c_satellite(const CnavSatellite& sat) {
    GnssSatelliteSpec cm;
    cm.code = l2cm_replica(sat.prn);
    cm.chip_rate_hz = 1.023e6;
    cm.carrier_hz = 1227.60e6;
    cm.doppler_hz = sat.doppler_hz;
    cm.code_phase_chips = sat.code_phase_chips;
    cm.cn0_dbhz = sat.cn0_dbhz;
    cm.periods_per_symbol = 1;
    cm.symbols = sat.symbols;

    GnssSatelliteSpec cl = cm;
    const auto clc = l2cl_code(sat.prn);
    cl.code.assign(clc.size() * 2, 0);
    for (std::size_t i = 0; i < clc.size(); ++i) {
        cl.code[2 * i + 1] = clc[i];
    }
    cl.symbols.clear();
    // CL restarts with CM every 75 periods; start it on the same epoch.
    cl.periods_per_symbol = 1;
    return {std::move(cm), std::move(cl)};
}

std::vector<GnssSatelliteSpec> l5_satellite(const CnavSatellite& sat) {
    GnssSatelliteSpec i5;
    i5.code = l5_code(sat.prn, false);
    i5.chip_rate_hz = 10.23e6;
    i5.carrier_hz = 1176.45e6;
    i5.doppler_hz = sat.doppler_hz;
    i5.code_phase_chips = sat.code_phase_chips;
    i5.cn0_dbhz = sat.cn0_dbhz;
    i5.overlay = l5_nh10();
    i5.periods_per_symbol = 10;
    i5.symbols = sat.symbols;

    GnssSatelliteSpec q5 = i5;
    q5.code = l5_code(sat.prn, true);
    q5.overlay = l5_nh20();
    q5.symbols.clear();
    q5.carrier_phase_cycles = 0.25;
    return {std::move(i5), std::move(q5)};
}

}  // namespace revenant::siggen
