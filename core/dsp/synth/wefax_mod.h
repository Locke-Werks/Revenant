// A radiofax transmitter, for the round trip in tests/decode/test_wefax.cpp.
//
// Written from the same sections of WMO-No. 386 as core/decode/wefax.h: the
// IOC start signal, phasing, the picture lines and the stop signal, as
// continuous-phase FM on the 1900 Hz subcarrier.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/error.h"

namespace revenant::siggen {

struct WefaxModConfig {
    double rate = 48'000.0;
    double amplitude = 0.5;
    double offset_hz = 0.0;
    double pad_seconds = 0.5;
    int lpm = 120;
    int ioc = 576;
    // III 5.2.2 allows 5 to 10 s, 5.2.3 sets phasing at 30 s.
    double start_seconds = 5.0;
    double phasing_seconds = 30.0;
    // III 5.2.3.2: 5 % white and 95 % black when true, half and half when not.
    bool asymmetric_phasing = false;
    // 2300 Hz white, as WMO-No. 386 has it.
    bool white_high = true;
    bool stop = true;
};

// `gray` is rows of wefax_width(ioc) bytes, 0 black and 255 white.
[[nodiscard]] Expected<std::vector<float>> wefax_render(const WefaxModConfig& config,
                                                        std::span<const std::uint8_t> gray);

}  // namespace revenant::siggen
