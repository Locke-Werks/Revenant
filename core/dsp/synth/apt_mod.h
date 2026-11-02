// NOAA APT transmitter for tests: two channel images to formatted lines, to
// 2400 Hz AM audio, to FM complex baseband.
//
// Written from the same NOAA KLM User's Guide section 4.2 as
// core/decode/apt.h; see that header for the citations. Where the document is
// silent this file makes a choice and says so, and the decoder is written not
// to depend on the choice:
//
//   - grey wedge n of 1..8 is round(255 n / 8) and wedge 9 is 0;
//   - the AM envelope is 1 + m (2 v / 255 - 1) for word value v, m = 0.87,
//     so wedge 8 is the maximum modulation of 4.2.2;
//   - space data is 0 on channel A and 255 on channel B, minute markers are
//     lines 0 and 1 black and 2 and 3 white of every 120.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/decode/apt.h"
#include "core/dsp/types.h"

namespace revenant::siggen {

struct AptImage {
    // rows x 909 each, row major.
    std::vector<std::uint8_t> channel_a;
    std::vector<std::uint8_t> channel_b;
    std::size_t rows = 0;
    // Grey wedge 1..6 that wedge 16 repeats, 4.2.3: 2 is AVHRR channel 2,
    // 4 is channel 4.
    int channel_a_wedge = 2;
    int channel_b_wedge = 4;
    // Line number of the first row within the 128-line telemetry frame and
    // the 120-line minute.
    std::size_t first_line = 0;
};

using AptLineWords = std::array<std::uint8_t, decode::kAptLineWords>;

[[nodiscard]] std::vector<AptLineWords> apt_format(const AptImage& image);

struct AptAudioSpec {
    double sample_rate = 11025.0;
    double modulation_index = 0.87;
    // Word clock error, parts per million; positive runs the lines long.
    double clock_ppm = 0.0;
    // Silence before the first line, seconds.
    double lead_seconds = 0.3;
    double amplitude = 0.5;
};

[[nodiscard]] std::vector<float> apt_audio(const std::vector<AptLineWords>& lines,
                                           const AptAudioSpec& spec);

struct AptFmSpec {
    double sample_rate = 44100.0;
    double deviation_hz = 17000.0;  // docs/modes.md, about 17 kHz
    double doppler_start_hz = 0.0;
    double doppler_rate_hz_per_s = 0.0;
};

// audio must be at spec.sample_rate.
[[nodiscard]] std::vector<dsp::Complex32> apt_fm(const std::vector<float>& audio,
                                                 const AptFmSpec& spec);

}  // namespace revenant::siggen
