// Signals the characteriser tests need and core/dsp/synth does not make.
//
// WHY THIS IS IN THE TEST TREE, WHICH IS NORMALLY THE WRONG PLACE
//
// docs/conventions.md puts the CPU reference twins in the engine and says
// why: a reference that lives in the test tree is a reference nobody ships,
// reviews or keeps current. core/CMakeLists.txt says the same about the
// modulators, which moved out of tools/siggen once the engine could open a
// synthetic scene.
//
// Neither argument reaches these three. A reference twin is shipped because
// the kernel it referees is shipped. A modulator is shipped because the
// engine transmits from it. Nothing here is either: these are stimuli for the
// characteriser, and two of the three are not signals anybody would transmit.
//
//   gaussian_noise is the refusal case. There is no wanted signal in it at
//   all, which is the point.
//
//   pure_tone is an unmodulated carrier. core/dsp/synth's Cw mode keys one
//   with Morse, so it is never actually unmodulated, and the characteriser
//   has to be scored against the degenerate case separately from the keyed
//   one.
//
//   ofdm_signal is the one that would belong in core/dsp/synth if this lane
//   owned that directory, and it does not. The cyclic-prefix estimator needs
//   a multicarrier signal with a known useful-symbol length and a known
//   guard, and no mode in modulators.h produces one. When synth grows an
//   OFDM mode this moves there and this file keeps the other two.
//
// Everything here follows the same contract the modulators do: pure
// functions of their arguments and an explicit seed, no clock read, and
// bit-identical output from the same seed on any machine. Seeds are the
// caller's and the tests print them, per docs/conventions.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/dsp/types.h"

namespace revenant::characterise_test {

using dsp::Complex32;
using dsp::Hertz;
using dsp::SampleRate;

// Circularly symmetric complex Gaussian noise of the given total power, so
// mean(|x|^2) is variance and each quadrature carries half of it.
//
// Box-Muller over raw 64-bit words from std::mt19937_64, not
// std::normal_distribution. The standard does not specify how a distribution
// consumes its engine, so two standard libraries return different samples
// from the same seed, and a seed that does not reproduce across machines
// defeats the reason for printing it. core/dsp/synth/modulators.h makes the
// same choice for the same reason.
[[nodiscard]] std::vector<Complex32> gaussian_noise(std::size_t count,
                                                    double variance,
                                                    std::uint64_t seed);

// An unmodulated complex carrier at a constant amplitude.
//
// The phase comes from siggen::exact_phase, so this is the same integer
// reduction every modulator in core/dsp/synth uses and the tone does not
// drift against one of them over a long buffer.
[[nodiscard]] std::vector<Complex32> pure_tone(std::size_t count,
                                               SampleRate rate,
                                               Hertz frequency,
                                               double amplitude);

struct OfdmSpec {
    // Points in the inverse transform, which is the useful symbol length in
    // samples. Power of two.
    std::size_t useful_samples = 256;

    // Cyclic prefix, in samples. The last prefix_samples of each useful
    // symbol, copied in front of it, which is what makes the symbol
    // correlate with itself at a lag of useful_samples.
    std::size_t prefix_samples = 32;

    // Subcarriers carrying data, centred on DC and excluding DC itself. The
    // rest of the transform is left empty, which is the guard band every
    // real system has.
    std::size_t used_carriers = 100;

    std::size_t symbol_count = 40;

    // QPSK on every used subcarrier. Deterministic from this.
    std::uint64_t seed = 0;
};

// Samples for the whole burst: symbol_count blocks of
// prefix_samples + useful_samples, back to back, with no preamble and no
// framing. Empty when the spec is not renderable, which the caller checks by
// asking for a size it knows.
[[nodiscard]] std::vector<Complex32> ofdm_signal(const OfdmSpec& spec);

// Sum of two buffers, truncated to the shorter. Used to place a signal in
// noise when the noise power was decided by something other than
// siggen::add_awgn, which is every case here that has no wanted signal to
// calibrate against.
[[nodiscard]] std::vector<Complex32> add_buffers(const std::vector<Complex32>& a,
                                                 const std::vector<Complex32>& b);

}  // namespace revenant::characterise_test
