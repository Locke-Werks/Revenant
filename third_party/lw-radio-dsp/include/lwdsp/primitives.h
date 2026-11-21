// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// Building blocks shared by the modulators, the band and the receiver.
#pragma once

#include "lwdsp/types.h"

#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace lwdsp {

// Portable PRNG. std:: distributions differ between standard libraries, so
// determinism across MSVC, libstdc++ and libc++ needs our own generator and
// our own normal transform.
class Rng {
public:
    Rng() noexcept : Rng(0x9E3779B97F4A7C15ull) {}
    explicit Rng(std::uint64_t seed) noexcept;
    void seed(std::uint64_t s) noexcept;
    std::uint64_t next_u64() noexcept;
    float uniform() noexcept;               // [0, 1)
    float uniform(float lo, float hi) noexcept;
    float gaussian() noexcept;              // zero mean, unit variance
    cf32 complex_gaussian() noexcept;       // unit total power

    // SplitMix64 finaliser over master seed and a domain tag. Each noise
    // stage draws from its own derived stream so adding one stage does not
    // shift another's numbers.
    static std::uint64_t derive_seed(std::uint64_t master, std::uint64_t domain) noexcept;

private:
    // mt19937_64 output is specified bit for bit by the standard; only the
    // distributions are not, so those are done by hand.
    std::mt19937_64 eng_;
    bool have_spare_ = false;
    float spare_ = 0.0f;
};

// Numerically controlled oscillator. Phase is kept in double so long runs at
// fractional frequencies do not drift.
class Nco {
public:
    Nco() = default;
    Nco(double freq_hz, double sample_rate) noexcept;
    void set_frequency(double freq_hz, double sample_rate) noexcept;
    [[nodiscard]] double frequency() const noexcept { return freq_hz_; }
    void reset(double phase = 0.0) noexcept { phase_ = phase; }
    cf32 step() noexcept;
    // out[i] = in[i] * e^{j phase}
    void mix(std::span<const cf32> in, std::span<cf32> out) noexcept;

private:
    double freq_hz_ = 0.0;
    double inc_ = 0.0;
    double phase_ = 0.0;
};

// Windowed-sinc design helpers. Taps are normalized for unity DC gain.
std::vector<float> design_lowpass(int taps, double cutoff_hz, double sample_rate);
// Complex bandpass: lowpass shifted so the passband is [lo_hz, hi_hz], which
// may be asymmetric about zero (that is how USB and LSB passbands are built).
std::vector<cf32> design_complex_bandpass(int taps, double lo_hz, double hi_hz, double sample_rate);
// Type III Hilbert transformer (odd length, antisymmetric).
std::vector<float> design_hilbert(int taps);

enum class Window : std::uint8_t { Rectangular, Hann, BlackmanHarris, Kaiser };
std::vector<float> make_window(Window w, int n, float kaiser_beta = 8.6f);

// Streaming FIR with history. Real taps on complex data, complex taps on
// complex data, and real taps on real data.
template <typename Tap, typename Sample>
class Fir {
public:
    Fir() = default;
    explicit Fir(std::vector<Tap> taps);
    void set_taps(std::vector<Tap> taps);
    void reset();
    Sample push(Sample x) noexcept;
    // Decimators push every sample but only need the dot product on the
    // samples they keep.
    void shift_in(Sample x) noexcept;
    [[nodiscard]] Sample output() const noexcept;
    void process(std::span<const Sample> in, std::span<Sample> out) noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return taps_.size(); }

private:
    std::vector<Tap> taps_;
    std::vector<Sample> hist_; // doubled ring so the dot product is contiguous
    std::size_t pos_ = 0;
};

using FirRR = Fir<float, float>;
using FirRC = Fir<float, cf32>;
using FirCC = Fir<cf32, cf32>;

// RBJ biquad, direct form II transposed, double state.
class Biquad {
public:
    static Biquad notch(double f0_hz, double q, double sample_rate);
    static Biquad lowpass(double f0_hz, double q, double sample_rate);
    static Biquad highpass(double f0_hz, double q, double sample_rate);
    static Biquad dc_block(double corner_hz, double sample_rate);
    // Coefficients already normalized so a0 == 1.
    static Biquad from_coefficients(double b0, double b1, double b2, double a1, double a2);
    float process(float x) noexcept;
    void reset() noexcept { z1_ = z2_ = 0.0; }

private:
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double z1_ = 0, z2_ = 0;
};

// Flush denormals to zero on the calling thread for the guard's lifetime.
// Long decaying IIR tails otherwise fall into denormal range and the audio
// thread stalls.
class ScopedDenormalFlush {
public:
    ScopedDenormalFlush() noexcept;
    ~ScopedDenormalFlush();
    ScopedDenormalFlush(const ScopedDenormalFlush&) = delete;
    ScopedDenormalFlush& operator=(const ScopedDenormalFlush&) = delete;

private:
    unsigned long long saved_ = 0;
    bool restore_ = false;
};

// True when the calling thread is currently flushing denormals.
[[nodiscard]] bool denormals_are_flushed() noexcept;

} // namespace lwdsp
