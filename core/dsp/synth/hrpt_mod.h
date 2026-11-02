// NOAA HRPT transmitter for tests: five AVHRR channel images to minor frames,
// to split-phase phase-modulated complex baseband.
//
// Written from the same NOAA KLM User's Guide section 4.1 as
// core/decode/hrpt.h. Words the tests do not look at (telemetry, TIP, spare,
// auxiliary sync) are left zero; the decoder does not depend on them.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/decode/hrpt.h"
#include "core/dsp/types.h"

namespace revenant::siggen {

struct HrptImage {
    std::size_t rows = 0;
    // rows x 2048 ten-bit samples per channel.
    std::array<std::vector<std::uint16_t>, decode::kHrptChannels> channels;
    std::uint8_t spacecraft = 7;
    std::uint16_t day = 123;
    std::uint32_t first_msec = 43'200'000;
};

using HrptFrameWords = std::array<std::uint16_t, decode::kHrptFrameWords>;

[[nodiscard]] std::vector<HrptFrameWords> hrpt_format(const HrptImage& image);

struct HrptBasebandSpec {
    double sample_rate = 3.0e6;
    double phase_deg = decode::kHrptPhaseDegKlm;
    double carrier_hz = 0.0;
    double drift_hz_per_s = 0.0;
    double clock_ppm = 0.0;
    double lead_seconds = 0.02;
};

[[nodiscard]] std::vector<dsp::Complex32> hrpt_baseband(const std::vector<HrptFrameWords>& frames,
                                                        const HrptBasebandSpec& spec);

}  // namespace revenant::siggen
