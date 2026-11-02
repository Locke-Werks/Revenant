// An SSTV transmitter, for the round trip in tests/decode/test_sstv.cpp.
//
// Written from the same sections of the Dayton paper as core/decode/sstv.h,
// and from the same mode table, so a failing round trip means one end misread
// the paper. Continuous-phase FM, every tone and pixel placed at its exact
// time in the paper's milliseconds.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/sstv.h"
#include "core/error.h"

namespace revenant::siggen {

struct SstvModConfig {
    // Samples per second the audio is rendered at. A test models a sample
    // clock error by rendering at one rate and decoding at another.
    double rate = 48'000.0;
    double amplitude = 0.5;
    // Added to every frequency, a mistuned receiver.
    double offset_hz = 0.0;
    // Silence before and after.
    double pad_seconds = 0.5;
    // The calibration header and VIS code before the image.
    bool vis = true;
};

// `rgb` is width * height * 3 bytes, R G B, rows top to bottom. Gray modes
// read the green byte.
[[nodiscard]] Expected<std::vector<float>> sstv_render(const SstvModConfig& config,
                                                       const decode::SstvMode& mode,
                                                       std::span<const std::uint8_t> rgb);

}  // namespace revenant::siggen
