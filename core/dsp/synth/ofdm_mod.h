// A generic OFDM transmitter, the counterpart of core/decode/ofdm.h, for
// round-trip tests of that receiver and of the mode decoders built on it.
//
// Each symbol is the inverse FFT of its carrier grid, scaled to be unitary,
// with the last cp_length samples copied in front of it. Pilots from the
// configuration's table overwrite whatever the caller put on their carriers.
// Nothing is filtered: the spectrum has the sinc sidelobes of rectangular
// symbols, which a receiver for real signals has to tolerate anyway.

#pragma once

#include <cstddef>
#include <vector>

#include "core/decode/ofdm.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;

struct OfdmTransmission {
    std::vector<Complex32> samples;
    // grid[s][c] is what was sent on config.carriers[c] in symbol s, pilots
    // included.
    std::vector<std::vector<Complex32>> grid;
};

// grid[s] holds one value per config.carriers entry. Symbol s uses pilot
// table entry (first_pilot_phase + s) % table size.
[[nodiscard]] Expected<OfdmTransmission> ofdm_modulate(
    const decode::OfdmConfig& config,
    const std::vector<std::vector<Complex32>>& grid,
    std::size_t first_pilot_phase = 0);

}  // namespace revenant::siggen
