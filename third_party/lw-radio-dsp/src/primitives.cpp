// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// Rng seeding, the 53-bit uniform, Box-Muller and the Kaiser/Bessel helpers
// are ported from Revenant core/dsp/synth/channel.cpp. ScopedDenormalFlush is
// ported from Revenant core/dsp/denormal_mode.cpp, with the AArch64 branch
// added here.
#include "lwdsp/primitives.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define LWDSP_X86 1
#include <pmmintrin.h>
#include <xmmintrin.h>
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define LWDSP_A64 1
#endif

namespace lwdsp {

namespace {

constexpr double kPiD = std::numbers::pi;

double bessel_i0(double x) {
    const double q = 0.25 * x * x;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k < 64; ++k) {
        const double d = static_cast<double>(k);
        term *= q / (d * d);
        sum += term;
        if (term < sum * 1.0e-17) break;
    }
    return sum;
}

double sinc(double x) {
    if (std::abs(x) < 1.0e-12) return 1.0;
    const double s = kPiD * x;
    return std::sin(s) / s;
}

} // namespace

// ---------------------------------------------------------------------------
// Rng

std::uint64_t Rng::derive_seed(std::uint64_t master, std::uint64_t domain) noexcept {
    std::uint64_t s = master + domain * 0x9E3779B97F4A7C15ULL;
    s = (s ^ (s >> 30)) * 0xBF58476D1CE4E5B9ULL;
    s = (s ^ (s >> 27)) * 0x94D049BB133111EBULL;
    return s ^ (s >> 31);
}

Rng::Rng(std::uint64_t s) noexcept { seed(s); }

void Rng::seed(std::uint64_t s) noexcept {
    eng_.seed(s);
    have_spare_ = false;
    spare_ = 0.0f;
}

std::uint64_t Rng::next_u64() noexcept { return eng_(); }

float Rng::uniform() noexcept {
    // Top 24 bits so the result is exactly representable and never 1.0.
    return static_cast<float>(eng_() >> 40) * (1.0f / 16777216.0f);
}

float Rng::uniform(float lo, float hi) noexcept { return lo + (hi - lo) * uniform(); }

float Rng::gaussian() noexcept {
    if (have_spare_) {
        have_spare_ = false;
        return spare_;
    }
    // Uniform in (0, 1] from the top 53 bits, so log never sees zero.
    constexpr double kInv53 = 1.0 / 9007199254740992.0;
    const double u1 = (static_cast<double>(eng_() >> 11) + 1.0) * kInv53;
    const double u2 = (static_cast<double>(eng_() >> 11) + 1.0) * kInv53;
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double a = 2.0 * kPiD * u2;
    spare_ = static_cast<float>(r * std::sin(a));
    have_spare_ = true;
    return static_cast<float>(r * std::cos(a));
}

cf32 Rng::complex_gaussian() noexcept {
    constexpr float k = 0.70710678118654752f;
    const float re = gaussian();
    const float im = gaussian();
    return {re * k, im * k};
}

// ---------------------------------------------------------------------------
// Nco

Nco::Nco(double f, double fs) noexcept { set_frequency(f, fs); }

void Nco::set_frequency(double f, double fs) noexcept {
    freq_hz_ = f;
    inc_ = fs > 0.0 ? f / fs : 0.0;
}

cf32 Nco::step() noexcept {
    const double a = 2.0 * kPiD * phase_;
    const cf32 v{static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    phase_ += inc_;
    phase_ -= std::floor(phase_);
    return v;
}

void Nco::mix(std::span<const cf32> in, std::span<cf32> out) noexcept {
    const std::size_t n = std::min(in.size(), out.size());
    for (std::size_t i = 0; i < n; ++i) out[i] = in[i] * step();
}

// ---------------------------------------------------------------------------
// Filter design

std::vector<float> make_window(Window w, int n, float beta) {
    std::vector<float> out(static_cast<std::size_t>(std::max(n, 0)), 1.0f);
    if (n <= 1) return out;
    const double m = static_cast<double>(n - 1);
    const double i0b = bessel_i0(beta);
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) / m;
        double v = 1.0;
        switch (w) {
        case Window::Rectangular: v = 1.0; break;
        case Window::Hann: v = 0.5 - 0.5 * std::cos(2.0 * kPiD * x); break;
        case Window::BlackmanHarris:
            v = 0.35875 - 0.48829 * std::cos(2.0 * kPiD * x) + 0.14128 * std::cos(4.0 * kPiD * x) -
                0.01168 * std::cos(6.0 * kPiD * x);
            break;
        case Window::Kaiser: {
            const double t = 2.0 * x - 1.0;
            v = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - t * t))) / i0b;
            break;
        }
        }
        out[static_cast<std::size_t>(i)] = static_cast<float>(v);
    }
    return out;
}

namespace {
// Kaiser beta 6 is about 60 dB stopband, which is below what the noise
// floor shows on a waterfall and keeps the transition narrow.
constexpr float kDesignBeta = 6.0f;

std::vector<double> lowpass_d(int taps, double cutoff_hz, double fs) {
    taps = std::max(taps | 1, 3); // odd so the delay is a whole sample
    const auto win = make_window(Window::Kaiser, taps, kDesignBeta);
    const double fc = cutoff_hz / fs;
    const double mid = 0.5 * static_cast<double>(taps - 1);
    std::vector<double> h(static_cast<std::size_t>(taps));
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
        const double t = static_cast<double>(i) - mid;
        h[static_cast<std::size_t>(i)] = 2.0 * fc * sinc(2.0 * fc * t) * win[static_cast<std::size_t>(i)];
        sum += h[static_cast<std::size_t>(i)];
    }
    for (double& v : h) v /= sum;
    return h;
}
} // namespace

std::vector<float> design_lowpass(int taps, double cutoff_hz, double fs) {
    const auto h = lowpass_d(taps, cutoff_hz, fs);
    std::vector<float> out(h.size());
    std::transform(h.begin(), h.end(), out.begin(), [](double v) { return static_cast<float>(v); });
    return out;
}

std::vector<cf32> design_complex_bandpass(int taps, double lo_hz, double hi_hz, double fs) {
    if (hi_hz < lo_hz) std::swap(lo_hz, hi_hz);
    const double centre = 0.5 * (lo_hz + hi_hz);
    const double half = std::max(0.5 * (hi_hz - lo_hz), 1.0);
    const auto h = lowpass_d(taps, half, fs);
    const double mid = 0.5 * static_cast<double>(h.size() - 1);
    std::vector<cf32> out(h.size());
    for (std::size_t i = 0; i < h.size(); ++i) {
        const double a = 2.0 * kPiD * centre * (static_cast<double>(i) - mid) / fs;
        out[i] = cf32{static_cast<float>(h[i] * std::cos(a)), static_cast<float>(h[i] * std::sin(a))};
    }
    return out;
}

std::vector<float> design_hilbert(int taps) {
    taps = std::max(taps | 1, 3);
    const auto win = make_window(Window::BlackmanHarris, taps);
    const int mid = (taps - 1) / 2;
    std::vector<float> h(static_cast<std::size_t>(taps), 0.0f);
    for (int i = 0; i < taps; ++i) {
        const int k = i - mid;
        if (k % 2 != 0) {
            h[static_cast<std::size_t>(i)] =
                static_cast<float>(2.0 / (kPiD * k) * win[static_cast<std::size_t>(i)]);
        }
    }
    return h;
}

// ---------------------------------------------------------------------------
// Fir

template <typename T, typename S> Fir<T, S>::Fir(std::vector<T> t) { set_taps(std::move(t)); }

template <typename T, typename S> void Fir<T, S>::set_taps(std::vector<T> t) {
    taps_ = std::move(t);
    reset();
}

template <typename T, typename S> void Fir<T, S>::reset() {
    hist_.assign(taps_.size() * 2, S{});
    pos_ = 0;
}

template <typename T, typename S> void Fir<T, S>::shift_in(S x) noexcept {
    const std::size_t n = taps_.size();
    if (n == 0) return;
    pos_ = (pos_ + 1 == n) ? 0 : pos_ + 1;
    hist_[pos_] = x;
    hist_[pos_ + n] = x;
}

template <typename T, typename S> S Fir<T, S>::output() const noexcept {
    const std::size_t n = taps_.size();
    if (n == 0) return S{};
    // hist_[pos_ + n - k] is the sample k steps old, contiguous thanks to
    // the doubled buffer.
    const S* h = hist_.data() + pos_ + n;
    S acc{};
    for (std::size_t k = 0; k < n; ++k) acc += taps_[k] * *(h - k);
    return acc;
}

template <typename T, typename S> S Fir<T, S>::push(S x) noexcept {
    if (taps_.empty()) return x;
    shift_in(x);
    return output();
}

template <typename T, typename S>
void Fir<T, S>::process(std::span<const S> in, std::span<S> out) noexcept {
    const std::size_t n = std::min(in.size(), out.size());
    for (std::size_t i = 0; i < n; ++i) out[i] = push(in[i]);
}

template class Fir<float, float>;
template class Fir<float, cf32>;
template class Fir<cf32, cf32>;

// ---------------------------------------------------------------------------
// Biquad (RBJ audio EQ cookbook)

namespace {
Biquad make_biquad(double b0, double b1, double b2, double a0, double a1, double a2);
}

Biquad Biquad::notch(double f0, double q, double fs) {
    const double w = 2.0 * kPiD * f0 / fs;
    const double alpha = std::sin(w) / (2.0 * q);
    const double c = std::cos(w);
    return make_biquad(1.0, -2.0 * c, 1.0, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
}

Biquad Biquad::lowpass(double f0, double q, double fs) {
    const double w = 2.0 * kPiD * f0 / fs;
    const double alpha = std::sin(w) / (2.0 * q);
    const double c = std::cos(w);
    return make_biquad((1.0 - c) / 2.0, 1.0 - c, (1.0 - c) / 2.0, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
}

Biquad Biquad::highpass(double f0, double q, double fs) {
    const double w = 2.0 * kPiD * f0 / fs;
    const double alpha = std::sin(w) / (2.0 * q);
    const double c = std::cos(w);
    return make_biquad((1.0 + c) / 2.0, -(1.0 + c), (1.0 + c) / 2.0, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
}

Biquad Biquad::dc_block(double corner, double fs) {
    // First-order section expressed in biquad form.
    const double r = std::exp(-2.0 * kPiD * corner / fs);
    return make_biquad(1.0, -1.0, 0.0, 1.0, -r, 0.0);
}

float Biquad::process(float x) noexcept {
    const double in = x;
    const double y = b0_ * in + z1_;
    z1_ = b1_ * in - a1_ * y + z2_;
    z2_ = b2_ * in - a2_ * y;
    return static_cast<float>(y);
}

Biquad Biquad::from_coefficients(double b0, double b1, double b2, double a1, double a2) {
    Biquad q;
    q.b0_ = b0;
    q.b1_ = b1;
    q.b2_ = b2;
    q.a1_ = a1;
    q.a2_ = a2;
    return q;
}

namespace {
Biquad make_biquad(double b0, double b1, double b2, double a0, double a1, double a2) {
    return Biquad::from_coefficients(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
}
} // namespace

// ---------------------------------------------------------------------------
// Denormals

ScopedDenormalFlush::ScopedDenormalFlush() noexcept {
#if defined(LWDSP_X86)
    // Flush-to-zero covers denormal results, denormals-are-zero covers
    // denormal inputs; both are needed.
    saved_ = _mm_getcsr();
    restore_ = true;
    _mm_setcsr(static_cast<unsigned>(saved_) | 0x8040u);
#elif defined(LWDSP_A64)
    unsigned long long fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    saved_ = fpcr;
    restore_ = true;
    fpcr |= (1ull << 24); // FZ
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
}

ScopedDenormalFlush::~ScopedDenormalFlush() {
    if (!restore_) return;
#if defined(LWDSP_X86)
    _mm_setcsr(static_cast<unsigned>(saved_));
#elif defined(LWDSP_A64)
    const unsigned long long v = saved_;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
#endif
}

bool denormals_are_flushed() noexcept {
#if defined(LWDSP_X86)
    return (_mm_getcsr() & 0x8040u) == 0x8040u;
#elif defined(LWDSP_A64)
    unsigned long long fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    return (fpcr >> 24) & 1u;
#else
    return false;
#endif
}

} // namespace lwdsp
