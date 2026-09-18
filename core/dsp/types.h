// The vocabulary types every DSP block, tool and test shares.
//
// Two of these are conventions the whole project turns on, and both are stated
// in the handoff document because every SDR project learns them the hard way.
//
// Frequency is integer hertz, always. Floating-point frequency accumulates
// error across mixing stages and surfaces later as a tuning offset nobody can
// source. An int64_t counts every hertz from DC to well past any radio that
// will ever be attached.
//
// Time is a sample index from stream start, always, and it is authoritative.
// Wall clock is derived at the edges. Retroactive tuning needs the index exact
// across hours of capture; a double runs out of integer precision at 2^53
// samples, which sounds like plenty until someone captures at 20 MS/s for a
// few months. A uint64_t does not.

#pragma once

#include <complex>
#include <cstdint>
#include <span>

namespace revenant::dsp {

// Sample position from the start of a stream. Never wraps in any realistic
// deployment: at 100 MS/s this overflows after about 5,800 years.
using SampleIndex = std::uint64_t;

// Absolute frequency in hertz. Signed because offsets from a center frequency
// are frequencies too, and a baseband component at -12 kHz is an ordinary
// thing to name.
using Hertz = std::int64_t;

// Samples per second. Integer for the same reason frequency is.
using SampleRate = std::int64_t;

// std::complex<float> is layout-compatible with float[2] by guarantee, so a
// span of these maps onto a std430 vec2 array on the device with no repacking.
// That guarantee is why the project uses it rather than a hand-rolled struct.
using Complex32 = std::complex<float>;

static_assert(sizeof(Complex32) == 2 * sizeof(float),
              "Complex32 must be two packed floats to alias a std430 vec2 array");

using ComplexSpan = std::span<Complex32>;
using ConstComplexSpan = std::span<const Complex32>;
using RealSpan = std::span<float>;
using ConstRealSpan = std::span<const float>;

// Every block carries this. The index is truth; the anchor converts it to a
// human timescale and is disciplined against GPS or PPS where the hardware
// offers one.
struct BlockTimestamp {
    SampleIndex start = 0;

    // Nanoseconds since the Unix epoch at the moment sample 0 of the stream was
    // captured. Established once at stream start and then corrected, never
    // re-derived per block from a clock read.
    std::int64_t epoch_anchor_ns = 0;

    SampleRate rate = 0;

    [[nodiscard]] constexpr std::int64_t wall_clock_ns() const {
        if (rate <= 0) {
            return epoch_anchor_ns;
        }
        // Split the division rather than multiplying first. start * 1e9
        // overflows 64 bits after about 18 billion samples, which at 20 MS/s is
        // fifteen minutes into a capture. There is no 128-bit integer on MSVC
        // to fall back on, so the whole seconds and the remainder are converted
        // separately. The remainder is below rate by construction, so its
        // product stays in range for any sample rate under 9.2 GS/s.
        const auto rate_u = static_cast<SampleIndex>(rate);
        const auto whole_seconds = static_cast<std::int64_t>(start / rate_u);
        const auto remainder = static_cast<std::int64_t>(start % rate_u);
        return epoch_anchor_ns + whole_seconds * 1'000'000'000 +
               (remainder * 1'000'000'000) / rate;
    }
};

// Convenience for the many places that need a phase increment per sample from
// a frequency. Returned in radians, double so the accumulation in a caller's
// NCO does not start out already lossy.
[[nodiscard]] constexpr double phase_increment(Hertz frequency, SampleRate rate) {
    if (rate == 0) {
        return 0.0;
    }
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    return kTwoPi * static_cast<double>(frequency) / static_cast<double>(rate);
}

}  // namespace revenant::dsp
