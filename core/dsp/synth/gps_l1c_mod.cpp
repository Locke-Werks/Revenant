// GPS L1C. See gps_l1c_mod.h.

#include "core/dsp/synth/gps_l1c_mod.h"

#include <cmath>

namespace revenant::siggen {

using namespace revenant::decode::gnss;

std::vector<std::uint8_t> l1c_frame_symbols(unsigned toi, std::span<const std::uint8_t> sf2,
                                            std::span<const std::uint8_t> sf3) {
    auto with_crc = [](std::span<const std::uint8_t> data) {
        std::vector<std::uint8_t> v(data.begin(), data.end());
        const std::uint32_t crc = crc24q(v);
        for (int i = 23; i >= 0; --i) {
            v.push_back(static_cast<std::uint8_t>((crc >> i) & 1U));
        }
        return v;
    };
    auto coded = l1c_ldpc_encode(2, with_crc(sf2));
    const auto c3 = l1c_ldpc_encode(3, with_crc(sf3));
    coded.insert(coded.end(), c3.begin(), c3.end());
    const auto inter = l1c_interleave(coded);
    const auto t = l1c_toi_encode(toi);
    std::vector<std::uint8_t> out(t.begin(), t.end());
    out.insert(out.end(), inter.begin(), inter.end());
    return out;
}

std::vector<GnssSatelliteSpec> l1c_satellite(const L1cSatellite& sat) {
    std::vector<GnssSatelliteSpec> out;
    GnssSatelliteSpec ca;
    ca.code = gps_ca_code(sat.prn);
    ca.doppler_hz = sat.doppler_hz;
    ca.code_phase_chips = sat.ca_code_phase_chips;
    ca.cn0_dbhz = sat.ca_cn0_dbhz;
    out.push_back(ca);

    // Half-chips of L1C at sample 0: the same instant as C/A, counted from
    // an L1C epoch `epoch_offset` C/A periods before it is due, so a
    // receiver sees the L1C epoch that many C/A epochs after the first.
    const double c = sat.ca_code_phase_chips;
    const double l1c_chips =
        sat.epoch_offset == 0 ? c : c + 1023.0 * static_cast<double>(10 - sat.epoch_offset);
    const unsigned lead = sat.lead_periods % 1800;

    GnssSatelliteSpec pilot;
    pilot.code = boc11(l1c_code(sat.prn, true));
    pilot.chip_rate_hz = 2.046e6;
    pilot.doppler_hz = sat.doppler_hz;
    pilot.code_phase_chips = 2.0 * l1c_chips;
    pilot.cn0_dbhz = sat.ca_cn0_dbhz + 0.25;
    const auto ov = l1c_overlay(sat.prn);
    pilot.overlay.resize(1800);
    for (std::size_t q = 0; q < 1800; ++q) {
        pilot.overlay[q] = ov[(q + 1800 - lead) % 1800];
    }
    pilot.periods_per_symbol = 1;

    GnssSatelliteSpec data = pilot;
    data.code = boc11(l1c_code(sat.prn, false));
    data.overlay.clear();
    data.cn0_dbhz = sat.ca_cn0_dbhz - 4.5;
    if (!sat.frames.empty()) {
        const auto& f0 = sat.frames.front();
        data.symbols.assign(f0.end() - lead, f0.end());
        for (std::size_t i = 1; i < sat.frames.size(); ++i) {
            data.symbols.insert(data.symbols.end(), sat.frames[i].begin(), sat.frames[i].end());
        }
    }
    out.push_back(std::move(pilot));
    out.push_back(std::move(data));
    return out;
}

}  // namespace revenant::siggen
