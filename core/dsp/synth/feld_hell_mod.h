// Transmitter for Feld-Hell columns, the counterpart of
// core/decode/feld_hell.h and written from the same page: 14 half-pixel
// slots a column at 245 a second, bottom to top, on-off keyed, each run of
// key-down slots given raised-cosine edges one slot long,
// per its "Dot Shaping". No font: the caller supplies the columns.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/feld_hell.h"
#include "core/error.h"

namespace revenant::siggen {

struct FeldHellModConfig {
    dsp::SampleRate rate = 8000;
    double carrier_hz = 1000.0;
    double amplitude = 0.5;
    double lead_in_samples = 0.0;
    double tail_samples = 0.0;
};

// Bit i of each column is slot i from the bottom.
[[nodiscard]] Expected<std::vector<float>> feld_hell_render(const FeldHellModConfig& config,
                                                            std::span<const std::uint16_t> columns);

}  // namespace revenant::siggen
