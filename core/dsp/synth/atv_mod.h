// Analogue television transmitter for the round trip in
// tests/decode/test_atv.cpp: monochrome pictures to composite video with the
// line and field synchronising waveforms of ITU-R BT.470-6 tables 1-1 and
// 1-2 and figure 2, and that composite onto FM or negative AM (table 3 item
// 7, C3F neg.) complex baseband.
//
// Timing comes from core/decode/atv.h's atv_timing(), so the two ends share
// one reading of the document; the decoder's tests check those values
// against the tables separately.
//
// No colour subcarrier, no pre-emphasis on the FM path, and the AM path is
// double sideband rather than vestigial: the envelope a VSB receiver detects
// is the DSB envelope plus a quadrature error this generator does not model.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/atv.h"
#include "core/dsp/types.h"

namespace revenant::siggen {

using dsp::Complex32;

struct AtvPicture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;  // 2 * rows_per_field
    std::vector<std::uint8_t> gray;  // row major
};

// Composite video in BT.470 table 1 units: blanking 0, peak white 100, sync
// at the standard's sync level. `lead_lines` line periods of plain line sync
// come first, then each picture as two fields, first field first.
[[nodiscard]] std::vector<float> atv_composite(decode::AtvStandard standard, dsp::SampleRate rate,
                                               std::span<const AtvPicture> pictures,
                                               double lead_lines = 12.0);

// FM: sync tip at -deviation_hz, peak white at +deviation_hz.
[[nodiscard]] std::vector<Complex32> atv_fm_modulate(std::span<const float> composite,
                                                     decode::AtvStandard standard,
                                                     dsp::SampleRate rate, double deviation_hz);

// Negative AM: sync tip at full carrier, peak white at 12.5 % (table 3 item 8
// gives 10 to 15 % for system M and 10 to 12.5 % for B and G).
[[nodiscard]] std::vector<Complex32> atv_am_modulate(std::span<const float> composite,
                                                     decode::AtvStandard standard);

}  // namespace revenant::siggen
