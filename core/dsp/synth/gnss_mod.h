// Synthetic GNSS constellations: several spread-spectrum satellites at once,
// each with its own code phase, Doppler, C/N0 and symbol stream, in complex
// white Gaussian noise.
//
// Written from the same interface documents as the decoders, gnss_acq.h has
// the list, so a round trip that fails means one end misread a document.
//
// THE NOISE CONVENTION
//
// Complex noise of unit total variance per sample, so the noise density is
// N0 = 1 / fs and a satellite of carrier-to-noise density C/N0 has amplitude
// sqrt(C/N0 / fs). That is the definition of C/N0 in Kaplan and Hegarty
// section 8.6 and what the tracker's moments estimator reports back.
//
// TIMING
//
// A satellite's code phase at sample n is code_phase_chips + n * chip_rate *
// (1 + doppler / carrier) / fs. The code period it falls in numbers its
// overlay chip and, through periods_per_symbol, its symbol, so symbol and
// overlay edges sit on code-period edges as every one of these signals
// specifies. Symbols past the end of the stream are zero.
//
// Streaming: generate() continues where the previous call stopped, so a
// half-minute constellation never has to exist in memory at once.

#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct GnssSatelliteSpec {
    std::vector<std::int8_t> code;  // +1/-1
    double chip_rate_hz = 1.023e6;
    double carrier_hz = 1575.42e6;
    double offset_hz = 0.0;         // nominal carrier relative to the stream's DC
    double doppler_hz = 0.0;
    double code_phase_chips = 0.0;  // at sample 0
    double carrier_phase_cycles = 0.0;
    double cn0_dbhz = 45.0;
    std::vector<std::int8_t> overlay;  // per code period, cyclic; empty for none
    unsigned periods_per_symbol = 20;
    std::vector<std::uint8_t> symbols;  // 0 or 1; 1 inverts the carrier
};

class GnssSynth {
public:
    [[nodiscard]] static Expected<GnssSynth> create(dsp::SampleRate rate,
                                                    std::vector<GnssSatelliteSpec> satellites,
                                                    std::uint64_t noise_seed,
                                                    bool add_noise = true);

    // Fills `out`, overwriting it, and advances.
    void generate(std::span<dsp::Complex32> out);

    [[nodiscard]] dsp::SampleIndex position() const { return n_; }

private:
    GnssSynth() = default;

    struct State {
        GnssSatelliteSpec spec;
        double amplitude = 0.0;
        double chips_per_sample = 0.0;
        double cycles_per_sample = 0.0;
        double code_len = 0.0;
    };

    double fs_ = 0.0;
    std::vector<State> sats_;
    std::mt19937_64 rng_;
    bool noise_ = true;
    dsp::SampleIndex n_ = 0;
};

// The number of samples that span `seconds`.
[[nodiscard]] std::size_t gnss_samples(dsp::SampleRate rate, double seconds);

}  // namespace revenant::siggen
