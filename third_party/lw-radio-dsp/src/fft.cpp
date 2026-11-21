// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
#include "fft.h"

// The thread pool would drag in pthreads on Linux and buys nothing at
// waterfall sizes.
#define POCKETFFT_NO_MULTITHREADING
#include "pocketfft/pocketfft_hdronly.h"

#include <algorithm>

namespace lwdsp::detail {

struct Fft::Plan {
    explicit Plan(std::size_t n) : p(n) {}
    pocketfft::detail::pocketfft_c<float> p;
};

Fft::Fft(std::size_t n) : n_(n), plan_(std::make_unique<Plan>(n)) {}
Fft::~Fft() = default;
Fft::Fft(Fft&&) noexcept = default;
Fft& Fft::operator=(Fft&&) noexcept = default;

void Fft::forward(std::vector<cf32>& data) {
    // std::complex<float> is layout compatible with pocketfft's cmplx<float>
    // (both are two packed floats, real first; [complex.numbers] guarantees
    // this for std::complex).
    plan_->p.exec(reinterpret_cast<pocketfft::detail::cmplx<float>*>(data.data()), 1.0f, true);
}

void power_spectrum(Fft& fft, const std::vector<float>& window, std::span<const cf32> iq,
                    std::vector<cf32>& scratch, std::vector<float>& power_out) {
    const std::size_t n = fft.size();
    scratch.assign(n, cf32{});
    const std::size_t take = std::min(n, iq.size());
    const std::size_t pad = n - take;
    const cf32* src = iq.data() + (iq.size() - take);
    double wsum = 0.0;
    for (float w : window) wsum += w;
    for (std::size_t i = 0; i < take; ++i) scratch[pad + i] = src[i] * window[pad + i];
    fft.forward(scratch);
    const float norm = static_cast<float>(1.0 / (wsum * wsum));
    power_out.resize(n);
    const std::size_t half = n / 2;
    for (std::size_t k = 0; k < n; ++k) {
        // Shift so negative frequencies come first.
        const std::size_t src_bin = (k + n - half) % n;
        power_out[k] = std::norm(scratch[src_bin]) * norm;
    }
}

} // namespace lwdsp::detail
